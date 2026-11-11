#include "minivectordb/database.hpp"
#include "minivectordb/profile.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <unistd.h>
using namespace minivectordb;
#define CHECK(x) do {if(!(x))throw std::runtime_error(std::string(#x)+" line "+std::to_string(__LINE__));} while(false)
struct Result {
    std::vector<std::string> columns;
    std::vector<Row> rows;
    std::optional<ErrorCode> error;
    ExecutionInfo info;
    profile::Snapshot stats;
};
Result run(Database& db,const std::string& sql,SearchMode mode=SearchMode::Exact) {
    Result r;profile::set_enabled(true);profile::reset();
    try {r.info=db.execute(sql,{[&](const auto& c){r.columns=c;},[&](const auto& row){r.rows.push_back(row);}},mode);}
    catch(const Error& e){r.error=e.code();}
    r.stats=profile::snapshot();profile::set_enabled(false);return r;
}
void scenario(const std::filesystem::path& dir,bool indexed,bool selective,std::size_t budget) {
    Options optimized;optimized.typed_scan_cache=false;optimized.selective_decode=selective;
    optimized.scan_snapshot_bytes=budget;
    Options reference=optimized;reference.validated_snapshot_replay=false;
    auto a=std::make_unique<Database>(dir/"optimized",optimized);
    auto b=std::make_unique<Database>(dir/"reference",reference);
    auto both=[&](const std::string& sql,SearchMode mode=SearchMode::Exact) {
        auto x=run(*a,sql,mode),y=run(*b,sql,mode);
        CHECK(x.columns==y.columns);CHECK(x.rows==y.rows);CHECK(x.error==y.error);
        CHECK(x.info.affected_rows==y.info.affected_rows);
        CHECK(x.info.snapshot_bytes<=budget);CHECK(x.info.typed_cache_bytes==0);
        if(x.info.validated_snapshot_rows_examined) {
            CHECK(x.info.validated_snapshot_rows_examined==x.info.snapshot_rows_examined);
            CHECK(x.stats.row_validation.calls+x.stats.row_decode.calls==x.rows.size());
            CHECK(y.stats.row_validation.calls+y.stats.row_decode.calls>x.stats.row_validation.calls+x.stats.row_decode.calls);
        }
        return x;
    };
    both("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(3),g BIGINT,s VARCHAR(100),body VARCHAR(512),a BIGINT,b BIGINT,c BIGINT,d BIGINT)");
    const std::string labels[]{"","a","aa","é","中","😀"};
    std::string insert="INSERT INTO t VALUES ";
    for(int i=0;i<60;++i) {
        if(i)insert+=',';
        insert+='('+std::to_string(i-30)+",'["+std::to_string(i%7)+",1,-2]',"+
            std::to_string(i%3)+",'"+labels[i%6]+"','"+std::string(256,'x')+"',1,2,3,4)";
    }
    both(insert);
    if(indexed)both("CREATE VECTOR INDEX tv ON t(v)");
    auto reads=[&] {
        const std::string filters[]{""," WHERE s='中'"," WHERE s<'é'"," WHERE s<='aa'",
            " WHERE s>'é'"," WHERE s>='😀' AND g>=1"," WHERE s=''"," WHERE s='absent'",
            " WHERE id>=-9223372036854775808 AND s<='😀'"," WHERE id=-1"};
        for(int round=0;round<3;++round)for(const auto& filter:filters) {
            auto r=both("SELECT id,L2_DISTANCE(v,'[2,0,-1]') AS d,s,body FROM t"+filter+" ORDER BY d LIMIT 9",
                        round%2?SearchMode::Ann:SearchMode::Exact);
            if(round==2 && filter.empty())CHECK((r.info.validated_snapshot_rows_examined>0)==(budget==32768));
        }
    };
    reads();both("BEGIN");both("UPDATE t SET s='é',v='[0,0,0]' WHERE id=-1");reads();
    both("INSERT INTO t VALUES(-1,'[0,0,0]',1,'x','x',1,2,3,4)");reads();
    both("ROLLBACK");reads();both("BEGIN");both("DELETE FROM t WHERE s='中'");reads();
    both("COMMIT");reads();both("UPDATE t SET absent=1");reads();
    both("ROLLBACK");reads();
    // Reopening must rebuild proofs rather than using stale process-local views.
    a.reset();b.reset();
    a=std::make_unique<Database>(dir/"optimized",optimized);
    b=std::make_unique<Database>(dir/"reference",reference);
    reads();
}
int main() {
    try {
        const auto base=std::filesystem::path(std::getenv("MVDB_TEST_DIR")?std::getenv("MVDB_TEST_DIR"):"/tmp");
        std::filesystem::create_directories(base);
        auto root=(base/"validated-snapshot-XXXXXX").string();CHECK(::mkdtemp(root.data()));
        for(bool index:{false,true})for(bool selective:{false,true})for(auto budget:{0U,512U,32768U}) {
            auto path=std::filesystem::path(root)/(std::to_string(index)+std::to_string(selective)+std::to_string(budget));
            std::filesystem::create_directory(path);scenario(path,index,selective,budget);
        }
        std::filesystem::remove_all(root);
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
