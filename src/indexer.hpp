#pragma once

#include "parser.hpp"

// SQLITE
#include "sqlite_orm.h"

// OpenSSL
#include <openssl/err.h>
#include <openssl/evp.h>

// boost
#include <boost/url/url.hpp>

#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace indexer {
struct WebpageEntry {
private:
  WebpageEntry(std::string_view url,
               std::vector<parser::ProductSchema> parsed_listings,
               std::string webpage_hash)
      : url(url), parsed_listings(parsed_listings), webpage_hash(webpage_hash) {
  }

public:
  WebpageEntry() = default;

  // create a webpage entry belonging to a url (normalize the URL), cotaining
  // all the parsed website data, then computes a hash over all the parsed data.
  // on success, returns the filled out WebpageEntry struct, otherwise returns a
  // uint64_t describing an openssl error. The one time this function can go
  // wrong is if an openssl operation fails. Safety: safe, all memory managed by
  // smart pointers.
  [[nodiscard]] static inline auto
  create(std::string url,
         std::vector<parser::ProductSchema> parsed_listings) noexcept
      -> std::expected<const WebpageEntry, uint64_t> {
    // normalize url (all lowercase)
    std::transform(url.begin(), url.end(), url.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    // generate hash from parsed listing content
    std::string to_hash = "";
    for (const auto &ps : parsed_listings) {
      to_hash += ps.price.value_or("");
      to_hash += ps.listing_description;
      to_hash += ps.listing_name;
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
      return std::unexpected(ERR_get_error());
    }

    const EVP_MD *md = EVP_sha256();
    if (!md) {
      return std::unexpected(ERR_get_error());
    }

    if (EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
      return std::unexpected(ERR_get_error());
    }

    if (EVP_DigestUpdate(ctx.get(), to_hash.data(), to_hash.size()) != 1) {
      return std::unexpected(ERR_get_error());
    }

    std::vector<unsigned char> hash(EVP_MAX_MD_SIZE);
    uint32_t hash_len = 0;
    if (EVP_DigestFinal(ctx.get(), hash.data(), &hash_len) != 1) {
      return std::unexpected(ERR_get_error());
    }

    hash.resize(hash_len);
    std::ostringstream oss;
    oss << std::setfill('0') << std::hex;
    for (unsigned char byte : hash) {
      oss << std::setw(2) << static_cast<int>(byte);
    }

    std::string webpage_hash = oss.str();

    // every listing row points back at this page
    for (auto &ps : parsed_listings) {
      ps.webpage_hash = webpage_hash;
      ps.webpage_url = url;
    }

    return WebpageEntry(url, parsed_listings, webpage_hash);
  }

  std::string url;
  std::string webpage_hash;
  std::vector<parser::ProductSchema> parsed_listings;
};

class Database {
public:
  Database(const Database &) = delete;
  Database &operator=(Database &&) = delete;

  static auto get() -> Database & {
    static Database inst;
    return inst;
  }

  static auto save_webpage(const WebpageEntry &we) -> void {
    storage.replace(we);
    for (const auto &ps : we.parsed_listings) {
      storage.insert(ps);
    }
  }

  [[nodiscard]] static inline auto get_webpages(std::string_view url)
      -> std::vector<WebpageEntry> {
    return storage.get_all<WebpageEntry>(
        sqlite_orm::where(sqlite_orm::c(&WebpageEntry::url) == url));
  }

  static inline auto remove_webpage(std::string_view url) {
    storage.remove<WebpageEntry>(url);
  }

private:
  static constexpr inline std::string_view storage_name = "webcs.db";
  static inline auto storage = sqlite_orm::make_storage(
      std::string{storage_name},
      sqlite_orm::make_table(
          "webpages",
          sqlite_orm::make_column("url", &WebpageEntry::url,
                                  sqlite_orm::primary_key()),
          sqlite_orm::make_column("webpage_hash", &WebpageEntry::webpage_hash)),
      sqlite_orm::make_table(
          "parsed_webpage_content",
          sqlite_orm::make_column("id", &parser::ProductSchema::id,
                                  sqlite_orm::primary_key().autoincrement()),
          sqlite_orm::make_column("webpage_url",
                                  &parser::ProductSchema::webpage_url),
          sqlite_orm::make_column("webpage_hash",
                                  &parser::ProductSchema::webpage_hash),
          sqlite_orm::make_column("price", &parser::ProductSchema::get_price,
                                  &parser::ProductSchema::set_price),
          sqlite_orm::make_column("listing_name",
                                  &parser::ProductSchema::get_listing_name,
                                  &parser::ProductSchema::set_listing_name),
          sqlite_orm::make_column(
              "listing_description",
              &parser::ProductSchema::get_listing_description,
              &parser::ProductSchema::set_listing_description),
          sqlite_orm::foreign_key(&parser::ProductSchema::webpage_url)
              .references(&WebpageEntry::url)
              .on_delete.cascade()));

  Database() { storage.sync_schema(); };
  ~Database() = default;
};

} // namespace indexer
