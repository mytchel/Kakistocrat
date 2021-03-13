#include <stdio.h>
#include <unistd.h>

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
#include <optional>
#include <chrono>

using namespace std::chrono_literals;

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include "util.h"
#include "crawl.h"

namespace crawl {

bool crawler::check_blacklist(
      std::string host)
{
  for (auto &b: blacklist) {
    if (host.find(b) != std::string::npos) {
      return true;
    }
  }

  return false;
}

page* site_find_add_page(site *site, std::string url, size_t level,
    std::string path = "")
{
  if (site->level > level) site->level = level;

  auto p = site->find_page(url);
  if (p != NULL) {
    if (p->level > level) p->level = level;
    return p;
  }

  if (path.empty()) {
    path = util::make_path(url);
  }

  p = site->find_page_by_path(path);
  if (p != NULL) {
    if (p->level > level) p->level = level;
    return p;
  }

  auto id = site->next_id++;

  return &site->pages.emplace_back(id, level, url, path);
}

void crawler::enable_references(
    site *isite,
    size_t max_add_sites,
    size_t next_max_pages)
{
  std::map<uint32_t, size_t> sites_link_count;

  for (auto &page: isite->pages) {
    if (!page.valid) continue;

    for (auto &l: page.links) {
      auto it = sites_link_count.find(l.site);
      if (it == sites_link_count.end()) {
        sites_link_count.emplace(l.site, 1);
      } else {
        it->second++;
      }
    }
  }

  std::list<uint32_t> linked_sites;

  for (auto &s: sites_link_count) {
    linked_sites.push_back(s.first);
  }

  linked_sites.sort(
      [&sites_link_count](uint32_t a, uint32_t b) {
        auto aa = sites_link_count.find(a);
        auto bb = sites_link_count.find(b);
        return aa->second > bb->second;
      });

  size_t add_sites = 0;
  for (auto &sid: linked_sites) {
    if (add_sites >= max_add_sites) {
      break;
    }

    add_sites++;
    auto site = find_site(sid);
    if (site != NULL) {
      site->max_pages += next_max_pages;

      printf("site %s is adding available pages %i to %s\n", isite->host.c_str(),
          next_max_pages, site->host.c_str());
    }
  }
}

void crawler::update_site(
    site *isite,
    std::list<scrape::index_url> &page_list)
{
  isite->pages.reserve(page_list.size());

  for (auto &u: page_list) {
    auto p = site_find_add_page(isite, u.url, isite->level + 1, u.path);

    p->links.clear();
    p->links.reserve(u.links.size());

    p->title = u.title;

    p->last_scanned = u.last_scanned;
    p->valid = u.ok;
  }

  for (auto &u: page_list) {
    auto p = isite->find_page(u.url);
    if (p == NULL) continue;

    for (auto &l: u.links) {
      auto host = util::get_host(l);
      if (host == "") continue;

      if (host == isite->host) {
        auto n_p = isite->find_page(l);
        // Incase all the links didn't come though then
        // don't fuck with the page reference.
        if (n_p != NULL) {
          p->links.emplace_back(isite->id, n_p->id);
        }

      } else {
        if (check_blacklist(host)) {
          continue;
        }

        site *o_site = find_site(host);

        if (o_site == NULL) {
          site n_site(next_id++, p->level + 1, host);

          auto n_p = site_find_add_page(&n_site, l, p->level + 1);

          p->links.emplace_back(n_site.id, n_p->id);

          sites.push_back(n_site);

        } else {
          auto o_p = site_find_add_page(o_site, l, p->level + 1);

          p->links.emplace_back(o_site->id, o_p->id);
        }
      }
    }
  }
}

void crawler::load_seed(std::vector<std::string> url)
{
  for (auto &o: url) {
    auto host = util::get_host(o);

    if (check_blacklist(host)) {
      continue;
    }

    auto o_site = find_site(host);
    if (o_site == NULL) {
      site n_site(next_id++, 0, host);

      n_site.max_pages = levels[0].max_pages;

      site_find_add_page(&n_site, o, 0);

      sites.push_back(n_site);

    } else {
      site_find_add_page(o_site, o, 0);
    }
  }
}

bool crawler::have_next_site()
{
  for (auto &site: sites) {
    if (site.scraping) continue;
    if (site.scraped) continue;
    if (site.max_pages == 0) continue;
    if (site.level >= levels.size()) continue;

    return true;
  }

  return false;
}

site* crawler::get_next_site()
{
  site *s = NULL;

  auto start = std::chrono::steady_clock::now();

  for (auto &site: sites) {
    if (site.scraping) continue;
    if (site.scraped) continue;
    if (site.max_pages == 0) continue;
    if (site.level >= levels.size()) continue;

    if (s == NULL || site.level < s->level) {
      s = &site;
    }
  }

  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  if (elapsed.count() > 100) {
    printf("get next site took %fms\n", elapsed.count());
  }

  if (s != NULL) {
    s->load();
  }

  return s;
}

void crawler::crawl()
{
  auto n_threads = std::thread::hardware_concurrency();
  // TODO: get from file limit
  size_t max_con_per_thread = 100;//1000 / (2 * n_threads);

  printf("starting %i threads\n", n_threads);

  Channel<scrape::site*> in_channels[n_threads];
  Channel<scrape::site*> out_channels[n_threads];
  Channel<bool> stat_channels[n_threads];
  bool thread_stats[n_threads];

  std::vector<std::thread> threads;
  for (size_t i = 0; i < n_threads; i++) {
    auto th = std::thread(
        [](
            Channel<scrape::site*> &in,
            Channel<scrape::site*> &out,
            Channel<bool> &stat,
            int i, size_t m) {
          scrape::scraper(in, out, stat, i, m);
        },
        std::ref(in_channels[i]),
        std::ref(out_channels[i]),
        std::ref(stat_channels[i]),
        i, max_con_per_thread);

    threads.emplace_back(std::move(th));
  }

  std::list<scrape::site> scrapping_sites;

  save();
  auto last_save = std::chrono::system_clock::now();
  bool have_changes = false;

  while (true) {
    if (have_changes && last_save + 10s < std::chrono::system_clock::now()) {
      // Save the current index so exiting early doesn't loose
      // all the work that has been done
      save();
      last_save = std::chrono::system_clock::now();
    }

    bool all_blocked = true;

    for (size_t i = 0; i < n_threads; i++) {
      if (!stat_channels[i].empty()) {
        thread_stats[i] << stat_channels[i];
        all_blocked &= !thread_stats[i];
      }

      if (thread_stats[i]) {
        auto site = get_next_site();
        if (site != NULL) {
          site->scraping = true;

          std::list<scrape::index_url> urls;

          for (auto &p: site->pages) {
            urls.emplace_back(p.url, p.path, p.last_scanned, p.valid);
          }

          scrapping_sites.emplace_back(site->host, levels[site->level].max_pages, urls);
          auto &s = scrapping_sites.back();

          &s >> in_channels[i];
        }
      }

      if (!out_channels[i].empty()) {
        scrape::site *s;
        s << out_channels[i];

        printf("site finished %s\n", s->host.c_str());

        auto site = find_site(s->host);
        if (site == NULL) {
          printf("site '%s' not found in map.\n", s->host.c_str());
          exit(1);
        }

        site->scraped = true;
        site->scraping = false;
        site->last_scanned = time(NULL);

        update_site(site, s->url_scanned);

        if (site->level + 1 < levels.size()) {
          auto level = levels[site->level];
          auto next_level = levels[site->level + 1];

          enable_references(site, level.max_add_sites, next_level.max_pages);
        }

        printf("site finished for level %zu with %3zu (+ %3zu unchanged) pages : %s\n",
            site->level, s->url_scanned.size(), s->url_unchanged.size(),
            site->host.c_str());

        scrapping_sites.remove_if([s](const scrape::site &ss) {
            return &ss == s;
            });

        site->unload();
        have_changes = true;
      }
    }

    auto delay = std::chrono::milliseconds(1);

    if (scrapping_sites.empty() && !have_next_site()) {
      time_t now = time(NULL);

      bool have_something = false;
      for (auto &s: sites) {
        size_t min = (4 * (1 + s.level)) * 60 * 60;
        if (s.last_scanned + min < now) {
          int r = rand() % ((24 * (1 + s.level) - 4) * 60 * 60);
          s.scraped = s.last_scanned + min + r < now;
          if (!s.scraped) {
            printf("transition %s from scraped to ready as %i + %i + %i < %i\n",
                s.host.c_str(), s.last_scanned, min, r, now);

            s.load();
            have_something = true;
          }
        } else {
          s.unload();
        }
      }

      if (!have_something) {
        delay = std::chrono::minutes(1);
        printf("have nothing\n");
        save();
      }

    } else if (all_blocked) {
      delay = std::chrono::milliseconds(100);
    }

    std::this_thread::sleep_for(delay);
  }
}

}

