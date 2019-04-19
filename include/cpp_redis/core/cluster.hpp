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

#define __METER "m"

#define __CPP_REDIS_DEFAULT_HOST "127.0.0.1"
#define __CPP_REDIS_DEFAULT_PORT 6379

namespace cpp_redis {

namespace cluster {

using slot_t = std::tuple<int, int>;

enum class link_state_type { CONNECTED = 1, DISCONNECTED = 2 };

using link_state_t = link_state_type;

using slot_vec_t = std::vector<slot_t>;

class node {
private:
  string_t m_ip;
  client_ptr_t m_client;
  int m_port;
  // optional_t<string_t> master;
  int ping_sent;
  int ping_recv;

  //! AKA version. Pick the highest version
  //! when multiple nodes claim the same hash slot
  int config_epoch;
  link_state_t link_state;
  slot_vec_t slots;

public:
  node(string_t ip, int port)
      : m_ip(std::move(ip)), m_client(new client_t()), m_port(port) {}

   void set_address(string_t &address) {
    int sep = address.find_first_of(':');
    m_ip = address.substr(0, sep);
    std::cout << "ip:  " << m_ip << std::endl;
    string_t v = address.substr(sep, address.length());
    std::cout << "lksdjf " << v << std::endl;
    m_port = 8000;
    // m_port = std::stoi(address.substr(sep,address.length())); }
  }
   int get_port() { return m_port; }
};

using node_t = node;

using node_pair_t = std::pair<string_t, node_t>;

using node_ptr_t = std::shared_ptr<node_t>;

using node_map_t = std::map<string_t, node_ptr_t>;

class node_slots {
public:
  node_map_t m_nodes;
};

using node_vec_t = std::vector<node>;

struct cluster_slot {
  slot_t range;
};

std::istream &operator>>(std::istream &is, node_pair_t &t) {
  // Read string to space
  getline(is >> std::ws, t.first, ' ');

  int i = 1;
  while ((is.peek() != '\n') && (is >> std::ws)) {
    string_t temp;
    getline(is >> std::ws, temp, ' ');
    switch (i) {
    case 1:
      t.second.set_address(temp);
      break;
    case 2: // flags
      std::cout << temp << std::endl;
      break;
    case 3: // master
      std::cout << temp << std::endl;
      break;
    default:
      std::cout << temp << std::endl;
      break;
    }
    i++;
  }

  // Read formatted input; ignore everything else to end of line
  // is >> t.avg;
  // is.ignore( std::numeric_limits<std::streamsize>::max(), '\n' );

  return is;
}

std::istream &operator>>(std::istream &is, node_map_t &data) {
  data.clear();
  node_pair_t rec = {"", node_t("", 0)};
  while (is >> rec)
    data.insert({rec.first, std::shared_ptr<node_t>(&rec.second)});
  return is;
}

class cluster_client {
private:
  node_map_t m_nodes;
  std::vector<string_t> m_slots;
  std::pair<string_t, int> m_address;

public:
  cluster_client(const string_t ip, int port) : m_address({ip, port}) {}

  void connect() {
    client_t rclient;

    rclient.connect(m_address.first, m_address.second);

    rclient.cluster_nodes([&](const reply_t &repl) {
      if (!repl.is_error() && repl.is_bulk_string()) {
        node_map_t nm;
        std::istringstream(repl.as_string()) >> nm;
      }
    });
    // m_nodes.emplace()
  }
};
} // namespace cluster
} // namespace cpp_redis

#endif // CPP_REDIS_CORE_CLUSTER_HPP_