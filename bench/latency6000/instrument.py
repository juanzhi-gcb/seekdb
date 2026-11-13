from pathlib import Path
import sys,shutil
root=Path(sys.argv[1]);source=Path(sys.argv[2])
shutil.copyfile(source,root/'src/diagnostic_trace.cpp')
shutil.copyfile(source.with_name('diagnostic_stages.hpp'),root/'src/diagnostic_stages.hpp')
p=root/'src/protocol/jsonl.hpp';s=p.read_text();s=s.replace('#pragma once','#pragma once\nextern "C" void mvdb_diag_begin();\nextern "C" void mvdb_diag_end();',1)
s=s.replace('if(!input.next(line,oversized,limit))break;','if(!input.next(line,oversized,limit))break;\n        mvdb_diag_begin();')
s=s.replace('response.emit(id);','response.emit(id);\n        mvdb_diag_end();')
s=s.replace('            continue;\n        }\n        // A transport','            mvdb_diag_end();continue;\n        }\n        // A transport')
assert s.count('mvdb_diag_begin();')==2 and s.count('mvdb_diag_end();')==3
s=s.replace('#pragma once','#pragma once\n#include "../diagnostic_stages.hpp"',1)
s=s.replace('auto request=Parser(line).parse();','auto request=mvdb_diag::measure(0,[&]{return Parser(line).parse();});')
s=s.replace('response.emit(id);','mvdb_diag::measure(4,[&]{response.emit(id);});')
s=s.replace('auto info=db->execute(field("sql"),sink,mode=="ann"?SearchMode::Ann:SearchMode::Exact);','auto info=mvdb_diag::measure(5,[&]{return db->execute(field("sql"),sink,mode=="ann"?SearchMode::Ann:SearchMode::Exact);});')
s=s.replace('sink.row=[&](const Row& row) {','sink.row=[&](const Row& row) {mvdb_diag::Stage row_format(6);')
p.write_text(s)
p=root/'src/sql/executor.cpp';s=p.read_text();s='#include "../diagnostic_stages.hpp"\n'+s
s=s.replace('auto statement=sql::parse(text);','auto statement=mvdb_diag::measure(1,[&]{return sql::parse(text);});')
s=s.replace('bound=sql::bind(statement,schema);','bound=mvdb_diag::measure(2,[&]{return sql::bind(statement,schema);});')
s=s.replace('return select(statement,schema,bound,sink);','return mvdb_diag::measure(3,[&]{return select(statement,schema,bound,sink);});')
p.write_text(s)
with (root/'CMakeLists.txt').open('a') as f:f.write('\ntarget_sources(minivectordb PRIVATE src/diagnostic_trace.cpp)\ntarget_link_options(minivectordb PRIVATE -Wl,--wrap=fsync -Wl,--wrap=pwrite -Wl,--wrap=pread)\n')
