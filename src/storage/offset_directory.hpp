#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace minivectordb::detail {
// An ordered directory of offsets only. Blocks bound insertion movement while
// keeping keys/offsets contiguous. No reference is retained across ensure/erase.
class OffsetDirectory {
public:
  using Key=std::pair<std::uint64_t,std::int64_t>;
private:
  struct Entry {Key key;std::uint64_t value;};
  struct Block {std::vector<Entry> rows;Block(){rows.reserve(64);}};
  static constexpr std::size_t block_limit=512;
  std::vector<std::unique_ptr<Block>> blocks_;
  std::size_t block_for(Key key) const {
    return std::size_t(std::lower_bound(blocks_.begin(),blocks_.end(),key,
      [](const auto& block,Key k){return block->rows.back().key<k;})-blocks_.begin());
  }
  static auto position(Block& block,Key key) {
    return std::lower_bound(block.rows.begin(),block.rows.end(),key,
      [](const Entry& row,Key k){return row.key<k;});
  }
public:
  std::uint64_t* find(Key key) {
    const auto n=block_for(key);if(n==blocks_.size())return nullptr;
    auto& b=*blocks_[n];const auto at=position(b,key);
    return at!=b.rows.end()&&at->key==key?&at->value:nullptr;
  }
  const std::uint64_t* find(Key key) const {
    return const_cast<OffsetDirectory*>(this)->find(key);
  }
  bool ensure(Key key) {
    auto n=block_for(key);
    if(n==blocks_.size()) {
      if(!blocks_.empty()&&blocks_.back()->rows.size()<block_limit)n=blocks_.size()-1;
      else {
        auto block=std::make_unique<Block>();block->rows.push_back({key,0});
        blocks_.push_back(std::move(block));return true;
      }
    }
    auto* b=blocks_[n].get();auto at=position(*b,key);
    if(at!=b->rows.end()&&at->key==key)return false;
    if(b->rows.size()==block_limit) {
      // Allocate and install the split before shrinking the old block. All
      // throwing allocations leave the original directory logically intact.
      auto right=std::make_unique<Block>();
      right->rows.reserve(block_limit);
      right->rows.assign(b->rows.begin()+block_limit/2,b->rows.end());
      blocks_.insert(blocks_.begin()+std::ptrdiff_t(n+1),std::move(right));
      b->rows.resize(block_limit/2);
      if(b->rows.back().key<key)b=blocks_[n+1].get();
      at=position(*b,key);
    }
    b->rows.insert(at,{key,0});return true;
  }
  void set(Key key,std::uint64_t value){ensure(key);*find(key)=value;}
  void erase(Key key) {
    const auto n=block_for(key);if(n==blocks_.size())return;
    auto& b=*blocks_[n];auto at=position(b,key);
    if(at==b.rows.end()||at->key!=key)return;
    b.rows.erase(at);if(b.rows.empty())blocks_.erase(blocks_.begin()+std::ptrdiff_t(n));
  }
  void purge_zero() {
    for(auto& b:blocks_)b->rows.erase(std::remove_if(b->rows.begin(),b->rows.end(),
      [](const Entry& e){return !e.value;}),b->rows.end());
    blocks_.erase(std::remove_if(blocks_.begin(),blocks_.end(),
      [](const auto& b){return b->rows.empty();}),blocks_.end());
  }
  void swap(OffsetDirectory& other) noexcept {blocks_.swap(other.blocks_);}
  std::vector<std::pair<std::int64_t,std::uint64_t>> snapshot(std::uint64_t space) const {
    std::vector<std::pair<std::int64_t,std::uint64_t>> rows;
    auto n=block_for({space,INT64_MIN});
    for(;n<blocks_.size();++n)for(const auto& e:blocks_[n]->rows) {
      if(e.key.first<space)continue;
      if(e.key.first>space)return rows;
      if(e.value)rows.push_back({e.key.second,e.value});
    }
    return rows;
  }
};
}
