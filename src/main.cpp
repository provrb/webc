#include <atomic>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/url/impl/parse.hpp>
#include <boost/url/url_base.hpp>
#include <boost/url/url_view.hpp>
#include <chrono>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <print>
#include <queue>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

// boost/beast
#include "boost/beast/core/flat_buffer.hpp"
#include "boost/beast/core/tcp_stream.hpp"
#include "boost/beast/http/dynamic_body_fwd.hpp"
#include "boost/beast/http/field.hpp"
#include "boost/beast/http/message_fwd.hpp"
#include "boost/beast/http/string_body_fwd.hpp"
#include "boost/beast/http/verb.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

// boost/ASIO
#include "boost/asio/io_context.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "boost/system/system_error.hpp"
#include <boost/url/url.hpp>

#include "concurrentqueue.h"
#include "indexer.hpp"
#include "parser.hpp"

/*
    "The basic web crawling algorithm is simple:
    Given a set of seed Uni-form Resource Locators (URLs),
    a crawler downloads all the web pages
    addressed by the URLs, extracts the hyperlinks contained in the pages,
    and iteratively downloads the web pages addressed by these hyperlinks."
    source:
   https://woodyinternational.com/index.php/jaii/article/view/121/97

    The goal of this web crawler is to parse data like prices for stuff like
   car listings. Pull the price, listing link, vehicle name, km,
   description, etc etc.
*/

/*
    After arriving at a page it will be useful to perform a valuation of the
   current page. The below criterion can be checked with a hash
   we can check if its a duplicate from the url though.
    1. is it a duplicate
    2. has it been modified since we last checked

    After pulling a page's content, take sha256 hash and make a new entry in
   the index. Then parse the data we want and store it with the sha256 hash
   as the key. We can also store links that the page contained or maybe even
   do some sort of data structure. We don't store the HTML/webpage content
   because that will take a lot of bytes in strings, and After we extract
   and parse all the data we need I don't believe there is any reason for
   the HTML content.
*/

/*
 * Current limitations: doesn't work with https right now.
 *
 * For persistant storage we'd need a database.
 * Would need to be rigorously tested among many different websites.
 * Per-site rate limits.
 * Be a respectful webcrawler ethically. Apparently it's possible to
 * accidently DOS a website. Crawlers can be injected with misleading
 * content or useless content.
 */

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;

static inline constexpr size_t MAX_BATCH_SIZE = 500;
static inline constexpr size_t NUM_WORKERS = 45;
static inline constexpr size_t MAX_DEPTH = 100;
static inline moodycamel::ConcurrentQueue<std::pair<boost::urls::url, size_t>>
    cc_links_to_visit;

// every url ever queued, shared by all workers so a page is only crawled once.
// marked when the url is enqueued
static inline std::unordered_set<boost::urls::url> visited;
static inline std::mutex visited_mutex;

// how many workers are in the middle of a page. an empty queue only means the
// crawl is over when this is 0, otherwise a worker may still enqueue links
static inline std::atomic<size_t> in_flight = 0;

class Crawler {
public:
  explicit Crawler(std::string start_address,
                   indexer::DatabaseConnection &database_connection)
      : start_addr_(std::move(start_address)), tcp_resolver_(ioc_),
        tcp_stream_(ioc_), database(database_connection) {
    batch_to_insert_.reserve(MAX_BATCH_SIZE);
    crawler_parser = parser::Parser();
    std::println("[crawler] init with start address: `{}`", start_addr_);
  }

  [[nodiscard]] static auto resolve_link(const boost::urls::url_view &page,
                                         std::string_view href)
      -> std::optional<boost::urls::url> {
    const auto ref = boost::urls::parse_uri_reference(href);
    if (!ref.has_value())
      return std::nullopt;

    boost::urls::url absolute;
    if (!boost::urls::resolve(page, *ref, absolute))
      return std::nullopt;

    if (absolute.scheme_id() != boost::urls::scheme::http)
      return std::nullopt;

    absolute.remove_fragment();
    absolute.normalize();
    return absolute;
  }

  auto breadth(this auto &self) -> void {
    std::pair<boost::urls::url, size_t> next_link = {};

    while (true) {
      ++in_flight;
      if (!cc_links_to_visit.try_dequeue(next_link)) {
        --in_flight;
        if (in_flight == 0 && cc_links_to_visit.size_approx() == 0)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      struct InFlightGuard {
        ~InFlightGuard() { --in_flight; }
      } in_flight_guard;

      const boost::urls::url current_url = std::move(next_link.first);
      const size_t depth = std::move(next_link.second);

      std::println("{}", current_url.data());
      http::response<http::string_body> page_response;
      try {
        page_response = self.get_webpage_content(current_url);
      } catch (const boost::system::system_error &e) {
        std::println(stderr, "[crawler] fetching `{}` failed: {}",
                     current_url.data(), e.what());
        self.tcp_stream_.close();
        self.connected_ = false;
        continue;
      }

      if (page_response.result() != http::status::ok) {
        std::println(stderr, "[crawler] `{}` returned status {}",
                     current_url.data(), page_response.result_int());
        continue;
      }

      const std::string response_body = page_response.body();
      std::vector<parser::ProductSchema> schemas =
          self.crawler_parser.extract_all_matching(response_body);

      std::expected<indexer::WebpageEntry, uint64_t> current_webpage_entry_res =
          indexer::WebpageEntry::create(current_url.data(), schemas);

      if (!current_webpage_entry_res.has_value()) {
        std::println(stderr,
                     "[crawler] webpage::create() for url `{}` failed with "
                     "openssl code `{}`",
                     current_url.data(), current_webpage_entry_res.error());
        continue;
      }

      indexer::WebpageEntry current_webpage_entry =
          current_webpage_entry_res.value();

      self.insert_into_batch(current_webpage_entry);

      if (depth >= MAX_DEPTH)
        continue;

      std::vector<std::string> links =
          self.crawler_parser.extract_links(response_body);

      for (const auto &l : links) {
        const auto resolved = resolve_link(current_url, l);
        if (!resolved.has_value())
          continue;

        // stay on the site we started on
        if (resolved->host() != self.addr_host_)
          continue;

        {
          std::lock_guard lock(visited_mutex);
          if (!visited.insert(resolved.value()).second)
            continue;
        }

        cc_links_to_visit.enqueue(std::make_pair(resolved.value(), depth + 1));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    self.flush_batch();
  }

  /*
   * Start scouring the sites and any links starting from the
   * start address provided in the constructor.
   */
  auto start(this auto &self) -> void {
    try {
      // setup initial url
      const auto base_res = boost::urls::parse_uri(self.start_addr_);
      if (!base_res.has_value()) {
        std::println(stderr,
                     "[crawler::start] start address given syntactically "
                     "incorrect. error {}",
                     base_res.error().message());
        return;
      }
      self.base_addr_ = base_res.value();
      if (self.base_addr_->path().empty())
        self.base_addr_->set_path("/");

      self.addr_host_ = self.base_addr_.value().host();

      self.endpoints_ = self.tcp_resolver_.resolve(self.addr_host_, "http");
      for (const auto &entry : self.endpoints_) {
        auto endpoint = entry.endpoint();
        std::println("entry: {}:{}", endpoint.address().to_string(),
                     endpoint.port());
      }

      self.tcp_stream_.connect(self.endpoints_);
      self.connected_ = true;

      // 1. use given url
      // 2. check for duplicates in indexer/database
      // 3. pass to parser. html_content
      // 4. add parsed data to indexer/database
      // 5. find more urls
      // self.recurse(self.base_addr_.value(), 0);
      self.breadth();
    } catch (const boost::system::system_error &e) {
      std::println(stderr, "[crawler->start] error: `{}`", e.what());
    }
  }

  [[nodiscard]] auto get_webpage_content(this auto &self,
                                         const boost::urls::url_view &url)
      -> http::response<http::string_body> {
    if (!self.connected_) {
      self.tcp_stream_.connect(self.endpoints_);
      self.connected_ = true;
    }

    std::string target(url.encoded_target());
    if (target.empty())
      target = "/";

    http::request<http::string_body> req = {http::verb::get, target, 11};
    req.set(http::field::host, self.addr_host_);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    http::write(self.tcp_stream_, req);

    http::response<http::string_body> response;
    beast::flat_buffer buffer;

    http::read(self.tcp_stream_, buffer, response);

    if (!response.keep_alive()) {
      self.tcp_stream_.close();
      self.connected_ = false;
    }
    return response;
  }

private:
  auto insert_into_batch(this auto &self, indexer::WebpageEntry entry) noexcept
      -> void {
    // check if this page we want to add to the batch already exists in the
    // database. if the page exists, check the pages hash between the hash of
    // the new page. if they differ then add the updated webpage `entry`
    const auto existing = self.database.get_webpage(entry.url);
    if (existing.has_value() && existing->webpage_hash == entry.webpage_hash)
      return;

    self.batch_to_insert_.push_back(std::move(entry));
    if (self.batch_to_insert_.size() >= MAX_BATCH_SIZE)
      self.flush_batch();
  }

  auto flush_batch(this auto &self) noexcept -> void {
    if (self.batch_to_insert_.empty())
      return;
    self.database.save_webpage_vector(self.batch_to_insert_);
    self.batch_to_insert_.clear();
  }

  indexer::DatabaseConnection &database;
  parser::Parser crawler_parser;

  std::string start_addr_; // contains "http" - website link we start on
  std::string addr_host_;
  std::optional<boost::urls::url>
      base_addr_; // start_addr as a boost::url, never changes. contains "http"

  asio::io_context ioc_;
  asio::ip::tcp::resolver tcp_resolver_;
  beast::tcp_stream tcp_stream_;
  asio::ip::tcp::resolver::results_type endpoints_;
  bool connected_ = false;

  std::vector<indexer::WebpageEntry> batch_to_insert_;
};

auto main() -> int {
  constexpr std::string_view seed_address = "http://books.toscrape.com";

  // single connection
  indexer::DatabaseConnection db;

  const auto seed_res = boost::urls::parse_uri(seed_address);
  if (!seed_res.has_value()) {
    std::println(stderr, "[main] seed address `{}` is not a valid url: {}",
                 seed_address, seed_res.error().message());
    return 1;
  }
  boost::urls::url seed = seed_res.value();
  if (seed.path().empty())
    seed.set_path("/");
  seed.normalize();
  visited.insert(seed);
  cc_links_to_visit.enqueue(std::make_pair(std::move(seed), 0));

  std::vector<std::jthread> threads;

  auto s = std::chrono::high_resolution_clock::now();
  std::println("Start.\n");
  for (size_t i = 0; i < NUM_WORKERS; ++i)
    threads.emplace_back([&db, seed_address] {
      try {
        Crawler crawler(std::string(seed_address), db);
        crawler.start();
      } catch (const std::exception &e) {
        std::println(stderr, "[worker] stopped: {}", e.what());
      }
    });

  for (auto &t : threads) {
    if (t.joinable())
      t.join();
  }

  auto e = std::chrono::high_resolution_clock::now();
  std::println("End.\n");

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(e - s);
  std::println("Elapsed: {}", elapsed.count());

  return 0;
}
