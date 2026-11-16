// Write-path cost decomposition: same row count, varying vector dimension and
// payload length, so per-float and per-byte costs can be separated from the
// fixed per-statement commit cost. Diagnostic only.
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

static void run(const char* dir, int rows, int dim, int body_len) {
    std::system((std::string("rm -rf ") + dir).c_str());
    std::filesystem::create_directories(dir);
    Options o{std::filesystem::path(dir), 8 * 1024 * 1024};
    o.journal_store = true;
    Database db(std::filesystem::path(dir) / "db.mvdb", o);
    db.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR(" + std::to_string(dim) + "),body VARCHAR(512));");
    std::mt19937 rng(20261110);
    std::uniform_int_distribution<int> c(-2048, 2048);
    std::vector<std::vector<float>> data(rows, std::vector<float>(dim));
    for (auto& r : data) for (auto& x : r) x = float(c(rng)) / 128.0f;
    // Pre-build every statement so the harness formatting is outside the timer.
    std::vector<std::string> sqls;
    for (int a = 0; a < rows; a += 100) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int k = a; k < a + 100 && k < rows; ++k) {
            if (k > a) sql += ',';
            sql += "(" + std::to_string(k) + "," + std::to_string(k % 10) + ",'" + vec(data[k]) + "','";
            sql.append(std::size_t(body_len), char('a' + k % 26)); sql += "')";
        }
        sqls.push_back(sql + ';');
    }
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& s : sqls) db.execute(s);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("dim=%3d body=%3d rows=%d  write=%7.1f ms  per_row=%6.2f us\n", dim, body_len, rows, ms, ms * 1000.0 / rows);
}

int main(int argc, char** argv) {
    int rows = argc > 1 ? std::atoi(argv[1]) : 10000;
    const char* dir = argc > 2 ? argv[2] : "/tmp/mvdb-wdec";
    for (int dim : {4, 16, 128}) for (int bl : {8, 256}) run(dir, rows, dim, bl);
    return 0;
}
