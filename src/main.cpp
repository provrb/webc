#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/url/impl/parse.hpp>
#include <boost/url/url_base.hpp>
#include <boost/url/url_view.hpp>
#include <iostream>
#include <iterator>
#include <optional>
#include <print>
#include <queue>
#include <string_view>
#include <unordered_set>

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
#include <utility>

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

constexpr size_t MAX_DEPTH = 5;

class Crawler {
public:
  explicit Crawler(std::string start_address)
      : start_addr_(std::move(start_address)), tcp_resolver_(ioc_),
        tcp_stream_(ioc_) {
    std::println("[crawler] init with start address: `{}`", start_addr_);
  }

  auto filter_links(this auto &self) -> std::vector<boost::urls::url> {}

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
    std::queue<std::pair<std::string, size_t>> links_to_visit;
    links_to_visit.push(std::make_pair(self.start_addr_, 0));

    while (!links_to_visit.empty()) {
      std::pair<std::string, size_t> next_link = links_to_visit.front();
      links_to_visit.pop();

      const std::string current_url = next_link.first;
      const size_t depth = next_link.second;
      const auto boost_url_res =
          resolve_link(self.base_addr_.value(), current_url);
      const boost::urls::url boost_url = boost_url_res.value();
      std::println("{}", boost_url.data());

      if (!self.visited_.insert(boost_url.data()).second) {
        continue;
      }

      http::response<http::dynamic_body> page_response;
      try {
        page_response = self.get_webpage_content(boost_url);
      } catch (const boost::system::system_error &e) {
        std::println(stderr, "[crawler] fetching `{}` failed: {}", current_url,
                     e.what());
        self.tcp_stream_.close();
        self.connected_ = false;
        continue;
      }

      if (page_response.result() != http::status::ok) {
        std::println(stderr, "[crawler] `{}` returned status {}", current_url,
                     page_response.result_int());
        continue;
      }

      const std::string response_body =
          beast::buffers_to_string(page_response.body().data());
      std::vector<parser::ProductSchema> schemas =
          parser::Parser::extract_all_matching(response_body);

      std::expected<indexer::WebpageEntry, uint64_t> current_webpage_entry_res =
          indexer::WebpageEntry::create(boost_url.data(), schemas);

      if (!current_webpage_entry_res.has_value()) {
        std::println(stderr,
                     "[crawler] webpage::create() for url `{}` failed with "
                     "openssl code `{}`",
                     current_url, current_webpage_entry_res.error());
        continue;
      }

      indexer::WebpageEntry current_webpage_entry =
          current_webpage_entry_res.value();

      // check for duplicates in database to see if we scraped an update version
      auto existing_url_extracts =
          indexer::Database::get().get_webpages(boost_url.data());

      if (existing_url_extracts.size() <= 0) {
        // no existing webpage in. save initial one
        indexer::Database::get().save_webpage(current_webpage_entry);
      } else {
        // this url is already in the database.
        // check if our hash is the same
        for (const indexer::WebpageEntry &we : existing_url_extracts) {
          if (we.webpage_hash != current_webpage_entry.webpage_hash) {
            // update this url's old parsed listings with the new one
            indexer::Database::get().remove_webpage(we.url);
            indexer::Database::get().save_webpage(current_webpage_entry);
          }
        }
      }

      std::vector<std::string> links =
          parser::Parser::extract_links(response_body);

      for (const auto &l : links) {
        const auto resolved = resolve_link(current_url, l);
        if (!resolved.has_value())
          continue;

        // stay on the site we started on
        if (resolved->host() != self.addr_host_)
          continue;

        if (self.visited_.contains(resolved.value().data())) {
          continue;
        }

        links_to_visit.push(std::make_pair(resolved.value().data(), depth + 1));
      }
    }
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
      -> http::response<http::dynamic_body> {
    // the server closed the last connection: reuse the resolved addresses
    if (!self.connected_) {
      self.tcp_stream_.connect(self.endpoints_);
      self.connected_ = true;
    }

    // path + query, e.g. "/catalogue/page-2.html?x=1"
    std::string target(url.encoded_target());
    if (target.empty())
      target = "/";

    http::request<http::string_body> req = {http::verb::get, target, 11};
    req.set(http::field::host, self.addr_host_);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    http::write(self.tcp_stream_, req);

    http::response<http::dynamic_body> response;
    beast::flat_buffer buffer;

    http::read(self.tcp_stream_, buffer, response);

    if (!response.keep_alive()) {
      self.tcp_stream_.close();
      self.connected_ = false;
    }
    return response;
  }

private:
  std::string start_addr_;
  std::string addr_host_;
  std::optional<boost::urls::url> base_addr_;

  asio::io_context ioc_;
  asio::ip::tcp::resolver tcp_resolver_;
  beast::tcp_stream tcp_stream_;
  asio::ip::tcp::resolver::results_type endpoints_;
  bool connected_ = false;

  std::unordered_set<std::string> visited_;
  std::queue<std::pair<std::string, size_t>> to_visit_;
};

auto main() -> int {
  // using some random book web scraping page as a test
  Crawler c("http://books.toscrape.com");
  c.start();

  return 0;
}
