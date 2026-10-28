"""Read-only AMD environment diagnostics. Never prints credentials or full env."""
import argparse
import glob
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def command(arguments):
    executable = shutil.which(arguments[0])
    if executable is None:
        return {"available": False, "command": arguments}
    try:
        result = subprocess.run([executable] + arguments[1:], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=30,
                                universal_newlines=True)
        return {"available": True, "command": arguments, "exit_code": result.returncode,
                "output": result.stdout[:16000]}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"available": True, "command": arguments, "error": str(error)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        help="New JSON report path; refuses to overwrite an existing file")
    args = parser.parse_args()
    report = {"schema_version": 1, "purpose": "environment_check_not_performance",
              "system": platform.system(), "architecture": platform.machine(),
              "cpu_count_visible": os.cpu_count(),
              "devices": {path: os.access(path, os.R_OK | os.W_OK)
                          for path in ["/dev/kfd"] + sorted(glob.glob("/dev/dri/renderD*"))},
              "commands": {"hipcc": command(["hipcc", "--version"]),
                           "rocminfo": command(["rocminfo"]),
                           "amd_smi": command(["amd-smi", "static", "--json"]),
                           "cmake": command(["cmake", "--version"]),
                           "compiler": command(["c++", "--version"])}}
    if not report["commands"]["hipcc"]["available"] and Path("/opt/rocm/bin/hipcc").is_file():
        report["commands"]["hipcc"] = command(["/opt/rocm/bin/hipcc", "--version"])
    report["ready_for_gpu_validation"] = (
        platform.system() == "Linux" and report["devices"].get("/dev/kfd", False)
        and any(path.startswith("/dev/dri/renderD") and allowed
                for path, allowed in report["devices"].items())
        and report["commands"]["hipcc"].get("exit_code") == 0
        and report["commands"]["rocminfo"].get("exit_code") == 0)
    report["limitations"] = ["Device access and tool versions do not prove hipVS was built or executed.",
                             "Compile the bridge and run SQL correctness plus profiler verification separately."]
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(text)
    else:
        print(text, end="")
    return 0 if report["ready_for_gpu_validation"] else 2


if __name__ == "__main__":
    sys.exit(main())
