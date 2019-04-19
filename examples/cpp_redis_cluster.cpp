// The MIT License (MIT)
//
// Copyright (c) 2015-2017 Simon Ninon <simon.ninon@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include <cpp_redis/cpp_redis>
#include <cpp_redis/misc/macro.hpp>
#include <string>
#include <cpp_redis/core/cluster.hpp>
#include <cpp_redis/builders/bulk_string_builder.hpp>

#define ENABLE_SESSION = 1

#ifdef _WIN32
#include <Winsock2.h>
#endif //! _WIN32

int main(void) {
  std::string buffer = "235\r\n07c37dfeb235213a872192d90877d0cd55635b91 127.0.0.1:30004 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238317239 4 connected";
  
  cpp_redis::builders::bulk_string_builder builder;
  builder << buffer;

  buffer += "\n67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 127.0.0.1:30002 master - 0 1426238316232 2 connected 5461-10922\r";

  builder << buffer;

   buffer += "\n";
  builder << buffer;

  builder.reply_ready();

  auto reply = builder.get_reply();
  if(reply.is_bulk_string()) {

    std::cout << "HERE" << reply.as_string() << std::endl;
  
  cpp_redis::cluster::node_map_t nm = {};
  std::string r = reply.as_string();
  std::istringstream(r) >> nm;
  
  int port = nm["07c37dfeb235213a872192d90877d0cd55635b91"]->get_port();
  std::cout << "PORT: " << port << std::endl;
  } else {
    std::cout << "ERRORORRJOIJE" << std::endl;
  }
  return 0;
}
