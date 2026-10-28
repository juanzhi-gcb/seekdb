# HIPVS import and DDL batch contracts

These Linux CPU tests compile the repository's production modules in
`src/oblib/lib/vector`. They require an existing seekdb VSAG SDK and do not
require ROCm, a GPU or a running SQL server. They do **not** establish GPU
execution, SQL integration, benchmark Recall or performance acceptance.

Run from any directory:

```sh
VSAG_SDK=/path/to/devel python3 /path/to/seekdb/unittest/oblib/lib/vector/hipvs/run_native_tests.py
```

`HIPVS_TEST_BUILD` selects an isolated output directory (default
`/tmp/ob-amd-hipvs-native`). `HIPVS_SOURCE_ROOT` can explicitly select the
production vector directory. The runner stages a local libaio compatibility
symlink only when Ubuntu's t64 package provides the same ABI under another
SONAME; it never modifies the SDK or system directories.

A standalone CMake alternative is also available:

```sh
cmake -S unittest/oblib/lib/vector/hipvs -B /tmp/seekdb-hipvs-tests -DVSAG_SDK=/path/to/devel
cmake --build /tmp/seekdb-hipvs-tests -j2
ctest --test-dir /tmp/seekdb-hipvs-tests --output-on-failure
```

For current vector Unity syntax using an already configured seekdb checkout:

```sh
SEEKDB_COMPILE_COMMANDS=/path/to/build/compile_commands.json python3 unittest/oblib/lib/vector/hipvs/check_syntax.py
```

This preserves the configured toolchain/flags and generated dependency
headers, placing this checkout's current oblib headers first. It checks the
four-source Unity group, including the modified vector wrappers and adaptor;
it is not a full observer or SQL DDL build.

The local verification SDK package is
`devdeps-vsag-abiv1-1.1.0-1752026041310.el8.x86_64.rpm`.
The locally linked library reports revision `012a0f2`; the import test prints it on every run. Vendored CPU fixture
headers are upstream hnswlib; their Apache license is in
`third_party/hnswlib/LICENSE`. Fixture graph construction is CPU-only.

Coverage includes shuffled internal-node/ordinal-label order, final VIDs that
collide with dense ordinal labels, raw IP distances against brute-force dot
products for all VIDs, native-versus-imported ANN agreement, a well-conditioned
IP top-1 fixture against exact truth, VSAG filters, persistence/reload, CPU
incremental Add, invalid graph rejection, input deep copy, checked environment
limits and retained inputs after finalization failure. Nonmetric IP can have
native ANN misses even at ef=N; the test prints observed misses and verifies
that import preserves native behavior. This is not a Recall acceptance test.

The default 1 GiB input limit bounds owned vector/VID capacity, and the default
1 GiB export limit bounds serialized bytes copied from the bridge. Neither
bounds total process RSS, temporary graph/import allocations, ROCm allocations
or combined peak host/device memory. The handler's memory estimate includes
reserved vector/VID capacity in addition to the VSAG estimate.
