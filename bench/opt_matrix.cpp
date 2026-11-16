// Local option matrix: same workload, different Options, uninstrumented timing.
// Diagnostic only; never a competition score.
#include "minivectordb/database.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace minivectordb;

static std::string vec(const std::vector<float>& v) {
    std::string s = "["; char b[32];
    for (std::size_t i = 0; i < v.size(); ++i) { std::snprintf(b, sizeof b, "%.10g", double(v[i])); if (i) s += ','; s += b; }
    return s + "]";
}

struct Variant { const char* name; void (*apply)(Options&); };

static double g_wall_write = 0, g_wall_query = 0;

static void run(const char* dir, const Variant& v, int rows, int queries) {
    std::system((std::string("rm -rf ") + dir).c_str());
    std::filesystem::create_directories(dir);
    Options o{std::filesystem::path(dir), 8 * 1024 * 1024};
    o.journal_store = true;
    v.apply(o);
    std::mt19937 rng(20261110);
    std::uniform_int_distribution<int> comp(-2048, 2048);
    std::vector<std::vector<float>> data(rows, std::vector<float>(128));
    for (auto& r : data) for (auto& x : r) x = float(comp(rng)) / 128.0f;
    std::vector<int> order(rows); for (int i = 0; i < rows; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    {
        Database db(std::filesystem::path(dir) / "db.mvdb", o);
        db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(128),body VARCHAR(512));");
        auto t0 = std::chrono::steady_clock::now();
        for (int a = 0; a < rows; a += 100) {
            std::string sql = "INSERT INTO t VALUES ";
            for (int k = a; k < a + 100 && k < rows; ++k) {
                int id = order[k];
                if (k > a) sql += ',';
                sql += "(" + std::to_string(id) + "," + std::to_string(id % 10) + ",'" + vec(data[id]) + "','record-" + std::to_string(id) + "-";
                sql.append(256, char('a' + id % 26)); sql += "')";
            }
            db.execute(sql + ';');
        }
        g_wall_write = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::vector<std::string> sqls; sqls.reserve(queries);
        for (int j = 0; j < queries; ++j) {
            std::vector<float> q(128); for (auto& x : q) x = float(comp(rng)) / 128.0f;
            std::string s = "SELECT id,L2_DISTANCE(v,'" + vec(q) + "') AS d,body FROM t";
            if (j % 2) s += " WHERE g=" + std::to_string((j / 2) % 10);
            sqls.push_back(s + " ORDER BY d LIMIT 10;");
        }
        QuerySink warm; warm.row = [](const Row&) {};
        for (int j = 0; j < 10; ++j) db.execute(sqls[std::size_t(j)], warm);
        auto t1 = std::chrono::steady_clock::now();
        std::uint64_t seen = 0;
        for (const auto& s : sqls) { QuerySink sink; sink.row = [&](const Row&) { ++seen; }; db.execute(s, sink); }
        g_wall_query = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        std::printf("%-22s write=%7.1f ms  query=%7.1f ms  rows_out=%llu\n", v.name, g_wall_write, g_wall_query,
                    static_cast<unsigned long long>(seen));
    }
}

int main(int argc, char** argv) {
    int rows = argc > 1 ? std::atoi(argv[1]) : 10000;
    int queries = argc > 2 ? std::atoi(argv[2]) : 1000;
    const char* dir = argc > 3 ? argv[3] : "/tmp/mvdb-matrix";
    const Variant variants[] = {
        {"default", [](Options&) {}},
        {"no_typed_cache", [](Options& o) { o.typed_scan_cache = false; }},
        {"no_transposed", [](Options& o) { o.transposed_scan_cache = false; }},
        {"no_clustered", [](Options& o) { o.clustered_scan_cache = false; }},

        {"no_projection", [](Options& o) { o.projection_cache_bytes = 0; }},
        {"no_int8_screen", [](Options& o) { o.bounded_int8_screen = false; }},
        {"scalar_kernel", [](Options& o) { o.batched_vector_kernel = false; }},
        {"no_batch_writes", [](Options& o) { o.batch_writes = false; }},
        {"no_selective_decode", [](Options& o) { o.selective_decode = false; }},
        {"raw_scan", [](Options& o) { o.typed_scan_cache = false; o.table_scan_snapshot = false; o.projection_cache_bytes = 0; }},
    };
    for (const auto& v : variants) run(dir, v, rows, queries);
    return 0;
}
