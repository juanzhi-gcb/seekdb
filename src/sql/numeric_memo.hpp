#pragma once
namespace minivectordb::sql {
inline thread_local bool numeric_memo_enabled=false;
struct NumericMemoScope {
 bool previous;
 explicit NumericMemoScope(bool enabled):previous(numeric_memo_enabled){numeric_memo_enabled=enabled;}
 ~NumericMemoScope(){numeric_memo_enabled=previous;}
 NumericMemoScope(const NumericMemoScope&)=delete;
 NumericMemoScope& operator=(const NumericMemoScope&)=delete;
};
}
