#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
namespace minivectordb {
// Single-writer, single-thread API. Immutable pages are append-only; no compaction yet.
// All mutations require begin(). Each failed put/erase leaves the prior working root intact.
// Savepoints belong to one Store and one active transaction.
class Store {
public:
  class Savepoint {
    friend class Store;
    std::uint64_t root_ = 0, epoch_ = 0, owner_ = 0;
  };
  explicit Store(const std::filesystem::path& path);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  void begin();
  std::optional<std::string> get(std::int64_t key) const;
  void put(std::int64_t key, std::string_view value);
  bool erase(std::int64_t key);
  void commit();
  void rollback();
  Savepoint savepoint() const;
  void restore(const Savepoint&);
  // Visits in signed key order. Callback must not mutate this Store.
  void scan(const std::function<void(std::int64_t, std::string_view)>& visitor) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
