#include <cpp_redis/core/cluster.hpp>
#include <cpp_redis/misc/crc16.hpp>

namespace cpp_redis {

void cpp_redis::cluster::cluster_client::connect() {

  client_t client;

  m_is_first_connect = true;

  client.connect(m_address.first, m_address.second);

  client.cluster_nodes([this](const reply_t &repl) {
    std::cout << "here" << std::endl;
    if (!repl.is_error() && repl.is_bulk_string()) {
      repl << m_nodes;
    }
  });

  client.sync_commit();

  for (const auto& node : m_nodes) {
    auto *nptr = node.second.get();
    if (nptr->get_port() == m_address.second) {
      m_clients[node.first] = std::shared_ptr<client_t>(&client);
    } else {
      m_clients[node.first] = std::make_shared<client_t>();
      m_clients[node.first]->connect(nptr->get_ip(), nptr->get_port());
    }
    m_ranges[node.first] = nptr->get_range();
  }
  // m_nodes.emplace()
}
cluster::cluster_client &
cluster::cluster_client::getset(const string_t &key, const string_t &val,
                                const reply_callback_t &reply_callback) {
  auto c = get_client(key);
  c->getset(key, val, reply_callback);
  c->sync_commit();
  return *this;
}

void cluster::node::set_address(string_t &address) {
  int sep = address.find_first_of(':');
  m_ip = address.substr(0, sep);
  string_t v = address.substr(sep, address.length());
  m_port = std::stoi(address.substr(sep+1,address.length()));
}
void cluster::node::set_type(string_t &type_str) {
  if (type_str == "slave") {
    m_type = node_type::slave;
  } else {
    m_type = node_type::master;
  }
}
void cluster::node::set_range(string_t &range_str) {
  if (m_type == node_type::master) {
    int sep = range_str.find_first_of('-');
    auto f = range_str.substr(0, sep);
    m_slot.first = std::stoi(f);
    auto s = range_str.substr(sep+1, range_str.length());
    m_slot.second = std::stoi(s);
  }
}
} // namespace cpp_redis