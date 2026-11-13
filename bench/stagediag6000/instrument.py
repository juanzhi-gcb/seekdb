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
with (root/'CMakeLists.txt').open('a') as f:f.write('\ntarget_link_libraries(minivectordb PRIVATE Threads::Threads)\ntarget_sources(minivectordb PRIVATE src/diagnostic_trace.cpp)\ntarget_link_options(minivectordb PRIVATE -Wl,--wrap=fsync -Wl,--wrap=pwrite -Wl,--wrap=pread -Wl,--wrap=sync_file_range)\n')

header='#pragma once\n#include <cstdint>\n#include <ctime>\ninline thread_local std::uint64_t mvdb_stage_totals[8]{};\ninline std::uint64_t mvdb_stage_clock(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}\nstruct MvdbStage {unsigned id;std::uint64_t start;explicit MvdbStage(unsigned i):id(i),start(mvdb_stage_clock()){}~MvdbStage(){mvdb_stage_totals[id]+=mvdb_stage_clock()-start;}};\n'
(root/'src/diagnostic_stage.hpp').write_text(header)
def rewrite(name,pairs,inc):
 p=root/name;s=p.read_text();s=inc+s
 for old,new in pairs:
  assert s.count(old)==1,(name,old,s.count(old));s=s.replace(old,new)
 p.write_text(s)
rewrite('src/protocol/jsonl.hpp',[
 ('auto request=Parser(line).parse();','auto request=[&]{MvdbStage stage(0);return Parser(line).parse();}();')
], '#include "../diagnostic_stage.hpp"\n')
rewrite('src/sql/executor.cpp',[
 ('auto statement=options.streaming_sql_tokens?sql::parse_stream(text,true):sql::parse(text);','auto statement=[&]{MvdbStage stage(1);return options.streaming_sql_tokens?sql::parse_stream(text,true):sql::parse(text);}();'),
 ('schema=lookup_table(store,statement.table);','schema=[&]{MvdbStage stage(2);return lookup_table(store,statement.table);}();'),
 ('bound=sql::bind(statement,schema);','bound=[&]{MvdbStage stage(3);return sql::bind(statement,schema);}();'),
 ('if(statement.kind==sql::Statement::Insert)for(const auto&r:bound.insert_rows)if(store.get_in(schema.id,std::get<std::int64_t>(r[schema.primary])))throw Error(ErrorCode::Constraint,"duplicate primary key");','{MvdbStage stage(4);if(statement.kind==sql::Statement::Insert)for(const auto&r:bound.insert_rows)if(store.get_in(schema.id,std::get<std::int64_t>(r[schema.primary])))throw Error(ErrorCode::Constraint,"duplicate primary key");}'),
 ('MutationBatch batch(store,schema.id,options.batch_writes);','MvdbStage stage(5);MutationBatch batch(store,schema.id,options.batch_writes);')
], '#include "../diagnostic_stage.hpp"\n')
p=root/'src/sql/executor.cpp';s=p.read_text();assert s.count('store.commit();')==2;s=s.replace('store.commit();','{MvdbStage stage(7);store.commit();}');p.write_text(s)
rewrite('src/storage/journal_store.hpp',[
 ('void apply(std::uint64_t space,std::span<const Store::Mutation> changes){','void apply(std::uint64_t space,std::span<const Store::Mutation> changes){MvdbStage stage(6);')
], '#include "../diagnostic_stage.hpp"\n')
