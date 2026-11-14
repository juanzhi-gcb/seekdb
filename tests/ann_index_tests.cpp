#include "sql/ann_index.hpp"
#include "minivectordb/exact.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <span>
#include <vector>

int main() {
  constexpr std::size_t count=4096,dimension=32,k=10,queries=100;
  std::mt19937 rng(20261114U);
  std::uniform_real_distribution<float> uniform(-1.0f,1.0f);
  std::vector<float> vectors(count*dimension);
  for(auto& value:vectors)value=uniform(rng);
  std::vector<std::int64_t> keys(count);
  std::iota(keys.begin(),keys.end(),std::int64_t(0));
  minivectordb::sql::HnswIndex index;
  if(!index.build(vectors.data(),count,dimension,32,200))return 1;
  const auto better=[](const minivectordb::Neighbor&a,const minivectordb::Neighbor&b){
    return a.distance<b.distance||(a.distance==b.distance&&a.key<b.key);
  };
  double recall=0;
  for(std::size_t q=0;q<queries;++q) {
    std::vector<float> query(dimension);
    for(auto& value:query)value=uniform(rng);
    std::vector<minivectordb::Neighbor> exact;
    exact.reserve(count);
    for(std::size_t row=0;row<count;++row)
      exact.push_back({keys[row],minivectordb::l2_distance(query,std::span<const float>(vectors.data()+row*dimension,dimension))});
    std::sort(exact.begin(),exact.end(),better);exact.resize(k);
    auto ann=index.search(keys.data(),vectors.data(),query,k,200);
    std::size_t hits=0;
    for(const auto& item:ann)for(const auto& truth:exact)if(item.key==truth.key){++hits;break;}
    recall+=double(hits)/double(k);
  }
  recall/=double(queries);
  std::cout<<"ann recall@10 "<<recall<<'\n';
  return recall>=0.95?0:1;
}
