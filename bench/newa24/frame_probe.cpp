#include "protocol/jsonl.hpp"
int main(int argc,char**argv){
 if(argc!=2)return 2;
 std::ios::sync_with_stdio(false);std::cin.tie(nullptr);
 minivectordb::jsonl::Response response(argv[1]);
 for(std::size_t n:{0u,32u,512u,1024u,3200u,65500u,65536u,65537u,131000u}){
  response.reset();response.append(",\"ok\":true,\"rows\":[[\"");response.append(std::string(n,'x'));response.append("\"]]}");response.prepare();response.emit("1");
 }
}
