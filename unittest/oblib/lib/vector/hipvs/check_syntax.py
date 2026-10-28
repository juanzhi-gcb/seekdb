"""Check the current vector Unity group using an existing seekdb compile database."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

checkout = Path(__file__).resolve().parents[5]
database = Path(os.environ.get("SEEKDB_COMPILE_COMMANDS", "/root/seekdb/build_debug/compile_commands.json"))
entries = json.loads(database.read_text())
entry = next(item for item in entries if "unity_oblib_lib_ob_vector_util" in item["file"])
args = entry.get("arguments") or shlex.split(entry["command"])
clean = []
i = 0
while i < len(args):
    if args[i] in ("-o", "-c"):
        i += 2
    else:
        clean.append(args[i])
        i += 1
# Put current checkout headers ahead of the installed checkout, retaining its generated dependencies.
clean[1:1] = ["-I" + str(checkout / "src/oblib"), "-I" + str(checkout / "src/oblib/common")]
with tempfile.TemporaryDirectory(prefix="seekdb-hipvs-syntax-") as directory:
    unity = Path(directory) / "vector_unity.cpp"
    unity.write_text("".join('#include "' + str(checkout / "src/oblib/lib/vector" / name) + '"\n'
        for name in ("ob_vector_util.cpp", "ob_vsag_adaptor.cpp", "hipvs_hnsw_import.cpp", "hipvs_batch.cpp")))
    subprocess.run(clean + ["-fsyntax-only", str(unity)], check=True, cwd=entry["directory"])
print("PASS: current vector Unity group syntax with installed seekdb toolchain and SDK")
