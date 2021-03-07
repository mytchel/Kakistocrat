#include <stdio.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include <vector>
#include <list>
#include <map>
#include <string>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "util.h"
#include "crawl.h"
#include "scorer.h"

using nlohmann::json;

namespace scorer {

void to_json(json &j, const page &p) {
  j = json{
      {"i", p.id},
      {"l", p.level},
      {"s", p.score},
      {"u", p.url},
      {"p", p.path},
      {"t", p.title},
      {"links", p.links}};
}

void from_json(const json &j, page &p) {
  j.at("i").get_to(p.id);
  j.at("l").get_to(p.level);
  j.at("s").get_to(p.score);
  j.at("u").get_to(p.url);
  j.at("p").get_to(p.path);
  j.at("t").get_to(p.title);
  j.at("links").get_to(p.links);
}

void scores::iteration()
{
  std::map<uint64_t, double> new_scores;

  for (auto &i: pages) {
    auto &p = i.second;

    crawl::page_id page_id(p.id);

    double link_score = p.score / (double) p.links.size();

    p.score = 0;

    for (auto &l: p.links) {
      crawl::page_id id(l);
      double score = link_score;
      if (page_id.site == id.site) {
        score *= 0.9;
      }

      auto i = new_scores.find(l);
      if (i == new_scores.end()) {
        new_scores.emplace(l, score);
      } else {
        i->second += score;
      }
    }
  }

  double sum = 0;

  for (auto &i: pages) {
    auto &p = i.second;
    auto score = new_scores.find(p.id);
    if (score != new_scores.end()) {
      p.score += score->second;
    }

    p.score /= (1 + p.level);

    sum += p.score;
  }

  printf("total score = %f\n", sum);

  for (auto &i: pages) {
    auto &p = i.second;
    p.score /= sum;
  }
}

void scores::init(crawl::index &index)
{
  pages.clear();
  size_t n_pages = 0;

  for (auto &s: index.sites) {
    if (!s.enabled) continue;

    s.load();

    for (auto &p: s.pages) {
      if (p.valid) {
        n_pages++;
      }
    }
  }

  printf("init from scrape with %lu pages\n", n_pages);

  /*
   * TODO:
   *    fancy level biasing.
   *
  double r = 2.0/3.0;
  if (level == level_counts.size() - 1) {
    r = 1.0;
  }

  double c = 1.0;
  for (size_t l = 0; l < level; l++) {
    c *= 1.0/3.0;
  }

  double base = c * r / n;


  for (size_t i = 0; i < max_level; i++) {
    level_scores.push_back(0);
  }

  auto &l = level_counts.at(s.level);
*/

  double score = 1.0 / (double) n_pages;

  for (auto &s: index.sites) {
    for (auto &p: s.pages) {
      if (!p.valid) continue;

      crawl::page_id id(s.id, p.id);

      std::vector<uint64_t> links;

      for (auto &l: p.links) {
        links.push_back(l.to_value());
      }

      page n(id.to_value(), p.level, score, p.url, p.path, p.title, links);

      pages.emplace(id.to_value(), n);
    }
  }

  for (auto &i: pages) {
    auto &p = i.second;

    crawl::page_id page_id(p.id);

    std::vector<uint64_t> fixed_links;

    for (auto &l: p.links) {
      auto p = find_page(l);
      if (p != NULL) {
        fixed_links.push_back(l);
      }
    }

    p.links = fixed_links;
  }

  double sum = 0;

  for (auto &i: pages) {
    auto &p = i.second;
    sum += p.score;
  }

  printf("total score = %f\n", sum);
}

void scores::save(std::string path)
{
  std::ofstream file;

  printf("save scores %lu -> %s\n", pages.size(), path.c_str());

  file.open(path, std::ios::out | std::ios::trunc);

  if (!file.is_open()) {
    fprintf(stderr, "error opening file %s\n", path.c_str());
    return;
  }

  json j = {{"pages", pages}};

  file << j;

  file.close();
}

void scores::load(std::string path)
{
  std::ifstream file;

  printf("load %s\n", path.c_str());

  file.open(path, std::ios::in);

  if (!file.is_open()) {
    fprintf(stderr, "error opening file %s\n", path.c_str());
    return;
  }

  pages.clear();

  json j = json::parse(file);

  j.at("pages").get_to(pages);

  file.close();
}

page* scores::find_page(uint64_t id)
{
  auto i = pages.find(id);
  if (i != pages.end()) {
    return &i->second;
  } else {
    return NULL;
  }
}

}

