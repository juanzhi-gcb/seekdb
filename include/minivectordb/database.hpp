#pragma once
#include "minivectordb/store.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minivectordb {
using Cell = std::variant<std::int64_t, std::string, std::vector<float>, double>;
using Row = std::vector<Cell>;

enum class ErrorCode { Syntax, Schema, Type, Constraint, Transaction, Corruption };
class Error : public std::runtime_error {
  ErrorCode code_;
public:
  Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
  ErrorCode code() const noexcept { return code_; }
};

struct Options {
  std::filesystem::path sort_directory;
  // Working-set target, excluding the current row/key and allocation/stream overhead.
  std::size_t memory_budget_bytes = 8 * 1024 * 1024;
  bool selective_decode = true;
  bool batch_writes = true;
  std::size_t index_cache_bytes = 1024 * 1024; // Bounded validated INDEX pages; zero disables.
  bool packed_values = true; // Format03 for new files; existing format is preserved.
  std::size_t packed_scan_cache_bytes = 0; // Optional per-scan packed memo; data buffer is the default.
  std::size_t data_cache_bytes = 16 * 1024 * 1024; // Bounded cross-query data pages; zero disables.
};
struct QuerySink {
  std::function<void(const std::vector<std::string>&)> columns;
  std::function<void(const Row&)> row;
};
enum class SearchMode { Exact, Ann };
struct ExecutionInfo {
  std::uint64_t vector_index_entries_examined = 0;
  // SELECT reports emitted rows; DML reports matching rows (including unchanged UPDATEs).
  std::uint64_t affected_rows = 0;
  std::uint64_t rows_examined = 0, distances_computed = 0, spill_runs = 0;
  // Decoded Cells only: includes winner refetch and DML scans, excludes output copies.
  std::uint64_t vector_values_materialized = 0, cells_materialized = 0;
};

// Single-threaded, non-reentrant. Callback references last only during the callback.
// Sink exceptions propagate; partial streamed SELECT output is possible on an error.
// Uncommitted work is rolled back on destruction. I/O exceptions propagate unchanged.
class Database {
public:
  // The parent directory of the database file must already exist.
  explicit Database(const std::filesystem::path&, Options = {});
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ExecutionInfo execute(std::string_view sql, const QuerySink& sink = {}, SearchMode mode = SearchMode::Exact);
  bool in_transaction() const noexcept;
  CacheStats data_cache_stats() const noexcept;
  void reset_data_cache_stats() noexcept;
  CacheStats cache_stats() const noexcept;
  void reset_cache_stats() noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
