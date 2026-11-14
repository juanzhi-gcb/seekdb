from pathlib import Path
import shutil,sys
root=Path(sys.argv[1]);own=Path(__file__).parent
def patch(path,changes,include):
 p=root/path;s=p.read_text();s=include+'\n'+s
 for before,after in changes:
  if path=="src/storage/packed_codec.hpp" and before.startswith("inline bool pack_bytes") and "bool compact_words=false" in s:
   before=before.replace("bool planes=false)","bool planes=false,bool compact_words=false)");after=after.replace("bool planes=false)","bool planes=false,bool compact_words=false)")
  if path=="src/storage/journal_store.hpp" and before.startswith("std::string read_framed") and "read_framed" not in s:
   continue # a24 uses unframed reads; report this stage as absent, not zero-cost framed I/O.
  assert s.count(before)==1,(path,before,s.count(before));s=s.replace(before,after)
 p.write_text(s)
for f in ['diagnostic_stages.hpp','diagnostic_trace.cpp']:shutil.copyfile(own/f,root/'src'/f)
patch('src/protocol/jsonl.hpp',[
 ('if(!input.next(line,oversized,limit))break;','if(!input.next(line,oversized,limit))break;mvdb_diag::begin();'),
 ('auto request=Parser(line).parse();','auto request=mvdb_diag::measure(0,[&]{return Parser(line).parse();});'),
 ('sink.row=[&](const Row& row) {','sink.row=[&](const Row& row) {mvdb_diag::Stage timing(6);'),
 ('auto info=db->execute(field("sql"),sink,mode=="ann"?SearchMode::Ann:SearchMode::Exact);','auto info=mvdb_diag::measure(8,[&]{return db->execute(field("sql"),sink,mode=="ann"?SearchMode::Ann:SearchMode::Exact);});'),
 ('            continue;\n        }\n        // A transport','            mvdb_diag::end();continue;\n        }\n        // A transport'),
 ('response.emit(id);','mvdb_diag::measure(7,[&]{response.emit(id);});mvdb_diag::end();')], '#include "../diagnostic_stages.hpp"')
patch('src/sql/executor.cpp',[
 ('const std::function<void(const SelectRow&)>& visit) {','const std::function<void(const SelectRow&)>& visit) {mvdb_diag::Stage fetch_timing(14);'),
 ('auto statement=sql::parse(text);','auto statement=mvdb_diag::measure(1,[&]{return sql::parse(text);});mvdb_diag::classify(statement.kind==sql::Statement::Insert,statement.kind==sql::Statement::Select && text.find("L2_DISTANCE")!=std::string_view::npos);'),
 ('schema=lookup_table(store,statement.table);','schema=mvdb_diag::measure(2,[&]{return lookup_table(store,statement.table);});'),
 ('bound=sql::bind(statement,schema);','bound=mvdb_diag::measure(3,[&]{return sql::bind(statement,schema);});'),
 ('return select(statement,schema,bound,sink);','return mvdb_diag::measure(4,[&]{return select(statement,schema,bound,sink);});')], '#include "../diagnostic_stages.hpp"')
patch('src/vector/exact.cpp',[
 ('bool allow_lower_bound,BoundScreenStats* screen_stats,const Int8BoundCache* integer_cache) {','bool allow_lower_bound,BoundScreenStats* screen_stats,const Int8BoundCache* integer_cache) {mvdb_diag::Stage timing(5);')], '#include "../diagnostic_stages.hpp"')
patch('src/storage/journal_store.hpp',[
 ('static std::uint32_t crc(std::string_view s){','static std::uint32_t crc(std::string_view s){mvdb_diag::Stage crc_timing(17);'),
 ('std::string read_framed(std::uint64_t offset,std::size_t size)const{','std::string read_framed(std::uint64_t offset,std::size_t size)const{mvdb_diag::Stage read_timing(16);'),
 ('void apply(std::uint64_t space,std::span<const Store::Mutation> changes){','void apply(std::uint64_t space,std::span<const Store::Mutation> changes){mvdb_diag::Stage timing(9);')], '#include "../diagnostic_stages.hpp"')
patch('src/storage/packed_codec.hpp',[
 ('inline std::string unpack_bytes(std::string_view input,std::size_t maximum=4064,bool planes=false) {','inline std::string unpack_bytes(std::string_view input,std::size_t maximum=4064,bool planes=false) {mvdb_diag::Stage unpack_timing(15);'),
 ('inline bool pack_bytes(std::string_view input,std::string& output,bool planes=false) {','inline bool pack_bytes(std::string_view input,std::string& output,bool planes=false) {mvdb_diag::Stage timing(10);')], '#include "../diagnostic_stages.hpp"')
patch('src/storage/row_codec.cpp',[
 ('std::string encode_row(const Schema&s,const Row&r) {','std::string encode_row(const Schema&s,const Row&r) {mvdb_diag::Stage timing(11);')], '#include "../diagnostic_stages.hpp"')
with (root/'CMakeLists.txt').open('a') as f:f.write('\ntarget_link_libraries(minivectordb PRIVATE Threads::Threads)\ntarget_sources(minivectordb PRIVATE src/diagnostic_trace.cpp)\ntarget_link_options(minivectordb PRIVATE -Wl,--wrap=fsync -Wl,--wrap=fdatasync -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=sync_file_range)\n')
