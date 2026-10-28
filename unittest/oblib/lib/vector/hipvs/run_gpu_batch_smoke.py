"""Run real GPU BuildBatch::finalize and public VSAG import in one C++ process."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--cxx-runtime", type=Path)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--skip-negative", action="store_true")
    parser.add_argument("--batch-source", type=Path)
    arguments = parser.parse_args()
    tests = Path(__file__).resolve().parent
    production = tests.parents[4] / "src/oblib/lib/vector"
    arguments.output.mkdir(parents=True, exist_ok=False)
    build = arguments.output / "build"
    runtime = build / "runtime"
    runtime.mkdir(parents=True)
    aio = Path("/usr/lib/x86_64-linux-gnu/libaio.so.1t64")
    if aio.exists() and not Path("/usr/lib/x86_64-linux-gnu/libaio.so.1").exists():
        (runtime / "libaio.so.1").symlink_to(aio)
    environment = os.environ.copy()
    library_paths = [str(runtime)]
    if arguments.cxx_runtime:
        library_paths.append(str(arguments.cxx_runtime))
    library_paths.append(environment.get("LD_LIBRARY_PATH", ""))
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(library_paths)
    if arguments.skip_negative:
        environment["HIPVS_SMOKE_SKIP_NEGATIVE"] = "1"
    sources = [tests / "gpu_batch_smoke.cpp", arguments.batch_source or production / "hipvs_batch.cpp",
        production / "hipvs_hnsw_import.cpp"]
    sdk_library = arguments.sdk / "lib/vsag_lib/libvsag.so"
    command = [arguments.compiler, "-std=c++17", "-O1", "-g", "-fvisibility=hidden",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-pthread", "-I" + str(production),
        "-isystem", str(arguments.sdk / "include"), *(str(path) for path in sources),
        "-L" + str(sdk_library.parent), "-Wl,-rpath," + str(sdk_library.parent),
        "-Wl,-rpath," + str(runtime), "-Wl,-rpath-link," + str(runtime),
        "-lvsag", "-ldl", *arguments.extra_flag, "-o", str(build / "gpu_batch_smoke")]
    tracked = sources + [Path(__file__).resolve(), production / "hipvs_batch.h",
        production / "hipvs_hnsw_import.h", production / "hipvs_bridge_abi.h",
        production.parents[3] / "deps/hipvs/hipvs_bridge.cpp",
        production.parents[3] / "deps/hipvs/CMakeLists.txt"]
    libraries = [sdk_library, arguments.bridge, Path("/opt/hipvs/lib/libcuvs.so"),
        Path("/opt/hipvs/lib/libcuvs_c.so")]
    metadata = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": platform.node(), "platform": platform.platform(),
        "command": command, "compiler": subprocess.check_output([arguments.compiler, "--version"], text=True),
        "sources": {str(path): sha256(path) for path in tracked},
        "libraries": {str(path): sha256(path) for path in libraries},
        "environment": {"LD_LIBRARY_PATH": environment["LD_LIBRARY_PATH"],
            "HIPVS_SMOKE_SKIP_NEGATIVE": environment.get("HIPVS_SMOKE_SKIP_NEGATIVE")},
        "scope": "same-process real AMD GPU BuildBatch::finalize -> public VSAG import/query; SQL not tested",
        "diagnostic_batch_source": str(arguments.batch_source) if arguments.batch_source else None,
        "recall": "64 deterministic independent queries, FP64 exact top10, no acceptance threshold"}
    def save():
        (arguments.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    save()
    compiled = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
    (arguments.output / "build.log").write_text(compiled.stdout)
    print(compiled.stdout, end="", flush=True)
    if compiled.returncode:
        return compiled.returncode
    executable = build / "gpu_batch_smoke"
    metadata["binary_sha256"] = sha256(executable)
    for name, path in [("binary", executable), ("bridge", arguments.bridge)]:
        result = subprocess.run(["ldd", str(path)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=environment)
        metadata["ldd_" + name] = result.stdout
        (arguments.output / ("ldd-" + name + ".log")).write_text(result.stdout)
    for name, path in [("vsag", sdk_library), ("cuvs", Path("/opt/hipvs/lib/libcuvs.so")),
            ("cuvs_c", Path("/opt/hipvs/lib/libcuvs_c.so"))]:
        versions = subprocess.run(["readelf", "--version-info", str(path)], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        (arguments.output / ("versions-" + name + ".log")).write_text(versions.stdout)
    save()
    with (arguments.output / "stdout.log").open("w") as output:
        process = subprocess.Popen([str(executable), str(arguments.bridge)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
        results = []
        for line in process.stdout:
            output.write(line)
            output.flush()
            print(line, end="", flush=True)
            if line.startswith("RESULT_JSON "):
                results.append(json.loads(line[len("RESULT_JSON "):]))
        exit_code = process.wait()
    (arguments.output / "observations.json").write_text(json.dumps(
        {"exit_code": exit_code, "results": results}, indent=2) + "\n")
    if any(sha256(Path(path)) != digest for path, digest in metadata["libraries"].items()):
        raise RuntimeError("a runtime library changed during verification")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
