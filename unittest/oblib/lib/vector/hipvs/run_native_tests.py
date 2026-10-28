"""Run HIPVS CPU format, ownership and configuration contracts against repository sources."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
checkout = root.parents[4]
production = Path(os.environ.get("HIPVS_SOURCE_ROOT", checkout / "src/oblib/lib/vector"))
if not (production / "hipvs_batch.cpp").exists():
    raise RuntimeError("actual seekdb HIPVS sources not found: " + str(production))
print("Testing production HIPVS sources:", production, flush=True)
sdk = Path(os.environ.get("VSAG_SDK", "/root/seekdb/deps/3rd/usr/local/oceanbase/deps/devel"))
build = Path(os.environ.get("HIPVS_TEST_BUILD", "/tmp/ob-amd-hipvs-native"))
build.mkdir(parents=True, exist_ok=True)
runtime = build / "runtime"
runtime.mkdir(exist_ok=True)
# Ubuntu's t64 package retains the x86_64 libaio ABI but changed its SONAME.
# Stage a compatibility name only inside this test build, never in system/SDK dirs.
aio = Path("/usr/lib/x86_64-linux-gnu/libaio.so.1t64")
if aio.exists() and not Path("/usr/lib/x86_64-linux-gnu/libaio.so.1").exists():
    alias = runtime / "libaio.so.1"
    if not alias.exists():
        alias.symlink_to(aio)
tests = root
sources = [tests / "hnsw_import_test.cpp", production / "hipvs_hnsw_import.cpp"]
subprocess.run([
    "g++", "-std=c++17", "-O1", "-g", "-fvisibility=hidden", "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-pthread", "-I" + str(production),
    "-isystem", str(tests / "third_party/hnswlib"), "-isystem", str(sdk / "include"),
    *(str(path) for path in sources), "-L" + str(sdk / "lib/vsag_lib"),
    "-Wl,-rpath," + str(sdk / "lib/vsag_lib"), "-Wl,-rpath," + str(runtime),
    "-Wl,-rpath-link," + str(runtime), "-lvsag", "-o", str(build / "hnsw_import_test"),
], check=True)
environment = os.environ.copy()
environment["LD_LIBRARY_PATH"] = str(runtime) + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
subprocess.run([str(build / "hnsw_import_test")], check=True, env=environment)
subprocess.run([
    "g++", "-std=c++17", "-O1", "-g", "-fvisibility=hidden", "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-pthread", "-I" + str(production), "-isystem", str(sdk / "include"),
    str(tests / "batch_test.cpp"), str(production / "hipvs_batch.cpp"),
    str(production / "hipvs_hnsw_import.cpp"), "-L" + str(sdk / "lib/vsag_lib"),
    "-Wl,-rpath," + str(sdk / "lib/vsag_lib"), "-Wl,-rpath," + str(runtime),
    "-Wl,-rpath-link," + str(runtime), "-lvsag", "-ldl", "-o", str(build / "batch_test"),
], check=True)
subprocess.run([str(build / "batch_test")], check=True, env=environment)

options_test = root / "build_options_test.cpp"
subprocess.run([
    "g++", "-std=c++17", "-O1", "-g", "-fvisibility=hidden", "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-pthread", "-I" + str(production), "-isystem", str(sdk / "include"),
    str(options_test), str(production / "hipvs_batch.cpp"), str(production / "hipvs_hnsw_import.cpp"),
    "-L" + str(sdk / "lib/vsag_lib"), "-Wl,-rpath," + str(sdk / "lib/vsag_lib"),
    "-Wl,-rpath," + str(runtime), "-Wl,-rpath-link," + str(runtime), "-lvsag", "-ldl",
    "-o", str(build / "build_options_test"),
], check=True)
subprocess.run([str(build / "build_options_test")], check=True, env=environment)

# TESTONLY synthetic transport/lifecycle coverage; never GPU validation.
subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-I" + str(production),
                str(checkout / "deps/hipvs/hipvs_worker.cpp"), "-ldl", "-o", str(build / "seekdb_hipvs_worker")], check=True)
subprocess.run(["g++", "-std=c++17", "-shared", "-fPIC", "-I" + str(production),
                str(tests / "testonly_worker_bridge.cpp"), "-o", str(build / "testonly_worker_bridge.so")], check=True)
subprocess.run([
    "g++", "-std=c++17", "-O1", "-g", "-fvisibility=hidden", "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-pthread", "-I" + str(production), "-isystem", str(sdk / "include"),
    str(tests / "worker_protocol_test.cpp"), str(production / "hipvs_batch.cpp"), str(production / "hipvs_hnsw_import.cpp"),
    "-L" + str(sdk / "lib/vsag_lib"), "-Wl,-rpath," + str(sdk / "lib/vsag_lib"),
    "-Wl,-rpath," + str(runtime), "-Wl,-rpath-link," + str(runtime), "-lvsag", "-ldl",
    "-o", str(build / "worker_protocol_test"),
], check=True)
subprocess.run([str(build / "worker_protocol_test"), str(build / "testonly_worker_bridge.so"),
                str(build / "seekdb_hipvs_worker")], check=True, env=environment)

subprocess.run([sys.executable, str(tests / "worker_request_test.py"), str(build / "seekdb_hipvs_worker"),
                str(build / "testonly_worker_bridge.so")], check=True, env=environment)
