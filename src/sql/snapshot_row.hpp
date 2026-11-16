#pragma once
#include "sql.hpp"
#include "../storage/binary.hpp"
#include "minivectordb/profile.hpp"
#include <array>
#include <bit>
#include <cstring>

namespace minivectordb::sql {
// Compile the needed prefix of a fully admitted snapshot row once per query.
// Never apply this projection to unvalidated Store bytes. A rejecting predicate
// can stop early because the complete row was validated when the cache was built.
class SnapshotProjection {
    enum class Kind { Skip, SkipText, SkipVector, Vector, IntegerPredicate, TextPredicate };
    struct Step {Kind kind;std::size_t bytes;};
    std::vector<Step> steps_;
    const BoundPredicate* predicate_=nullptr;
    std::size_t dimension_=0;
    bool usable_=false;
    bool accepts(int comparison) const {
        const auto& op=predicate_->op;
        return !((op=="="&&comparison!=0)||(op=="<"&&comparison>=0)||
                 (op=="<="&&comparison>0)||(op==">"&&comparison<=0)||
                 (op==">="&&comparison<0));
    }
public:
    SnapshotProjection(const Schema& schema,std::size_t vector_column,
                       const std::vector<BoundPredicate>& predicates) {
        if(predicates.size()>1)return; // Preserve the general multi-predicate path.
        if(!predicates.empty())predicate_=&predicates.front();
        dimension_=schema.columns.at(vector_column).extent;
        const auto last=predicate_?std::max(vector_column,predicate_->column):vector_column;
        steps_.push_back({Kind::Skip,5});
        for(std::size_t column=0;column<=last;++column) {
            const auto& field=schema.columns[column];
            if(column==vector_column)steps_.push_back({Kind::Vector,dimension_});
            else if(predicate_&&column==predicate_->column)
                steps_.push_back({field.type==ColumnType::Bigint?Kind::IntegerPredicate:Kind::TextPredicate,0});
            else if(field.type==ColumnType::Varchar)steps_.push_back({Kind::SkipText,0});
            else if(field.type==ColumnType::Vector) {
                // Version-2 rows carry a tag byte and either 12-bit or float32
                // payloads, so the skip length is only known per row.
                steps_.push_back({Kind::SkipVector,field.extent});
            } else {
                const auto bytes=std::size_t(8);
                if(steps_.back().kind==Kind::Skip)steps_.back().bytes+=bytes;
                else steps_.push_back({Kind::Skip,bytes});
            }
        }
        usable_=true;
    }
    bool usable() const noexcept {return usable_;}
    bool matches_and_copy(std::string_view bytes,std::vector<float>& scratch) const {
        binary::Reader in(bytes);
        std::string_view vector_bytes;
        for(const auto& step:steps_)switch(step.kind) {
            case Kind::Skip:(void)in.take(step.bytes);break;
            case Kind::SkipText:(void)in.text_view();break;
            case Kind::SkipVector: {
                const auto n=in.get(4);
                if(n!=step.bytes)throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                const auto tag=in.get(1);
                in.take(tag?std::size_t((n*12+7)/8):std::size_t(n)*sizeof(float));
                break;
            }
            case Kind::Vector: {
                const auto n=in.get(4);
                if(n!=dimension_)throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                const auto packed=in.get(1);
                if(packed) {
                    const std::uint64_t need=(n*12+7)/8;
                    if(need>in.remaining())throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                    const auto raw=in.take(static_cast<std::size_t>(need));
                    scratch.resize(dimension_);
                    std::uint32_t acc=0;unsigned bits=0;std::size_t at=0;
                    for(std::size_t i=0;i<dimension_;++i) {
                        while(bits<12) {acc|=static_cast<std::uint32_t>(static_cast<unsigned char>(raw[at++]))<<bits;bits+=8;}
                        const auto code=static_cast<int>(acc&0xfff);acc>>=12;bits-=12;
                        scratch[i]=static_cast<float>(code-2048)/128.0f;
                    }
                    return true;
                }
                if(n>in.remaining()/sizeof(float))
                    throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                vector_bytes=in.take(std::size_t(n)*sizeof(float));break;
            }
            case Kind::IntegerPredicate: {
                profile::detail::Scope timer(&profile::Snapshot::predicate_evaluation);
                const auto a=std::bit_cast<std::int64_t>(in.get()),b=std::get<std::int64_t>(predicate_->value);
                if(!accepts(a<b?-1:(a>b?1:0)))return false;
                break;
            }
            case Kind::TextPredicate: {
                profile::detail::Scope timer(&profile::Snapshot::predicate_evaluation);
                if(!accepts(in.text_view().compare(std::get<std::string>(predicate_->value))))return false;
                break;
            }
        }
        profile::detail::Scope timer(&profile::Snapshot::vector_scratch_copy);
        scratch.resize(dimension_);
        if constexpr(std::endian::native==std::endian::little) {
            if(!vector_bytes.empty())std::memcpy(scratch.data(),vector_bytes.data(),vector_bytes.size());
        } else {
            binary::Reader components(vector_bytes);
            for(auto& value:scratch)value=std::bit_cast<float>(static_cast<std::uint32_t>(components.get(4)));
        }
        timer.bytes(vector_bytes.size());return true;
    }
};
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
        const auto version=in.get(1);
        if((version!=1&&version!=2) || in.get(4)!=schema.columns.size())
            throw Error(ErrorCode::Corruption,"snapshot row header mismatch");
        for(std::size_t c=0;c<schema.columns.size();++c) {
            (large_.empty()?small_[c]:large_[c])=bytes.size()-in.remaining();
            const auto& field=schema.columns[c];
            switch(field.type) {
                case ColumnType::Bigint: (void)in.take(8);break;
                case ColumnType::Varchar: (void)in.text_view();break;
                case ColumnType::Vector: {
                    const auto count=in.get(4);
                    if(count!=field.extent)
                        throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                    if(version>=2) {
                        const auto tag=in.get(1);
                        const std::uint64_t need=tag?(count*12+7)/8:count*sizeof(float);
                        if(need>in.remaining())throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                        (void)in.take(static_cast<std::size_t>(need));
                    } else {
                        if(count>in.remaining()/sizeof(float))
                            throw Error(ErrorCode::Corruption,"snapshot vector length mismatch");
                        (void)in.take(static_cast<std::size_t>(count)*sizeof(float));
                    }
                    break;
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
