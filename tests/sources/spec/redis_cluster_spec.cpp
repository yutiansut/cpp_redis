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

#include <thread>

#include <cpp_redis/core/client.hpp>
#include <cpp_redis/core/cluster.hpp>
#include <cpp_redis/core/subscriber.hpp>
#include <cpp_redis/misc/error.hpp>

#include <cpp_redis/builders/bulk_string_builder.hpp>

#include <gtest/gtest.h>

TEST(NodeMap, ValidateNodeParse) {
  cpp_redis::cluster::node_map_t nm;
  std::string upl(
      "07c37dfeb235213a872192d90877d0cd55635b91\r127.0.0.1:"
      "30004\rslave\re7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca\r0\r1426238317239"
      "\r4\rconnected\n67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1\r127.0.0.1:"
      "30002\rmaster\r-\r0\r1426238316232\r2\rconnected\r5461-"
      "10922\n292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f\r127.0.0.1:"
      "30003\rmaster\r-\r0\r1426238318243\r3\rconnected\r10923-"
      "16383\n6ec23923021cf3ffec47632106199cb7f496ce01\r127.0.0.1:"
      "30005\rslave\r67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1\r0\r1426238316232"
      "\r5\rconnected\n824fe116063bc5fcf9f4ffd895bc17aee7731ac3\r127.0.0.1:"
      "30006\rslave\r292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f\r0\r1426238317741"
      "\r6\rconnected\ne7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca\r127.0.0.1:"
      "30001\rmyself,master\r-\r0\r0\r1\rconnected\r0-5460");

  cpp_redis::builders::bulk_string_builder builder;

  std::string buffer = "5\r\nhello\r\n";
  builder << buffer;

  auto reply = builder.get_reply();

  EXPECT_TRUE(reply.is_bulk_string());

  auto s = reply.as_string();

  s << nm;

  int port = nm["07c37dfeb235213a872192d90877d0cd55635b91"]->get_port();
  std::cout << port << std::endl;

  EXPECT_EQ(port, 30004);

  // cpp_redis::cluster_client client("127.0.0.1", 30001);
}

TEST(RedisClient, ValidateClusterNodesReturnsBulkString) {
  cpp_redis::client client;

  EXPECT_FALSE(client.is_connected());
  //! should connect to 127.0.0.1:6379
  EXPECT_NO_THROW(client.connect("127.0.0.1", 30001));
  EXPECT_TRUE(client.is_connected());

  client.cluster_nodes([](cpp_redis::reply_t &reply) {
    EXPECT_FALSE(reply.is_error());
    EXPECT_TRUE(reply.is_bulk_string());
  });

  // client.cluster_addslots({"1", "2", "3"}, [&](const std::string &,
  // [](cpp_redis::reply_t &reply) {
  //   EXPECT_FALSE(reply.is_error());
  //   EXPECT_EQ(reply.error(), "OK");
  // });

  EXPECT_NO_THROW(client.sync_commit());
}

// TEST(RedisClient, ValidConnectionDefinedHost) {
//   cpp_redis::client client;

//   EXPECT_FALSE(client.is_connected());
//   //! should connect to 127.0.0.1:6379
//   EXPECT_NO_THROW(client.connect("127.0.0.1", 6379));
//   EXPECT_NO_THROW(client.sync_commit(std::chrono::milliseconds(100)));
//   EXPECT_TRUE(client.is_connected());
// }