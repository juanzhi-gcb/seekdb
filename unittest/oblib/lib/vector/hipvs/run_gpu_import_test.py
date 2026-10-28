"""Validate real GPU exports through the public VSAG importer, without rebuilding fixtures."""
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
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    tests = Path(__file__).resolve().parent
    production = Path(os.environ.get("HIPVS_SOURCE_ROOT", tests.parents[4] / "src/oblib/lib/vector"))
    sdk = Path(os.environ.get("VSAG_SDK", "/root/seekdb/deps/3rd/usr/local/oceanbase/deps/devel"))
    build = Path(os.environ.get("HIPVS_TEST_BUILD", "/tmp/ob-amd-hipvs-gpu-import"))
    build.mkdir(parents=True, exist_ok=True)
    arguments.output.mkdir(parents=True, exist_ok=True)
    runtime = build / "runtime"
    runtime.mkdir(exist_ok=True)
    aio = Path("/usr/lib/x86_64-linux-gnu/libaio.so.1t64")
    if aio.exists() and not Path("/usr/lib/x86_64-linux-gnu/libaio.so.1").exists():
        alias = runtime / "libaio.so.1"
        if not alias.exists():
            alias.symlink_to(aio)
    manifest = json.loads((arguments.exports / "results.json").read_text())
    assert sha256(arguments.exports / "vectors.f32") == manifest["input_sha256"], "GPU original input checksum mismatch"
    input_files = {"vectors.f32": sha256(arguments.exports / "vectors.f32")}
    for record in manifest["records"]:
        if "repetition" not in record:
            continue
        name = f"metric-{record['metric']}-run-{record['repetition']}.hnsw"
        input_files[name] = sha256(arguments.exports / name)
        assert input_files[name] == record["export_sha256"], f"GPU export checksum mismatch: {name}"
        assert record["backend"] == 1 and record["gpu_build_submissions"] == 1 and record["synchronized"] == 1
    assert len(input_files) == 7, "requires all six GPU exports and original vectors"
    library = sdk / "lib/vsag_lib/libvsag.so"
    sources = [tests / "gpu_export_import_test.cpp", production / "hipvs_hnsw_import.cpp"]
    command = [
        "g++", "-std=c++17", "-O1", "-g", "-fvisibility=hidden", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-pthread", "-I" + str(production), "-isystem", str(sdk / "include"),
        *(str(path) for path in sources), "-L" + str(sdk / "lib/vsag_lib"),
        "-Wl,-rpath," + str(sdk / "lib/vsag_lib"), "-Wl,-rpath," + str(runtime),
        "-Wl,-rpath-link," + str(runtime), "-lvsag", "-o", str(build / "gpu_export_import_test"),
    ]
    metadata = {
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "platform": platform.platform(),
        "sdk": str(sdk), "libvsag_sha256": sha256(library), "gpu_bridge_sha256": manifest["bridge_sha256"],
        "compiler": subprocess.check_output(["g++", "--version"], text=True), "command": command,
        "inputs": input_files, "sources": {str(path): sha256(path) for path in
            sources + [production / "hipvs_hnsw_import.h", Path(__file__).resolve()]},
        "scope": "CPU public VSAG imports and queries real AMD GPU exports; no new GPU run; SQL not tested",
        "quality": "64 independent deterministic FP32 queries; FP64 exact top10; no invented Recall threshold",
    }
    (arguments.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    compiled = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (arguments.output / "build.log").write_text(compiled.stdout)
    print(compiled.stdout, end="", flush=True)
    if compiled.returncode:
        return compiled.returncode
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = str(runtime) + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
    linkage = subprocess.check_output(["ldd", str(build / "gpu_export_import_test")], text=True, env=environment)
    metadata["ldd"] = linkage
    metadata["executable_sha256"] = sha256(build / "gpu_export_import_test")
    linked_vsag = [line.split("=>", 1)[1].strip().split()[0]
        for line in linkage.splitlines() if "libvsag.so" in line and "=>" in line]
    if len(linked_vsag) != 1 or Path(linked_vsag[0]).resolve() != library.resolve():
        raise RuntimeError("test executable does not load the selected SDK library: " + linkage)
    (arguments.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    process = subprocess.Popen([str(build / "gpu_export_import_test"), str(arguments.exports)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
    results = []
    with (arguments.output / "stdout.log").open("w") as output:
        for line in process.stdout:
            output.write(line)
            output.flush()
            print(line, end="", flush=True)
            if line.startswith("RESULT_JSON "):
                results.append(json.loads(line[len("RESULT_JSON "):]))
    exit_code = process.wait()
    if sha256(library) != metadata["libvsag_sha256"]:
        raise RuntimeError("SDK library changed during test")
    (arguments.output / "observations.json").write_text(json.dumps(
        {"exit_code": exit_code, "results": results, "metadata": "metadata.json", "stdout": "stdout.log"}, indent=2) + "\n")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
