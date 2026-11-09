#include "minivectordb/exact.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>
namespace minivectordb {
double l2_distance(std::span<const float> a,std::span<const float> b) {
  if(a.size()!=b.size()) throw std::invalid_argument("VECTOR dimension mismatch");
  double squared=0;
  for(std::size_t i=0;i<a.size();++i) {
    if(!std::isfinite(a[i]) || !std::isfinite(b[i])) throw std::invalid_argument("non-finite VECTOR component");
    const double difference=static_cast<double>(a[i])-static_cast<double>(b[i]);
    squared+=difference*difference;
  }
  return std::sqrt(squared);
}
std::vector<Neighbor> exact_top_k(std::span<const float> query,std::size_t k,const VectorSource& source,const KeyPredicate& predicate) {
  for(float component:query) if(!std::isfinite(component)) throw std::invalid_argument("non-finite query component");
  auto better=[](const Neighbor& a,const Neighbor& b) {return a.distance<b.distance || (a.distance==b.distance && a.key<b.key);};
  std::priority_queue<Neighbor,std::vector<Neighbor>,decltype(better)> heap(better);
  if(k==0) return {};
  source([&](std::int64_t key,std::span<const float> value) {
    if(predicate && !predicate(key)) return;
    Neighbor candidate{key,l2_distance(query,value)};
    if(heap.size()<k) heap.push(candidate);
    else if(better(candidate,heap.top())) {heap.pop(); heap.push(candidate);}
  });
  std::vector<Neighbor> result; result.reserve(heap.size());
  while(!heap.empty()) {result.push_back(heap.top()); heap.pop();}
  std::reverse(result.begin(),result.end());
  return result;
}
}
