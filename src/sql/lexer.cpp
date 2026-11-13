#include "sql.hpp"
namespace minivectordb::sql {
  namespace {
    bool digit(char c) {
      return c>='0'&&c<='9';
    }
    bool alpha(char c) {
      return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';
    }
    bool space(char c) {
      return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f';
    }
    char lower(char c) {
      return c>='A'&&c<='Z'?static_cast<char>(c+('a'-'A')):c;
    }
  }
  Token Lexer::next() {
    const auto s=input;auto& i=position;
    while(i<s.size()) {
      if(space(s[i])) {
        ++i;
        continue;
      }
      if(s[i]=='-'&&i+1<s.size()&&s[i+1]=='-') {
        i+=2;
        while(i<s.size()&&s[i]!='\n')++i;
        continue;
      }
      if(s[i]=='/'&&i+1<s.size()&&s[i+1]=='*') {
        i+=2;
        while(i+1<s.size()&&!(s[i]=='*'&&s[i+1]=='/'))++i;
        if(i+1==s.size()||i==s.size())throw Error(ErrorCode::Syntax,"unterminated comment");
        i+=2;
        continue;
      }
      if(s[i]=='\''||s[i]=='`') {
        char quote=s[i++];
        if(borrow_literals && quote=='\'') {
          const auto end=s.find(quote,i);
          if(end!=std::string_view::npos && (end+1==s.size() || s[end+1]!=quote)) {
            const auto view=s.substr(i,end-i);i=end+1;
            return {TokenKind::String,{},view};
          }
        }
        std::string value;
        bool closed=false;
        while(i<s.size()) {
          const auto end=s.find(quote,i);
          if(end==std::string_view::npos)break;
          value.append(s.substr(i,end-i));
          i=end+1;
          if(i<s.size()&&s[i]==quote) {
            value.push_back(quote);
            ++i;
          }
          else {closed=true;break;}
        }
        if(!closed)throw Error(ErrorCode::Syntax,"unterminated quoted token");
        if(quote=='`')for(auto& c:value)c=lower(c);
        return {
          quote=='\''?TokenKind::String:TokenKind::Identifier,std::move(value)
        };
      }
      if(alpha(s[i])) {
        std::string word;
        do {
          word.push_back(lower(s[i++]));
        }
        while(i<s.size()&&(alpha(s[i])||digit(s[i])));
        return {
          TokenKind::Word,std::move(word)
        };
      }
      if(digit(s[i])||s[i]=='.'||s[i]=='+'||s[i]=='-') {
        auto start=i;
        if(s[i]=='+'||s[i]=='-')++i;
        bool any=false;
        while(i<s.size()&&digit(s[i])) {
          ++i;
          any=true;
        }
        if(i<s.size()&&s[i]=='.') {
          ++i;
          while(i<s.size()&&digit(s[i])) {
            ++i;
            any=true;
          }
        }
        if(!any)throw Error(ErrorCode::Syntax,"invalid numeric literal");
        if(i<s.size()&&(s[i]=='e'||s[i]=='E')) {
          ++i;
          if(i<s.size()&&(s[i]=='+'||s[i]=='-'))++i;
          auto digits=i;
          while(i<s.size()&&digit(s[i]))++i;
          if(i==digits)throw Error(ErrorCode::Syntax,"invalid exponent");
        }
        return {
          TokenKind::Number,std::string(s.substr(start,i-start))
        };
      }
      if(std::string_view("(),[]=<>*;").find(s[i])!=std::string_view::npos) {
        std::string symbol(1,s[i++]);
        if((symbol=="<"||symbol==">")&&i<s.size()&&s[i]=='=')symbol.push_back(s[i++]);
        return {
          TokenKind::Symbol,std::move(symbol)
        };
      }
      throw Error(ErrorCode::Syntax,"unexpected SQL character");
    }
    return {TokenKind::End,{}};
  }
  std::vector<Token> lex(std::string_view s) {
    Lexer lexer(s);std::vector<Token> out;
    for(;;){auto token=lexer.next();const bool done=token.kind==TokenKind::End;out.push_back(std::move(token));if(done)return out;}
  }
}
