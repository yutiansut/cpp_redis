#include <utility>

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
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef CPP_REDIS_CORE_CLUSTER_HPP_
#define CPP_REDIS_CORE_CLUSTER_HPP_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <cstdio>
#include <cstring>

#include <cpp_redis/core/client.hpp>
#include <cpp_redis/helpers/string_util.hpp>
#include <cpp_redis/misc/crc16.hpp>

#define __METER "m"

#define __CPP_REDIS_DEFAULT_HOST "127.0.0.1"
#define __CPP_REDIS_DEFAULT_PORT 6379

namespace cpp_redis {

namespace cluster {

using slot_t = pair<int, int>;

enum class link_state_type { CONNECTED = 1, DISCONNECTED = 2 };

using link_state_t = link_state_type;

using slot_vec_t = std::vector<slot_t>;

enum class node_type { master = 1, slave = 2 };
using node_type_t = node_type;

class node {
private:
  string_t m_ip;
  client_ptr_t m_client;
  int m_port{};
  node_type_t m_type;
  slot_t m_slot;
  // optional_t<string_t> master;
  int ping_sent{};
  int ping_recv{};

  //! AKA version. Pick the highest version
  //! when multiple nodes claim the same hash slot
  int config_epoch{};
  link_state_t link_state;
  slot_vec_t slots;

public:
  node() = default;
  node(string_t ip, int port)
      : m_ip(std::move(ip)), m_client(new client_t()), m_port(port) {}

  void set_address(string_t &address);

  void set_type(string_t &type_str);

  void set_range(string_t &range_str);

  int get_port() { return m_port; }

  string_t get_ip() { return m_ip; }

  slot_t get_range() {
    return m_slot;
  }
};

using node_t = node;

using node_ptr_t = std::shared_ptr<node_t>;

using node_pair_t = std::pair<string_t, node_ptr_t>;

using node_map_t = std::map<string_t, node_ptr_t>;

class node_slots {
public:
  node_map_t m_nodes;
};

using node_vec_t = std::vector<node>;

struct cluster_slot {
  slot_t range;
};

inline void operator<<(string_t &is, node_pair_t &t) {
  vector<string_t> v = split_str(is, ' ');

  int i = 0;
  for(auto val:v) {
    switch (i) {
    case 0:
      t.first = val;
      break;
    case 1:
      t.second->set_address(val);
      break;
    case 2: // flags (master/slave)
      t.second->set_type(val);
      break;
    case 3: // master
      std::cout << val << std::endl;
      break;
    case 8: // range
      t.second->set_range(val);
      break;
    default:
      std::cout << val << std::endl;
      break;
    }
    i++;
  }
}

inline void operator<<(string_t &is, node_map_t &data) {
  vector<string_t> v = split_str(is, '\n');

  for(auto val:v) {
    node_pair_t rec = {"", std::make_shared<node_t>()};
    val << rec;
    data.insert(rec);
  }
}

inline void operator<<(const reply_t &repl, node_map_t &data) {
  if (!repl.is_error() && repl.is_bulk_string()) {
    string_t r = repl.as_string();
    r << data;
  }
}

class cluster_client {
private:
  node_map_t m_nodes;
  std::map<string_t, slot_t> m_ranges;
  std::map<string_t, std::shared_ptr<client_t>> m_clients;
  std::vector<string_t> m_slots;
  std::pair<string_t, int> m_address;

private:

  void connect(const string_t &key) {
    auto hash_key = crc16(key);
    for (const auto& range : m_ranges) {
      if (range.second.first <= hash_key <= range.second.second) {
        auto * node = m_nodes[range.first].get();
        int_t port = node->get_port();
        string_t host = node->get_ip();
        m_clients[range.first]->connect(host, port);
      }
    }
  }

  std::shared_ptr<client_t> &get_client(const string_t &key) {
    auto hash_key = crc16(key);
    for (const auto& range : m_ranges) {
      if (range.second.first <= hash_key <= range.second.second) {
        auto * node = m_nodes[range.first].get();
        int_t port = node->get_port();
        string_t host = node->get_ip();
        if (!m_clients[range.first]->is_connected()) {
          m_clients[range.first]->connect(host, port);
        }
        return m_clients[range.first];
      }
    }
  }

public:
  cluster_client(const string_t ip, int port) : m_address({ip, port}), m_nodes() {}
  bool m_is_first_connect{};
  void connect();

  cluster_client &getset(const string_t &key, const string_t &val,
         const reply_callback_t &reply_callback);

  void sync_commit() { for (auto client : m_clients) {
    client.second->sync_commit();
  } }


};
} // namespace cluster
} // namespace cpp_redis

#endif // CPP_REDIS_CORE_CLUSTER_HPP_