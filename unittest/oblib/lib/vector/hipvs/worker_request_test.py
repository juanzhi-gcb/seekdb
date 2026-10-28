"""TESTONLY malformed request and safe-file tests; this suite does not use a GPU."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

worker, bridge = sys.argv[1:]
request_format = "<QIIIIQIIIIiIQQ"
good = [0x3151455253565048, 1, 72, 40, 2, 1, 8, 16, 1, 1, 0, 0, 8, 128]

def run_case(name, values=None, suffix=None, kind=None):
    with tempfile.TemporaryDirectory(prefix="seekdb-hipvs-testonly-request-") as directory:
        root = Path(directory)
        request, response = root / "request", root / "response"
        payload = struct.pack(request_format, *(good if values is None else values))
        payload += struct.pack("<ff", 1.25, -2.5) if suffix is None else suffix
        request.write_bytes(payload)
        response.touch()
        os.chmod(request, 0o600)
        os.chmod(response, 0o600)
        if kind == "symlink_request":
            request.rename(root / "target")
            request.symlink_to(root / "target")
        if kind == "symlink_response":
            response.unlink()
            (root / "target").write_bytes(b"unchanged")
            response.symlink_to(root / "target")
        if kind == "fifo_request":
            request.unlink()
            os.mkfifo(request, 0o600)
        if kind == "fifo_response":
            response.unlink()
            os.mkfifo(response, 0o600)
        if kind == "permissions":
            os.chmod(request, 0o644)
        result = subprocess.run([worker, bridge, str(request), str(response)], timeout=5, capture_output=True)
        if kind in {"symlink_response", "fifo_response"}:
            assert result.returncode != 0, name
            if kind == "symlink_response":
                assert (root / "target").read_bytes() == b"unchanged"
        else:
            assert result.returncode == 0, (name, result.stderr)
            data = response.read_bytes()
            assert len(data) == 1344, (name, len(data))
            magic, version, size, status, reserved, output = struct.unpack_from("<QIIIIQ", data)
            assert (magic, version, size, status, reserved, output) == (0x3150535253565048, 1, 1344, 1, 0, 0), name
            assert data[32:320] == bytes(288), name
            assert b"\0" in data[320:] and data[320] != 0, name
        print("PASS TESTONLY request:", name)

for field, value, name in [(0, 0, "magic"), (1, 2, "version"), (2, 71, "header"),
                            (3, 0, "config_size"), (4, 0, "dimension"), (5, 0, "rows"),
                            (5, 2**64 - 1, "rows_overflow"), (8, 3, "metric"),
                            (9, 0, "threads"), (10, -1, "device"), (11, 1, "reserved"),
                            (12, 9, "input_length"), (13, 0, "zero_export"),
                            (13, 2**64 - 1, "export_overflow")]:
    values = good.copy()
    values[field] = value
    run_case(name, values)
run_case("truncated", suffix=b"\0")
run_case("trailing_bytes", suffix=struct.pack("<ff", 1.25, -2.5) + b"x")
run_case("nonfinite", suffix=struct.pack("<ff", float("nan"), -2.5))
for kind in ["symlink_request", "symlink_response", "fifo_request", "fifo_response", "permissions"]:
    run_case(kind, kind=kind)
