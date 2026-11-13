#pragma once
#include "int8_bound.hpp"
#include "minivectordb/exact.hpp"
#include <optional>
#include <queue>
#include <type_traits>

namespace minivectordb::detail {
struct CandidateStats {std::size_t examined=0,selected=0,exact=0;bool certified=false;};
// Source is an owned immutable typed cache throughout this call. No temporary
// visitor spans survive their callback. Screening never supplies result values:
// all returned distances use the reference sequential binary64 computation.
template<class Predicate,class KeyAt,class VectorAt>
std::optional<std::vector<Neighbor>> certified_candidates(
    const Int8BoundCache& cache,std::span<const float> query,std::size_t begin,
    std::size_t end,std::size_t k,Predicate&& predicate,KeyAt&& key_at,
    VectorAt&& vector_at,CandidateStats& stats,bool entire_interval=false) {
  stats={};
#ifndef MVDB_CERTIFICATE_CAPACITY
#define MVDB_CERTIFICATE_CAPACITY 32
#endif
  constexpr std::size_t capacity=MVDB_CERTIFICATE_CAPACITY;
  static_assert(capacity>=16 && capacity<=256);
  if(!k||k>16||end<begin||end-begin<256||cache.uniform())return {};
  const auto encoded=cache.query(query,true);if(!encoded.valid)return {};
  const auto run=[&]<class Score>() -> std::optional<std::vector<Neighbor>> {
  constexpr bool integral=std::is_integral_v<Score>;
  constexpr Score unknown=integral?std::numeric_limits<Score>::lowest():-std::numeric_limits<Score>::infinity();
  struct Candidate {Score lower;std::size_t row;};
  const auto less=[](const Candidate&a,const Candidate&b){return a.lower<b.lower||(a.lower==b.lower&&a.row<b.row);};
  std::vector<Candidate> candidates;candidates.reserve(capacity);
  Score excluded=integral?std::numeric_limits<Score>::max():std::numeric_limits<Score>::infinity();
  for(std::size_t block=begin/16*16;block<end;block+=16){
    std::array<Score,16> lower{};bool valid;
    if constexpr(integral)valid=cache.integer_scores(encoded,block,lower);
    else valid=cache.lower_scores(encoded,block,lower);
    if(!valid)lower.fill(unknown);
    if(entire_interval && block>=begin && end-block>=16 && candidates.size()==capacity) {
      const Score minimum=*std::min_element(lower.begin(),lower.end());
      if(minimum!=unknown && std::isfinite(minimum) && minimum>=candidates.front().lower) {
        excluded=std::min(excluded,minimum);stats.examined+=16;stats.selected+=16;continue;
      }
    }
    for(auto row=std::max(begin,block);row<std::min(end,block+16);++row){
      ++stats.examined;if(!predicate(row))continue;++stats.selected;
      Candidate item{lower[row-block],row};
      // Non-finite/unknown bounds cannot justify rejecting a row.
      if constexpr(!integral)if(!std::isfinite(item.lower))item.lower=unknown;
      if(candidates.size()<capacity){candidates.push_back(item);std::push_heap(candidates.begin(),candidates.end(),less);}
      else if(less(item,candidates.front())){
        excluded=std::min(excluded,candidates.front().lower);
        std::pop_heap(candidates.begin(),candidates.end(),less);candidates.back()=item;std::push_heap(candidates.begin(),candidates.end(),less);
      }else excluded=std::min(excluded,item.lower);
    }
  }
  std::sort(candidates.begin(),candidates.end(),less);
  const auto better=[](const Neighbor&a,const Neighbor&b){return a.distance<b.distance||(a.distance==b.distance&&a.key<b.key);};
  std::vector<Neighbor> exact;exact.reserve(k);std::vector<float> scratch;
  for(std::size_t i=0;i<candidates.size();++i){
    const auto& c=candidates[i];Neighbor item{key_at(c.row),l2_distance(query,vector_at(c.row,scratch))};++stats.exact;
    if(exact.size()<k){exact.push_back(item);std::push_heap(exact.begin(),exact.end(),better);}
    else if(better(item,exact.front())){std::pop_heap(exact.begin(),exact.end(),better);exact.back()=item;std::push_heap(exact.begin(),exact.end(),better);}
    const auto pending=i+1<candidates.size()?std::min(excluded,candidates[i+1].lower):excluded;
    const double lower=[&]{if constexpr(integral)return cache.integer_lower(encoded,pending);else return pending;}();
    if(exact.size()==k&&lower>cache.exclusion_upper(encoded,exact.front().distance)){
      std::sort(exact.begin(),exact.end(),better);stats.certified=true;return exact;
    }
  }
  if(stats.selected<=capacity){std::sort(exact.begin(),exact.end(),better);stats.certified=true;return exact;}
  return {}; // Caller runs the complete unchanged exact path on failure.
  };
  if(encoded.integral)return run.template operator()<std::int32_t>();
  return run.template operator()<double>();
}
}
