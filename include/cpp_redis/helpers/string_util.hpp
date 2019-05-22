//
// Created by Nick on 4/24/19.
//

#ifndef CPP_REDIS_STRING_UTIL_HPP
#define CPP_REDIS_STRING_UTIL_HPP

#include <cpp_redis/types/common_types.hpp>

namespace cpp_redis {

inline vector<string_t> split_str(string_t &str, const char sep) {
  string_t buff;
  vector<string_t> v;

  for(auto n:str)
  {
    if(n != sep) buff+=n; else
    if(!buff.empty()) { v.push_back(buff); buff = ""; }
  }
  if(!buff.empty()) v.push_back(buff);
  return v;
}

} // namespace cpp_redis

#endif // CPP_REDIS_STRING_UTIL_HPP
