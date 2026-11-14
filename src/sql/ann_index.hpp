#pragma once
#include "minivectordb/exact.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <span>
#include <vector>

namespace minivectordb::sql {
// Two-layer navigable small-world graph. The upper layer is a sparse routing
// layer; layer 0 contains every row. Returned distances still use the reference
// sequential binary64 kernel; the graph only prunes which rows reach it.
class HnswIndex {
  using Candidate=std::pair<double,std::uint32_t>;
  std::size_t count_=0, dimension_=0, degree_=0;
  int top_layer_=0;
  std::uint32_t entry0_=0, entry_top_=0;
  std::vector<std::vector<std::vector<std::uint32_t>>> links_;
  std::vector<std::uint8_t> levels_;
  mutable std::vector<std::uint32_t> visit_;
  mutable std::uint32_t visit_stamp_=0;

  static double squared(std::span<const float> a,std::span<const float> b) noexcept {
    double sum=0;
    for(std::size_t i=0;i<a.size();++i){const double d=double(a[i])-double(b[i]);sum+=d*d;}
    return sum;
  }
  std::span<const float> row(const float* vectors,std::uint32_t index) const noexcept {
    return std::span<const float>(vectors+std::size_t(index)*dimension_,dimension_);
  }
  std::vector<std::uint32_t> search_layer(const float* vectors,int layer,std::span<const float> query,
                                          std::size_t ef,std::uint32_t entry,
                                          std::size_t* scanned=nullptr) const {
    std::vector<std::uint32_t> result;
    if(layer<0||layer>top_layer_||!count_||!dimension_||query.size()!=dimension_)return result;
    ef=std::max<std::size_t>(ef,1);
    if(visit_.size()!=count_)visit_.assign(count_,0);
    if(++visit_stamp_==0){std::fill(visit_.begin(),visit_.end(),0);visit_stamp_=1;}
    const auto stamp=visit_stamp_;
    std::priority_queue<Candidate,std::vector<Candidate>,std::greater<Candidate>> candidates;
    std::priority_queue<Candidate> nearest;
    visit_[entry]=stamp;
    if(scanned)++*scanned;
    auto distance=squared(query,row(vectors,entry));
    candidates.emplace(distance,entry);
    nearest.emplace(distance,entry);
    while(!candidates.empty()) {
      const auto [current,node]=candidates.top();
      candidates.pop();
      if(nearest.size()>=ef&&current>nearest.top().first)break;
      for(const auto neighbor:links_[std::size_t(layer)][node]) {
        if(visit_[neighbor]==stamp)continue;
        visit_[neighbor]=stamp;
        if(scanned)++*scanned;
        const auto d=squared(query,row(vectors,neighbor));
        if(nearest.size()<ef||d<nearest.top().first) {
          candidates.emplace(d,neighbor);
          nearest.emplace(d,neighbor);
          if(nearest.size()>ef)nearest.pop();
        }
      }
    }
    result.reserve(nearest.size());
    while(!nearest.empty()){result.push_back(nearest.top().second);nearest.pop();}
    std::sort(result.begin(),result.end(),[&](auto a,auto b){
      const auto da=squared(query,row(vectors,a)),db=squared(query,row(vectors,b));
      return da<db||(da==db&&a<b);
    });
    return result;
  }
  void prune(const float* vectors,int layer,std::uint32_t node) {
    auto& list=links_[std::size_t(layer)][node];
    if(list.size()<=degree_)return;
    const auto base=row(vectors,node);
    std::sort(list.begin(),list.end(),[&](auto a,auto b){
      const auto da=squared(base,row(vectors,a)),db=squared(base,row(vectors,b));
      return da<db||(da==db&&a<b);
    });
    // HNSW diversity heuristic: drop a candidate that is closer to an already
    // selected neighbor than to the node itself.
    std::vector<std::uint32_t> selected,rejected;
    selected.reserve(degree_);rejected.reserve(list.size());
    for(const auto candidate:list) {
      bool diverse=true;
      const auto candidate_distance=squared(base,row(vectors,candidate));
      for(const auto chosen:selected)
        if(squared(row(vectors,candidate),row(vectors,chosen))<candidate_distance){diverse=false;break;}
      if(diverse)selected.push_back(candidate);else rejected.push_back(candidate);
      if(selected.size()>=degree_)break;
    }
    for(const auto candidate:rejected) {
      if(selected.size()>=degree_)break;
      selected.push_back(candidate);
    }
    list=std::move(selected);
  }
  void connect(const float* vectors,int layer,std::uint32_t node,std::vector<std::uint32_t> candidates) {
    if(candidates.size()>degree_)candidates.resize(degree_);
    for(const auto candidate:candidates)links_[std::size_t(layer)][node].push_back(candidate);
    for(const auto candidate:candidates){links_[std::size_t(layer)][candidate].push_back(node);prune(vectors,layer,candidate);}
    prune(vectors,layer,node);
  }
public:
  void clear() noexcept {count_=dimension_=degree_=0;top_layer_=0;entry0_=entry_top_=0;links_.clear();levels_.clear();visit_.clear();visit_stamp_=0;}
  bool matches(std::size_t count,std::size_t dimension,std::size_t degree) const noexcept {
    return count_&&count_==count&&dimension_==dimension&&degree_==degree;
  }
  bool build(const float* vectors,std::size_t count,std::size_t dimension,std::size_t degree,std::size_t ef_build) {
    clear();
    if(!vectors||!count||!dimension||count<256||degree<2)return false;
    count_=count;dimension_=dimension;degree_=degree;
    const auto effective_build_ef=std::max<std::size_t>(ef_build,degree);
    links_.assign(2,std::vector<std::vector<std::uint32_t>>(count));
    levels_.assign(count,0);
    std::vector<std::uint32_t> order(count);
    std::iota(order.begin(),order.end(),0U);
    std::mt19937 rng(20261114U);
    std::shuffle(order.begin(),order.end(),rng);
    for(const auto node:order)if(rng()%16==0)levels_[node]=1;
    entry0_=order.front();
    entry_top_=entry0_;
    if(levels_[entry0_])top_layer_=1;
    for(std::size_t position=0;position<order.size();++position) {
      const auto node=order[position];
      if(node==entry0_)continue;
      std::uint32_t route=entry0_;
      if(top_layer_>=1) {
        auto upper=search_layer(vectors,1,row(vectors,node),1,entry_top_);
        if(!upper.empty())route=upper.front();
      }
      connect(vectors,0,node,search_layer(vectors,0,row(vectors,node),effective_build_ef,route));
      if(levels_[node]) {
        if(top_layer_<1) {
          top_layer_=1;
          entry_top_=node;
        } else {
          connect(vectors,1,node,search_layer(vectors,1,row(vectors,node),effective_build_ef,entry_top_));
        }
      }
    }
    return true;
  }
  std::vector<Neighbor> search(const std::int64_t* keys,const float* vectors,std::span<const float> query,
                               std::size_t k,std::size_t ef,std::size_t* scanned=nullptr) const {
    std::vector<Neighbor> result;
    if(scanned)*scanned=0;
    if(!k||!keys||!vectors||!count_||query.size()!=dimension_)return result;
    std::uint32_t entry=entry0_;
    if(top_layer_>=1) {
      auto upper=search_layer(vectors,1,query,1,entry_top_,scanned);
      if(!upper.empty())entry=upper.front();
    }
    auto candidates=search_layer(vectors,0,query,std::max(ef,k),entry,scanned);
    const auto take=std::min(k,candidates.size());
    result.reserve(take);
    for(std::size_t i=0;i<take;++i)result.push_back(Neighbor{keys[candidates[i]],l2_distance(query,row(vectors,candidates[i]))});
    const auto better=[](const Neighbor&a,const Neighbor&b){
      return a.distance<b.distance||(a.distance==b.distance&&a.key<b.key);
    };
    std::sort(result.begin(),result.end(),better);
    return result;
  }
};
}
