#include "math.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstdlib>
#include <cstring>

#include <vector>
#include <list>
#include <set>
#include <map>
#include <string>
#include <algorithm>
#include <thread>
#include <future>
#include <optional>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdint>

#include <nlohmann/json.hpp>
#include "spdlog/spdlog.h"

#include "util.h"
#include "config.h"
#include "site.h"

#include "indexer.capnp.h"

#include <kj/debug.h>
#include <kj/string.h>
#include <kj/async-io.h>
#include <kj/async-unix.h>

#include <capnp/rpc.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/ez-rpc.h>
#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <iostream>

using nlohmann::json;

class ScorerWorkerImpl final: public ScorerWorker::Server,
                              public kj::TaskSet::ErrorHandler {

  struct node {
    // This doesn't support duplicate links. I don't think they
    // help much. Could be wrong.

    std::string url;
    bool is_seed{false};
    std::vector<uint32_t> links;

    uint32_t extra{0};
    uint32_t next_coupons{0};

    uint32_t coupons{0};
    uint32_t new_counter{0};

    uint32_t counter{0};

    node(const std::string &url, uint32_t counter, std::vector<uint32_t> links = {})
      : url(url), counter(counter), links(links)
    {}

    void setup(uint32_t K, uint32_t bias) {
      coupons = K;
      new_counter = bias;
      extra = bias;

      next_coupons = 0;
    }

    void add(uint32_t hits) {
      next_coupons += hits;
    }

    bool iterate_finish() {
      next_coupons += extra;

      new_counter += next_coupons;
      coupons = next_coupons;

      next_coupons = 0;

      return coupons > 0;
    }

    void finish() {
      counter = new_counter;
    }

    void iterate(ScorerWorkerImpl &worker) {
      if (links.empty()) {
        return;
      }

      std::vector<uint32_t> hits(links.size(), 0);

      size_t total_hits = 0;

      for (size_t i = 0; i < coupons; i++) {
        float p_stop = util::get_rand();
        if (p_stop > 1.0 - worker.param_e) {
          spdlog::info("stop as {} > 1.0 - {}", p_stop, worker.param_e);
          break;
        }

        size_t l = util::get_rand() * hits.size();
        hits[l]++;
        
        total_hits++;
      }

      for (size_t i = 0; i < links.size(); i++) {
        if (hits[i] > 0) {
          worker.addWalk(links[i], hits[i]);
        }
      }

      spdlog::debug("{:6} : {:4} : {}", new_counter, total_hits, url);
    }
  };

  struct adaptor {
    adaptor(kj::PromiseFulfiller<void> &fulfiller,
            ScorerWorkerImpl &worker,
            std::vector<node*> nodes)
      : fulfiller(fulfiller),
        worker(worker),
        nodes(nodes)
    {
      process();
    }

    void process() {
      while (!nodes.empty()) {
        if (worker.sendingWalks) {
          worker.addPendingWalk(this);
          return;
        }

        auto node = nodes.back();
        nodes.pop_back();

        node->iterate(worker);
      }

      spdlog::debug("iterate step finished");
      fulfiller.fulfill();
    }

    kj::PromiseFulfiller<void> &fulfiller;
    ScorerWorkerImpl &worker;
    std::vector<node*> nodes;
  };

public:
  ScorerWorkerImpl(kj::AsyncIoContext &io_context, const config &s, Scorer::Client master)
    : settings(s), tasks(*this), timer(io_context.provider->getTimer()),
      master(master)
  {
  }

  kj::Promise<void> getCounter(GetCounterContext context) override {
    auto params = context.getParams();
    std::string url = params.getUrl();

    uint32_t id = urlToId(url, false);
    if (id != 0) {
      auto it = nodes.find(id);
      if (it != nodes.end()) {
        spdlog::info("get score {} = {}", url, it->second.counter);
        context.getResults().setCounter(it->second.counter);
      }
    }

    return kj::READY_NOW;
  }

  kj::Promise<void> addSite(AddSiteContext context) override {
    spdlog::info("add site");

    auto sitePath = context.getParams().getSitePath();

    site_map site(sitePath);

    site.load();

    size_t c = 0;

    for (auto p: site.pages) {
      if (p.last_scanned > 0) {
        spdlog::info("add page {}", p.url);

        std::vector<uint32_t> links;
        for (auto &l: p.links) {
          links.emplace_back(urlToId(l.first, true));
        }

        auto id = urlToId(p.url, true);
        auto it = nodes.try_emplace(id, p.url, 0, links);

        if (!it.second) {
          it.first->second.links = links;
        }

        c++;
      }
    }

    context.getResults().setPageCount(c);

    return kj::READY_NOW;
  }

  kj::Promise<void> setSeed(SetSeedContext context) override {
    spdlog::info("set seed");

    auto url = context.getParams().getUrl();

    uint32_t id = urlToId(url, false);
    if (id != 0) {
      auto it = nodes.find(id);
      if (it != nodes.end()) {
        it->second.is_seed = true;
      }
    }

    return kj::READY_NOW;
  }


  kj::Promise<void> setup(SetupContext context) override {
    auto params = context.getParams();

    uint32_t K = params.getK();
    param_e = params.getE();
    uint32_t bias = params.getBias();

    output_path = params.getPath();

    spdlog::info("setup for scoring {}, {} -> {}", K, param_e, output_path);

    for (auto &n: nodes) {
      n.second.setup(K, n.second.is_seed ? bias : 0);
    }

    return kj::READY_NOW;
  }

  kj::Promise<void> save(SaveContext context) override {
   spdlog::debug("load {}", output_path);

    int fd = open(output_path.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0664);
    if (fd < 0) {
      spdlog::warn("failed to open {}", output_path);
      return kj::READY_NOW;
    }

    ::capnp::MallocMessageBuilder message;

    ScoreBlock::Builder b = message.initRoot<ScoreBlock>();

    auto n_nodes = b.initNodes(nodes.size());
    size_t i = 0;
    for (auto n: nodes) {
      auto n_node = n_nodes[i++];
    
      n_node.setUrl(n.second.url);
      n_node.setCounter(n.second.counter);
    }

    writePackedMessageToFd(fd, message);

    close(fd);
 
    return kj::READY_NOW;
  }

  kj::Promise<void> load(LoadContext context) override {
    nodes.clear();

    auto params = context.getParams();
    
    output_path = params.getPath();

    spdlog::debug("load {}", output_path);

    int fd = open(output_path.c_str(), O_RDONLY);
    if (fd < 0) {
      spdlog::warn("failed to open {}", output_path);
      return kj::READY_NOW;
    }

    ::capnp::PackedFdMessageReader message(fd);

    ScoreBlock::Reader reader = message.getRoot<ScoreBlock>();

    for (auto n: reader.getNodes()) {
      std::string url = n.getUrl();
      uint32_t counter = n.getCounter();

      nodes.emplace(urlToId(url, true), node(url, counter));
    }

    // Or earlier?
    close(fd);

    return kj::READY_NOW;
  }

  kj::Promise<void> iterate(IterateContext) override {
    std::vector<node *> l_nodes;

    spdlog::debug("iterate setup");
    
    for (auto &node: nodes) {
      l_nodes.push_back(&node.second);
    }
    
    spdlog::debug("iterate start");

    return kj::newAdaptedPromise<void, adaptor>(*this, l_nodes);
  }

  kj::Promise<void> iterateFinish(IterateFinishContext context) override {
    size_t active = 0;

    for (auto &node: nodes) {
      active += node.second.iterate_finish() ? 1 : 0;
    }

    spdlog::info("iterate finished with {} active for next", active);

    context.getResults().setRunning(active > 0);

    return kj::READY_NOW;
  }

  void addWalk(uint32_t id, uint32_t hits) {
    auto it = nodes.find(id);
    if (it != nodes.end()) {
      it->second.add(hits);

    } else {
      auto w = walks.try_emplace(id, hits);
      if (!w.second) {
        w.first->second += hits;

      } else {
        if (walks.size() > 10000) {
          sendingWalks = true;
          sendWalks();
        }
      }
    }
  }

  void sendWalks() {
    spdlog::info("sending {} walks", walks.size());

    auto request = master.addWalksRequest();

    auto newWalks = request.initWalks(walks.size());
    size_t i = 0;
    for (auto &walk: walks) {
      auto murl = urlFromId(walk.first);
      if (murl == nullptr) {
        continue;
      }

      auto &url = *murl;
      
      auto newWalk = newWalks[i++];

      newWalk.setUrl(url);
      newWalk.setHits(walk.second);
    }

    walks.clear();

    tasks.add(request.send().then(
          [this] (auto result) {
            spdlog::info("got remote walk response");
            sendingWalks = false;
            bumpPendingWalk();
          }));
  }

  kj::Promise<void> addWalks(AddWalksContext context) override {
    spdlog::info("got remote walk request");
    auto params = context.getParams();

    for (auto walk: params.getWalks()) {
      std::string url = walk.getUrl();
      auto hits = walk.getHits();

      uint32_t id = urlToId(url, false);
      if (id != 0) {
        auto it = nodes.find(id);
        if (it != nodes.end()) {
          it->second.add(hits);
        }
      }
    }

    return kj::READY_NOW;
  }

  void addPendingWalk(adaptor *a) {
    if (adaptors_pending_walks.size() > 0) {
      spdlog::warn("pending walk added twice?");
      return;
    }

    adaptors_pending_walks.push_back(a);
  }

  void bumpPendingWalk() {
    for (auto a: adaptors_pending_walks) {
      tasks.add(kj::Promise<void>(kj::READY_NOW).then(
          [a] () {
            a->process();
          }
        ));
    }

    adaptors_pending_walks.clear();
  }

  uint32_t urlToId(const std::string &u, bool make = false) {
    auto it = urls_to_id.find(u);
    if (it == urls_to_id.end()) {
      if (!make) {
        return 0;
      }

      uint32_t id = ++next_url_id;

      urls_to_id.emplace(u, id);
      urls.emplace(id, u);

      return id;

    } else {
      return it->second;
    }
  }

  const std::string * urlFromId(uint32_t id) {
    auto it = urls.find(id);
    if (it != urls.end()) {
      return &it->second;
    } else {
      return nullptr;
    }
  }

  void taskFailed(kj::Exception&& exception) override {
    spdlog::warn("task failed: {}", std::string(exception.getDescription()));
    kj::throwFatalException(kj::mv(exception));
  }

  const config &settings;

  Scorer::Client master;

  kj::TaskSet tasks;
  kj::Timer &timer;

  float param_e;

  std::string output_path;

  std::vector<std::string> site_paths;
  std::unordered_map<uint32_t, node> nodes;

  std::unordered_map<uint32_t, std::string> urls;
  std::unordered_map<std::string, uint32_t> urls_to_id;
  uint32_t next_url_id{0};

  std::vector<adaptor *> adaptors_pending_walks;

  std::map<uint32_t, uint32_t> walks;
  bool sendingWalks{false};
};

int main(int argc, char *argv[]) {
  spdlog::set_level(spdlog::level::debug);

  std::string scorerAddress = "localhost:1235";

  spdlog::info("read config");
  config settings = read_config();

  // two way vat

  kj::UnixEventPort::captureSignal(SIGINT);
  auto ioContext = kj::setupAsyncIo();

  auto addrPromise = ioContext.provider->getNetwork().parseAddress(scorerAddress)
  .then([](kj::Own<kj::NetworkAddress> addr) {
      spdlog::info("using addr {}", std::string(addr->toString().cStr()));
      return addr->connect().attach(kj::mv(addr));
  });

  auto stream = addrPromise.wait(ioContext.waitScope);

  capnp::TwoPartyVatNetwork network(*stream, capnp::rpc::twoparty::Side::CLIENT);

  auto rpcSystem = capnp::makeRpcClient(network);

  {
    capnp::MallocMessageBuilder message;
    auto hostId = message.getRoot<capnp::rpc::twoparty::VatId>();
    hostId.setSide(capnp::rpc::twoparty::Side::SERVER);

    Scorer::Client master = rpcSystem.bootstrap(hostId).castAs<Scorer>();

    spdlog::info("creating client");

    ScorerWorker::Client worker = kj::heap<ScorerWorkerImpl>(ioContext, settings, master);

    spdlog::info("create request");
    auto request = master.registerScorerWorkerRequest();
    request.setWorker(worker);

    spdlog::info("send scorer register");

    auto r = request.send().then(
        [] (auto result) {
          spdlog::info("scorer registered");
        },
        [] (auto exception) {
          spdlog::warn("exception registering scorer : {}", std::string(exception.getDescription()));
        });

    spdlog::info("waiting for register");
    r.wait(ioContext.waitScope);

    spdlog::info("waiting for sigint");
    ioContext.unixEventPort.onSignal(SIGINT).wait(ioContext.waitScope);
  }

  return 0;
}

