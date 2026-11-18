#include <atomic>
#include <cerrno>
namespace {
std::atomic<long> g_at{0};
std::atomic<long> g_calls{0};
std::atomic<long> g_failed{0};
}
extern "C" bool mvdb_test_read_fault() {
  if(g_at.load(std::memory_order_relaxed)<=0) return false;
  const long n=++g_calls;
  if(n>=g_at.load(std::memory_order_relaxed)) {++g_failed;return true;}
  return false;
}
extern "C" void mvdb_test_set_fault(long at) {
  g_calls.store(0);g_failed.store(0);g_at.store(at);
}
extern "C" long mvdb_test_read_calls() {return g_calls.load();}
extern "C" long mvdb_test_failed_reads() {return g_failed.load();}
