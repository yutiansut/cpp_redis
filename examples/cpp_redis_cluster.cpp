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
#include <cpp_redis/builders/bulk_string_builder.hpp>
#include <cpp_redis/core/cluster.hpp>
#include <cpp_redis/cpp_redis>
#include <cpp_redis/misc/macro.hpp>
#include <string>

#define ENABLE_SESSION = 1

#include "winsock_initializer.h"

int main() {
  winsock_initializer winsock_init;

  cpp_redis::cluster::cluster_client client("localhost", 30001);
  client.connect();

  client.getset("color", "red", [](const cpp_redis::reply_t repl){
    std::cout << repl << std::endl;
  });

  client.getset("fruit", "orange", [](const cpp_redis::reply_t repl){
    std::cout << repl << std::endl;
  });
}
