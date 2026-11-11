#pragma once
#include "sql.hpp"
#include "../storage/binary.hpp"
#include "minivectordb/profile.hpp"
#include <array>
#include <bit>
#include <cstring>

namespace minivectordb::sql {
// Internal view of bytes owned by a READY ScanSnapshot only. Admission has
// validated every field (including rejected rows) and index/base consistency.
// The executor invalidates that proof before every non-SELECT. Never use this
// view on Store bytes, an incomplete builder, or a caller-owned buffer.
// Keep structural bounds checks; only UTF-8/finiteness proofs are reused.
class SnapshotRow {
    const Schema& schema_;
    std::string_view bytes_;
    std::array<std::size_t,8> small_{};
    std::vector<std::size_t> large_;
    std::size_t offset(std::size_t column) const {
        return large_.empty()?small_.at(column):large_.at(column);
    }
public:
    SnapshotRow(const Schema& schema,std::int64_t key,std::string_view bytes)
        :schema_(schema),bytes_(bytes) {
        if(schema.columns.size()>small_.size())large_.resize(schema.columns.size());
        binary::Reader in(bytes);
        if(in.get(1)!=1 || in.get(4)!=schema.columns.size())
            throw Error(ErrorCode::Corruption,"snapshot row header mismatch");
        for(std::size_t c=0;c<schema.columns.size();++c) {
            (large_.empty()?small_[c]:large_[c])=bytes.size()-in.remaining();
            const auto& field=schema.columns[c];
            switch(field.type) {
                case ColumnType::Bigint: (void)in.take(8);break;
                case ColumnType::Varchar: (void)in.text_view();break;
                case ColumnType::Vector: {
                    const auto count=in.get(4);
                    if(count!=field.extent || count>in.remaining()/sizeof(float))
                        throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                    (void)in.take(static_cast<std::size_t>(count)*sizeof(float));break;
                }
            }
        }
        in.end();
        if(integer(schema.primary)!=key)
            throw Error(ErrorCode::Corruption,"snapshot primary key mismatch");
    }
    std::int64_t integer(std::size_t column) const {
        binary::Reader in(bytes_.substr(offset(column)));
        return std::bit_cast<std::int64_t>(in.get());
    }
    std::string_view text(std::size_t column) const {
        binary::Reader in(bytes_.substr(offset(column)));return in.text_view();
    }
    bool matches(const std::vector<BoundPredicate>& predicates) const {
        for(const auto& predicate:predicates) {
            profile::detail::Scope timer(&profile::Snapshot::predicate_evaluation);
            int comparison;
            if(schema_.columns[predicate.column].type==ColumnType::Bigint) {
                const auto a=integer(predicate.column),b=std::get<std::int64_t>(predicate.value);
                comparison=a<b?-1:(a>b?1:0);
            } else {
                // Both strings are already canonical UTF-8: snapshot admission
                // validates stored text, and the binder validates each literal.
                // Unsigned UTF-8 byte order preserves Unicode scalar order.
                comparison=text(predicate.column).compare(std::get<std::string>(predicate.value));
            }
            const auto& op=predicate.op;
            if((op=="="&&comparison!=0)||(op=="<"&&comparison>=0)||
               (op=="<="&&comparison>0)||(op==">"&&comparison<=0)||
               (op==">="&&comparison<0))return false;
        }
        return true;
    }
    void copy_vector(std::size_t column,std::vector<float>& scratch) const {
        profile::detail::Scope timer(&profile::Snapshot::vector_scratch_copy);
        binary::Reader in(bytes_.substr(offset(column)));
        const auto count=static_cast<std::size_t>(in.get(4));
        const auto data=in.take(count*sizeof(float));
        scratch.resize(count);
        if constexpr(std::endian::native==std::endian::little) {
            if(!data.empty())std::memcpy(scratch.data(),data.data(),data.size());
        } else {
            binary::Reader components(data);
            for(auto& value:scratch)value=std::bit_cast<float>(static_cast<std::uint32_t>(components.get(4)));
        }
        timer.bytes(data.size());
    }
};
}
