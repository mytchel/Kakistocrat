#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <curl/curl.h>
#include "lexbor/html/html.h"
#include <lexbor/dom/dom.h>

#include <list>
#include <vector>
#include <set>
#include <map>
#include <string>
#include <iostream>
#include <fstream>

#include <chrono>
#include <thread>

#include "channel.h"
#include "util.h"
#include "scrape.h"
#include "scraper.h"

namespace scrape {

struct curl_data {
  char *buf{NULL};
  size_t size{0};
  size_t max;

  site *m_site;
  index_url url;

  bool unchanged{false};

  curl_data(site *s, index_url u, size_t m) :
      m_site(s), url(u), max(m) {}

  ~curl_data() {
    if (buf) free(buf);
  }

  void finish(std::string effective_url);
  void finish_bad_http(int code);
  void finish_bad_net(CURLcode res);

  void save();
};

size_t curl_cb_buffer_write(void *contents, size_t sz, size_t nmemb, void *ctx)
{
  curl_data *d = (curl_data *) ctx;
  size_t realsize = sz * nmemb;

  if (d->max < d->size + realsize) {
    return 0;
  }

  if (d->buf == NULL) {
    d->buf = (char *) malloc(d->max);
    if (d->buf == NULL) {
      return 0;
    }
  }

  memcpy(&(d->buf[d->size]), contents, realsize);
  d->size += realsize;

  return realsize;
}

size_t curl_cb_header_write(char *buffer, size_t size, size_t nitems, void *ctx) {
  curl_data *d = (curl_data *) ctx;

  buffer[nitems*size] = 0;

  if (strstr(buffer, "content-type:")) {
    if (strstr(buffer, "text/html") == NULL) {
      return 0;
    }

  } else if (strstr(buffer, "Last-Modified: ")) {
    char *s = buffer + strlen("Last-Modified: ");

    tm tm;
    strptime(s, "%a, %d %b %Y %H:%M:%S", &tm);
    time_t time = mktime(&tm);

    if (d->url.last_scanned > time) {
      d->unchanged = true;
      return 0;
    }
  }

  return nitems * size;
}

void curl_data::save()
{
  std::ofstream file;

  if (buf == NULL) {
    printf("save '%s' but no buffer\n", url.url.c_str());
    return;
  }

  file.open(url.path, std::ios::out | std::ios::binary | std::ios::trunc);

  if (!file.is_open()) {
    fprintf(stderr, "error opening file %s for %s\n", url.path.c_str(), url.url.c_str());
    return;
  }

  file.write(buf, size);

  file.close();
}

std::list<std::string> find_links_lex(
      lxb_html_parser_t *parser,
      char *buf, size_t buf_len,
      std::string page_url)
{
  std::list<std::string> urls;

  lxb_status_t status;
  lxb_dom_element_t *element;
  lxb_html_document_t *document;
  lxb_dom_collection_t *collection;

  if (buf == NULL) {
    printf("buffer for %s has no data\n", page_url.c_str());
    return urls;
  }

  document = lxb_html_parse(parser, (const lxb_char_t *) buf, buf_len);
  if (document == NULL) {
    printf("Failed to create Document object\n");
    return urls;
  }

  collection = lxb_dom_collection_make(&document->dom_document, 128);
  if (collection == NULL) {
    printf("Failed to create Collection object");
    exit(1);
  }

  if (document->body == NULL) {
    return urls;
  }

  status = lxb_dom_elements_by_tag_name(lxb_dom_interface_element(document->body),
                                        collection,
                                        (const lxb_char_t *) "a", 1);
  if (status != LXB_STATUS_OK) {
      printf("Failed to get elements by name\n");
      exit(1);
  }

  auto page_proto = util::get_proto(page_url);
  auto page_host = util::get_host(page_url);
  auto page_dir = util::get_dir(util::get_path(page_url));

  char attr_name[] = "href";
  size_t attr_len = 4;
  for (size_t i = 0; i < lxb_dom_collection_length(collection); i++) {
      element = lxb_dom_collection_element(collection, i);

      size_t len;
      char *s = (char *) lxb_dom_element_get_attribute(
            element,
            (const lxb_char_t*) attr_name,
            attr_len,
            &len);

      if (s == NULL) {
        continue;
      }

      // http://
      // https://
      // #same-page skip
      // /from-root
      // from-current-dir
      // javascript: skip
      // //host/page keep protocol

      std::string url(s);

      if (url.empty() || url.front() == '#')
        continue;

      if (!util::bare_minimum_valid_url(url))
        continue;

      auto proto = util::get_proto(url);
      if (proto.empty()) {
        proto = page_proto;

      } else if (!want_proto(proto))  {
        continue;
      }

      auto host = util::get_host(url);
      if (host.empty()) {
        host = page_host;
      }

      auto path = util::get_path(url);

      if (bad_suffix(path))
        continue;

      if (bad_prefix(path))
        continue;

      if (!path.empty() && path.front() != '/') {
        path = page_dir + "/" + path;
      }

      auto fixed = proto + "://" + host + path;
      urls.push_back(fixed);
  }

  lxb_dom_collection_destroy(collection, true);
  lxb_html_document_destroy(document);

  return urls;
}

void curl_data::finish(std::string effective_url) {
  save();

  lxb_status_t status;
  lxb_html_parser_t *parser;
  parser = lxb_html_parser_create();
  status = lxb_html_parser_init(parser);

  if (status != LXB_STATUS_OK) {
    m_site->finish_bad(url, true);
    return;
  }

  auto urls = find_links_lex(parser, buf, size, effective_url);

  lxb_html_parser_destroy(parser);

  m_site->finish(url, urls);
}

void curl_data::finish_bad_http(int code) {
  m_site->finish_bad(url, true);
}

void curl_data::finish_bad_net(CURLcode res) {
  if (unchanged) {
    //m_site->finish_unchanged(url);
    m_site->finish_bad(url, false);
  } else {
    if (res != CURLE_WRITE_ERROR) {
      printf("miss %s %s\n", curl_easy_strerror(res), url.url.c_str());
      m_site->finish_bad(url, true);
    } else {
      m_site->finish_bad(url, false);
    }
  }
}

CURL *make_handle(site* s, index_url u)
{
  curl_data *d = new curl_data(s, u, 1024 * 1024 * 10);

  CURL *curl_handle = curl_easy_init();

  curl_easy_setopt(curl_handle, CURLOPT_PRIVATE, d);
  curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, curl_cb_header_write);
  curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, d);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_cb_buffer_write);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, d);
  curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
  curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);

  // Potentially stops issues but doesn't seem to change much.
  curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);

  curl_easy_setopt(curl_handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

  char url[util::max_url_len];
  strncpy(url, u.url.c_str(), sizeof(url));
  curl_easy_setopt(curl_handle, CURLOPT_URL, url);

  return curl_handle;
}

void
scraper(Channel<site*> &in, Channel<site*> &out, Channel<bool> &stat, int tid)
{
  printf("thread %i started\n", tid);

  size_t max_con = 300;
  size_t max_host = 6;

  CURLM *multi_handle = curl_multi_init();
  curl_multi_setopt(multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_con);
  curl_multi_setopt(multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS, max_host);

  /* enables http/2 if available */
#ifdef CURLPIPE_MULTIPLEX
  curl_multi_setopt(multi_handle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif

  std::list<site*> sites;

  size_t active_connections = 0;

  bool accepting = true;
  int stat_i = 0;
  while (true) {
    bool n_accepting = active_connections < max_con * 0.9;
    if (accepting != n_accepting) {
      n_accepting >> stat;
      accepting = n_accepting;
    }

    if (++stat_i % 1000 == 0)
      printf("%i with %i active for %i sites\n", tid, active_connections, sites.size());

    bool adding = true;
    while (active_connections < max_con && adding) {
      adding = false;
      auto s = sites.begin();
      while (s != sites.end()) {
        if ((*s)->finished()) {
          *s >> out;
          s = sites.erase(s);
          continue;
        }

        if ((*s)->have_next(max_host)) {
          auto u = (*s)->pop_next();
          curl_multi_add_handle(multi_handle, make_handle(*s, u));
          active_connections++;
          adding = true;
        }

        s++;
      }
    }

    if (!in.empty()) {
      site *s;
      s << in;

      if (s == NULL) {
        printf("%i got finish\n", tid);
        break;
      }

      sites.push_back(s);
    }

    if (sites.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    curl_multi_wait(multi_handle, NULL, 0, 1000, NULL);
    int still_running = 0;
    curl_multi_perform(multi_handle, &still_running);

    int msgs_left;
    CURLMsg *m = NULL;
    while ((m = curl_multi_info_read(multi_handle, &msgs_left))) {
      if(m->msg == CURLMSG_DONE) {
        CURL *handle = m->easy_handle;
        curl_data *d;
        char *url;

        curl_easy_getinfo(handle, CURLINFO_PRIVATE, &d);
        curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url);

        CURLcode res = m->data.result;

        if (res == CURLE_OK) {
          long res_status;
          curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &res_status);
          if (res_status == 200) {
            d->finish(std::string(url));
          } else {
            d->finish_bad_http((int) res_status);
          }
        } else {
          d->finish_bad_net(res);
        }

        delete d;

        curl_multi_remove_handle(multi_handle, handle);
        curl_easy_cleanup(handle);

        active_connections--;
      }
    }
  }

  curl_multi_cleanup(multi_handle);

  printf("%i ending\n", tid);
}
}


