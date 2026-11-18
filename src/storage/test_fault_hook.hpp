#pragma once
// Test-only read fault injection. The implementation lives in
// test_fault_hook.cpp, which CMake links only when MVDB_TEST_FAULTS is ON
// (default: follow BUILD_TESTING). Production builds leave the weak symbol
// undefined, so the guarded call sites compile to a single null check.
extern "C" bool mvdb_test_read_fault() __attribute__((weak));
extern "C" void mvdb_test_set_fault(long at);
extern "C" long mvdb_test_read_calls();
extern "C" long mvdb_test_failed_reads();
