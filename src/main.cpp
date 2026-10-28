#include "boost/asio/io_context.hpp"
#include "boost/beast/core/flat_buffer.hpp"
#include "boost/beast/core/tcp_stream.hpp"
#include "boost/beast/http/dynamic_body_fwd.hpp"
#include "boost/beast/http/field.hpp"
#include "boost/beast/http/message_fwd.hpp"
#include "boost/beast/http/string_body_fwd.hpp"
#include "boost/beast/http/verb.hpp"
#include "boost/system/system_error.hpp"
#include <iostream>
#include <print>
#include <string_view>

// boost/beast
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

// boost/ASIO
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

/*
    "The basic web crawling algorithm is simple:
    Given a set of seed Uni-form Resource Locators (URLs),
    a crawler downloads all the web pages
    addressed by the URLs, extracts the hyperlinks contained in the pages,
    and iteratively downloads the web pages addressed by these hyperlinks."
    source: https://woodyinternational.com/index.php/jaii/article/view/121/97

    The goal of this web crawler is to parse data like prices for stuff like car
   listings. Pull the price, listing link, vehicle name, km, description, etc
   etc.
*/

/*
    After arriving at a page it will be useful to perform a valuation of the
   current page. The below criterion can be checked with a hash
    1. is it a duplicate
    2. has it been modified since we last checked

    After pulling a page's content, take sha256 hash and make a new entry in the
   index. Then parse the data we want and store it with the sha256 hash as the
   key. We can also store links that the page contained or maybe even do some
   sort of data structure. We don't store the HTML/webpage content because that
   will take a lot of bytes in strings, and After we extract and parse all the
   data we need I don't believe there is any reason for the HTML content.
*/

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;

class Crawler {
public:
  explicit Crawler(std::string start_address)
      : start_addr_(std::move(start_address)), tcp_resolver_(ioc_),
        tcp_stream_(ioc_) {
    std::println("[crawler] init with start address: `{}`", start_addr_);
  }

  /*
   * Start scouring the sites and any links starting from the
   * start address provided in the constructor.
   */
  auto start(this auto &self) -> void {
    try {
      const auto result = self.tcp_resolver_.resolve(self.start_addr_, "http");
      for (const auto &entry : result) {
        auto endpoint = entry.endpoint();
        std::println("entry: {}:{}", endpoint.address().to_string(),
                     endpoint.port());
      }

      self.tcp_stream_.connect(result);
      const auto &page_response = self.get_webpage(self.start_addr_);
      std::cout << page_response << std::endl;

    } catch (const boost::system::system_error &e) {
      std::println(stderr, "[crawler->start] error: `{}`", e.what());
    }
  }

  [[nodiscard]] auto get_webpage(this auto &self, std::string_view host)
      -> http::response<http::dynamic_body> {
    http::request<http::string_body> req = {http::verb::get, "/", 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    http::write(self.tcp_stream_, req);

    http::response<http::dynamic_body> response;
    beast::flat_buffer buffer;

    http::read(self.tcp_stream_, buffer, response);

    return response;
  }

private:
  std::string start_addr_ = "";
  asio::io_context ioc_;
  asio::ip::tcp::resolver tcp_resolver_;
  beast::tcp_stream tcp_stream_;
};

auto main() -> int {
  Crawler c("crawler-test.com");
  c.start();

  return 0;
}
