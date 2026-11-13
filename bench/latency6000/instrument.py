from pathlib import Path
import sys,shutil
root=Path(sys.argv[1]);source=Path(sys.argv[2])
shutil.copyfile(source,root/'src/diagnostic_trace.cpp')
p=root/'src/protocol/jsonl.hpp';s=p.read_text();s=s.replace('#pragma once','#pragma once\nextern "C" void mvdb_diag_begin();\nextern "C" void mvdb_diag_end();',1)
s=s.replace('if(!input.next(line,oversized,limit))break;','if(!input.next(line,oversized,limit))break;\n        mvdb_diag_begin();')
s=s.replace('response.emit(id);','response.emit(id);\n        mvdb_diag_end();')
s=s.replace('            continue;\n        }\n        // A transport','            mvdb_diag_end();continue;\n        }\n        // A transport')
assert s.count('mvdb_diag_begin();')==2 and s.count('mvdb_diag_end();')==3
p.write_text(s)
with (root/'CMakeLists.txt').open('a') as f:f.write('\ntarget_sources(minivectordb PRIVATE src/diagnostic_trace.cpp)\ntarget_link_options(minivectordb PRIVATE -Wl,--wrap=fsync -Wl,--wrap=pwrite -Wl,--wrap=pread)\n')
