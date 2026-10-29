#pragma once

// lexbor
#include <lexbor/core/base.h>
#include <lexbor/core/types.h>
#include <lexbor/css/css.h>
#include <lexbor/css/parser.h>
#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/html/html.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/selectors/selectors.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parser {
struct ProductSchema {
  std::optional<std::string> price;
  std::string listing_name;
  std::string listing_description;
  std::string webpage_hash;
  std::string webpage_url; // the page this listing came from
  int id = 0; // row key in the database, assigned on insert

  [[nodiscard]] inline auto get_price() const -> std::optional<std::string> {
    return price;
  }
  [[nodiscard]] inline auto get_listing_name() const -> const std::string & {
    return listing_name;
  }
  [[nodiscard]] inline auto get_listing_description() const
      -> const std::string & {
    return listing_description;
  }

  inline auto set_price(std::optional<std::string> val) -> void { price = val; }
  inline auto set_listing_name(std::string val) -> void {
    listing_name = std::move(val);
  }
  inline auto set_listing_description(std::string val) -> void {
    listing_description = std::move(val);
  }
};

struct CssParserDeleter_ {
  void operator()(lxb_css_parser_t *p) const {
    lxb_css_parser_destroy(p, true);
  }
};

struct SelectorsDeleter_ {
  void operator()(lxb_selectors_t *s) const { lxb_selectors_destroy(s, true); }
};

struct SelectorListDeleter {
  void operator()(lxb_css_selector_list_t *l) const {
    lxb_css_selector_list_destroy_memory(l);
  }
};

using CssParser = std::unique_ptr<lxb_css_parser_t, CssParserDeleter_>;
using Selectors = std::unique_ptr<lxb_selectors_t, SelectorsDeleter_>;
using HtmlDoc =
    std::unique_ptr<lxb_html_document_t, decltype(&lxb_html_document_destroy)>;
using SelectorList =
    std::unique_ptr<lxb_css_selector_list_t, SelectorListDeleter>;

inline constexpr std::string_view SEL_NAME = "h3 a";
inline constexpr std::string_view SEL_PRICE = "p.price_color";
inline constexpr std::string_view SEL_DESCRIPTION = "#product_description ~ p";

class Parser {
public:
  Parser(const Parser &) = delete;
  Parser &operator=(Parser &&) = delete;

  static auto get() -> Parser & {
    static Parser inst;
    return inst;
  }

  [[nodiscard]] static auto get_css() -> const CssParser & { return get().css; }
  [[nodiscard]] static auto get_selectors() -> const Selectors & {
    return get().selectors;
  }

  [[nodiscard]] static auto
  extract_all_matching(const std::string &html_content) noexcept
      -> const std::vector<ProductSchema> {
    HtmlDoc doc(lxb_html_document_create(), lxb_html_document_destroy);
    lxb_html_document_parse(
        doc.get(), reinterpret_cast<const lxb_char_t *>(html_content.data()),
        html_content.size());

    std::vector<std::string> all_names = query_selectors(doc.get(), SEL_NAME);
    std::vector<std::string> all_prices = query_selectors(doc.get(), SEL_PRICE);
    std::vector<std::string> all_descriptions =
        query_selectors(doc.get(), SEL_DESCRIPTION);

    size_t upper_bound = std::max(
        {all_names.size(), all_prices.size(), all_descriptions.size()});
    std::vector<ProductSchema> products = {};
    products.reserve(upper_bound);

    for (size_t i = 0; i < upper_bound; i++) {
      std::optional<std::string> price =
          (i < all_prices.size()) ? std::optional<std::string>(all_prices.at(i))
                                  : std::nullopt;
      std::optional<std::string> name =
          (i < all_names.size()) ? std::optional<std::string>(all_names.at(i))
                                 : std::nullopt;
      std::optional<std::string> description =
          (i < all_descriptions.size())
              ? std::optional<std::string>(all_descriptions.at(i))
              : std::nullopt;

      ProductSchema ps;
      ps.price = std::move(price);
      ps.listing_name = name.value_or("");
      ps.listing_description = description.value_or("");
      products.push_back(std::move(ps));
    }
    return products;
  }

private:
  // function callback signature in lxb
  static auto collect_node_callback(lxb_dom_node_t *node,
                                    lxb_css_selector_specificity_t, void *ctx)
      -> lxb_status_t {
    static_cast<std::vector<lxb_dom_node_t *> *>(ctx)->push_back(node);
    return LXB_STATUS_OK;
  }

  // returns all the text values for the selector like a query.
  [[nodiscard]] static auto query_selectors(lxb_html_document_t *doc,
                                            std::string_view selector)
      -> std::vector<std::string> {
    SelectorList list{lxb_css_selectors_parse(
        get_css().get(), reinterpret_cast<const lxb_char_t *>(selector.data()),
        selector.size())};

    get_css()->memory = nullptr;
    auto *root = lxb_dom_document_element(&doc->dom_document);
    if (!list || root == nullptr)
      return std::vector<std::string>();

    std::vector<lxb_dom_node_t *> matching_nodes;
    lxb_selectors_find(get_selectors().get(), lxb_dom_interface_node(root),
                       list.get(), collect_node_callback, &matching_nodes);

    // parse the text from nodes
    std::vector<std::string> parsed;
    parsed.reserve(matching_nodes.size());
    for (const auto &node : matching_nodes) {

      // for elements/attributes
      auto *el = lxb_dom_interface_element(node);
      size_t len = 0;
      const lxb_char_t *title = lxb_dom_element_get_attribute(
          el, reinterpret_cast<const lxb_char_t *>("title"), 5, &len);

      // text content
      size_t len2 = 0;
      const lxb_char_t *text = lxb_dom_node_text_content(node, &len2);

      if (!title && !text)
        continue;

      std::string field;
      field.reserve(std::max(len, len2));
      if (title)
        field.assign(reinterpret_cast<const char *>(title), len);
      else if (text)
        field.assign(reinterpret_cast<const char *>(text), len2);

      parsed.push_back(std::move(field));
    }
    return parsed;
  }

  Parser() : css(lxb_css_parser_create()), selectors(lxb_selectors_create()) {
    lxb_css_parser_init(css.get(), nullptr);
    lxb_selectors_init(selectors.get());
  }
  ~Parser() = default;

  CssParser css;
  Selectors selectors;
};

} // namespace parser
