// Server-side stage profile for the local 10k x 128 exact workload.
// Not a competition score: this only prints inclusive stage timings collected by
// minivectordb::profile while the harness times the same 1000 queries end to end.
#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace minivectordb;

static std::string vec(const std::vector<float>& v) {
    std::string s = "[";
    char buf[32];
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%.10g", static_cast<double>(v[i]));
        if (i) s += ',';
        s += buf;
    }
    s += ']';
    return s;
}

int main(int argc, char** argv) {
    const int rows = argc > 1 ? std::atoi(argv[1]) : 10000;
    const int queries = argc > 2 ? std::atoi(argv[2]) : 1000;
    const char* dir = argc > 3 ? argv[3] : "/tmp/mvdb-profile";
    std::system((std::string("rm -rf ") + dir).c_str());
    std::filesystem::create_directories(dir);

    Options options{std::filesystem::path(dir), 8 * 1024 * 1024};
    options.journal_store = true;
    Database db(std::filesystem::path(dir) / "db.mvdb", options);

    std::mt19937 rng(20261110);
    std::uniform_int_distribution<int> comp(-2048, 2048);
    std::vector<std::vector<float>> data(rows, std::vector<float>(128));
    for (auto& row : data) for (auto& x : row) x = static_cast<float>(comp(rng)) / 128.0f;
    std::vector<int> order(rows); for (int i = 0; i < rows; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);

    db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(128),body VARCHAR(512));");
    profile::reset();
    profile::set_enabled(true);
    const auto write_begin = std::chrono::steady_clock::now();
    for (int a = 0; a < rows; a += 100) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int k = a; k < a + 100 && k < rows; ++k) {
            const int id = order[k];
            if (k > a) sql += ',';
            sql += "(" + std::to_string(id) + "," + std::to_string(id % 10) + ",'" + vec(data[id]) + "','";
            sql += "record-" + std::to_string(id) + "-";
            sql.append(256, static_cast<char>('a' + id % 26));
            sql += "')";
        }
        sql += ';';
        db.execute(sql);
    }
    const double write_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - write_begin).count();
    profile::set_enabled(false);
    const auto w = profile::snapshot();
    std::printf("WRITE rows=%d ms=%.1f pwrite_calls=%llu pwrite_ms=%.1f bytes=%llu fsync_calls=%llu fsync_ms=%.1f\n",
                rows, write_ms,
                static_cast<unsigned long long>(w.pwrite.calls), w.pwrite.nanoseconds/1e6,
                static_cast<unsigned long long>(w.pwrite.bytes),
                static_cast<unsigned long long>(w.fsync.calls), w.fsync.nanoseconds/1e6);

    std::vector<std::string> sqls;
    sqls.reserve(static_cast<std::size_t>(queries));
    for (int j = 0; j < queries; ++j) {
        std::vector<float> q(128);
        for (auto& x : q) x = static_cast<float>(comp(rng)) / 128.0f;
        std::string sql = "SELECT id,L2_DISTANCE(v,'" + vec(q) + "') AS d,body FROM t";
        if (j % 2) sql += " WHERE g=" + std::to_string((j / 2) % 10);
        sql += " ORDER BY d LIMIT 10;";
        sqls.push_back(std::move(sql));
    }
    for (int j = 0; j < 10 && !sqls.empty(); ++j) {
        QuerySink sink;
        std::size_t emitted = 0;
        sink.row = [&](const Row&) { ++emitted; };
        db.execute(sqls[static_cast<std::size_t>(j)], sink);
    }

    profile::reset();
    profile::set_enabled(true);
    const auto query_begin = std::chrono::steady_clock::now();
    std::uint64_t emitted_rows = 0;
    for (const auto& sql : sqls) {
        QuerySink sink;
        sink.row = [&](const Row&) { ++emitted_rows; };
        db.execute(sql, sink);
    }
    const double query_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - query_begin).count();
    profile::set_enabled(false);
    const auto s = profile::snapshot();

    auto line = [](const char* name, const profile::Metric& m, double total) {
        std::printf("%-22s calls=%8llu  %9.1f ms  %5.1f%%  per-call=%7.1f us  bytes=%llu\n", name,
                    static_cast<unsigned long long>(m.calls), m.nanoseconds / 1e6,
                    total > 0 ? 100.0 * (m.nanoseconds / 1e6) / total : 0.0,
                    m.calls ? m.nanoseconds / 1e3 / static_cast<double>(m.calls) : 0.0,
                    static_cast<unsigned long long>(m.bytes));
    };
    std::printf("rows=%d queries=%d emitted=%llu\n", rows, queries, static_cast<unsigned long long>(emitted_rows));
    std::printf("write_ms=%.1f query_ms=%.1f\n", write_ms, query_ms);
    line("table_scan", s.table_scan, query_ms);
    line("scan_consume", s.scan_consume, query_ms);
    line("vector_distance", s.vector_distance, query_ms);
    line("predicate_evaluation", s.predicate_evaluation, query_ms);
    line("vector_scratch_copy", s.vector_scratch_copy, query_ms);
    line("row_validation", s.row_validation, query_ms);
    line("row_decode", s.row_decode, query_ms);
    line("row_materialization", s.row_materialization, query_ms);
    line("pread", s.pread, query_ms);
    line("pwrite", s.pwrite, query_ms);
    line("fsync", s.fsync, query_ms);
    line("page_checksum", s.page_checksum, query_ms);
    return 0;
}
