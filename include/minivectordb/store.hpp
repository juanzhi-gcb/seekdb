#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
namespace minivectordb {
// Single-writer, single-thread API. Immutable pages are append-only; no compaction yet.
// All mutations require begin(). Each failed put/erase leaves the prior working root intact.
// Savepoints belong to one Store and one active transaction.
class Store {
public:
  struct Mutation {
    std::int64_t key;
    std::optional<std::string> value; // nullopt erases the key; missing keys are a no-op.
  };
  static constexpr std::size_t max_batch_mutations = 128;
  static constexpr std::size_t max_batch_bytes = 512 * 1024;
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
  std::optional<std::string> get_in(std::uint64_t space, std::int64_t key) const;
  void put_in(std::uint64_t space, std::int64_t key, std::string_view value);
  bool erase_in(std::uint64_t space, std::int64_t key);
  // Atomic working-root update; duplicate keys and oversized batches fail before writing.
  // Payload is bounded by max_batch_bytes, except one individually oversized value.
  void apply_batch_in(std::uint64_t space, std::span<const Mutation> mutations);
  void scan_in(std::uint64_t space, const std::function<void(std::int64_t, std::string_view)>& visitor) const;
  void commit();
  void rollback();
  Savepoint savepoint() const;
  void restore(const Savepoint&);
  // Visits in signed key order. Uses an immutable root snapshot; callback may mutate the active transaction.
  void scan(const std::function<void(std::int64_t, std::string_view)>& visitor) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
