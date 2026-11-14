#pragma once
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minivectordb::sql {
// SQL text normalized to a literal-free shape key. `literals` records the
// quoted-string/number literal values in SQL order; the plan cache replaces
// them without re-lexing or re-parsing the surrounding statement.
struct NormalizedSql {
  std::string key;
  std::vector<std::pair<bool,std::string>> literals; // true = quoted string
};
inline bool normalize_sql(std::string_view sql,NormalizedSql& out) {
  out.key.clear();
  out.literals.clear();
  std::size_t i=0;
  while(i<sql.size()) {
    const char c=sql[i];
    if(std::isspace(static_cast<unsigned char>(c))) {++i;continue;}
    if(c=='-'&&i+1<sql.size()&&sql[i+1]=='-') {
      i+=2;
      while(i<sql.size()&&sql[i]!='\n')++i;
      continue;
    }
    if(c=='/'&&i+1<sql.size()&&sql[i+1]=='*') {
      i+=2;
      while(i+1<sql.size()&&!(sql[i]=='*'&&sql[i+1]=='/'))++i;
      if(i+1>=sql.size())return false;
      i+=2;
      continue;
    }
    if(c=='\'') {
      ++i;
      std::string value;
      bool closed=false;
      while(i<sql.size()) {
        if(sql[i]=='\'') {
          if(i+1<sql.size()&&sql[i+1]=='\'') {value.push_back('\'');i+=2;continue;}
          ++i;
          closed=true;
          break;
        }
        value.push_back(sql[i++]);
      }
      if(!closed)return false;
      out.key.push_back('?');
      out.literals.emplace_back(true,std::move(value));
      continue;
    }
    if(c=='`') {
      ++i;
      std::string value;
      bool closed=false;
      while(i<sql.size()) {
        if(sql[i]=='`') {++i;closed=true;break;}
        value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(sql[i++]))));
      }
      if(!closed)return false;
      out.key.push_back('`');
      out.key.append(value);
      out.key.push_back('`');
      continue;
    }
    if(std::isalpha(static_cast<unsigned char>(c))||c=='_') {
      while(i<sql.size()&&(std::isalnum(static_cast<unsigned char>(sql[i]))||sql[i]=='_'))
        out.key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(sql[i++]))));
      out.key.push_back(' ');
      continue;
    }
    if(std::isdigit(static_cast<unsigned char>(c))||c=='.'||c=='+'||c=='-') {
      const auto start=i;
      if(c=='+'||c=='-')++i;
      bool any=false;
      while(i<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[i]))) {++i;any=true;}
      if(i<sql.size()&&sql[i]=='.') {
        ++i;
        while(i<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[i]))) {++i;any=true;}
      }
      if(any&&i<sql.size()&&(sql[i]=='e'||sql[i]=='E')) {
        const auto save=i;
        ++i;
        if(i<sql.size()&&(sql[i]=='+'||sql[i]=='-'))++i;
        const auto digits=i;
        while(i<sql.size()&&std::isdigit(static_cast<unsigned char>(sql[i])))++i;
        if(i==digits)i=save;
      }
      if(!any)return false;
      out.key.push_back('?');
      out.literals.emplace_back(false,std::string(sql.substr(start,i-start)));
      continue;
    }
    out.key.push_back(c);
    ++i;
  }
  return true;
}
}
