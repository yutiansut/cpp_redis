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

#include <cpp_redis/core/client.hpp>
#include <cpp_redis/misc/error.hpp>
#include <cpp_redis/misc/macro.hpp>

#include <cpp_redis/impl/reply.ipp>

namespace cpp_redis {

#ifndef __CPP_REDIS_USE_CUSTOM_TCP_CLIENT

client::client()
    : m_reconnecting(false), m_cancel(false), m_callbacks_running(0) {
  __CPP_REDIS_LOG(debug, "cpp_redis::client created");
}

#endif // __CPP_REDIS_USE_CUSTOM_TCP_CLIENT

client::client(const std::shared_ptr<network::tcp_client_iface> &tcp_client)
    : m_client(tcp_client), m_sentinel(tcp_client), m_reconnecting(false),
      m_cancel(false), m_callbacks_running(0) {
  __CPP_REDIS_LOG(debug, "cpp_redis::client created");
}

client::~client() {
  //!
  //!  ensure we stopped reconnection attempts
  //!
  if (!m_cancel) {
    cancel_reconnect();
  }

  //!
  //!  If for some reason sentinel is connected then disconnect now.
  //!
  if (m_sentinel.is_connected()) {
    m_sentinel.disconnect(true);
  }

  //!
  //!  disconnect underlying tcp socket
  //!
  if (m_client.is_connected()) {
    m_client.disconnect(true);
  }

  __CPP_REDIS_LOG(debug, "cpp_redis::client destroyed");
}

void client::connect(const string_t &name,
                     const connect_callback_t &connect_callback,
                     uint_t timeout_ms, int_t max_reconnects,
                     uint_t reconnect_interval_ms) {
  //!
  //!  Save for auto reconnects
  //!
  m_master_name = name;

  //!
  //!  We rely on the sentinel to tell us which redis server is currently the
  //!  master.
  //!
  if (m_sentinel.get_master_addr_by_name(name, m_redis_server, m_redis_port,
                                         true)) {
    connect(m_redis_server, m_redis_port, connect_callback, timeout_ms,
            max_reconnects, reconnect_interval_ms);
  } else {
    throw redis_error(
        "cpp_redis::client::connect() could not find master for m_name " +
        name);
  }
}

void client::connect(const string_t &host, std::size_t port,
                     const connect_callback_t &connect_callback,
                     uint_t timeout_ms, int_t max_reconnects,
                     uint_t reconnect_interval_ms) {
  __CPP_REDIS_LOG(debug, "cpp_redis::client attempts to connect");

  //!
  //!  Save for auto reconnects
  //!
  m_redis_server = host;
  m_redis_port = port;
  m_connect_callback = connect_callback;
  m_max_reconnects = max_reconnects;
  m_reconnect_interval_ms = reconnect_interval_ms;

  //!
  //!  notify start
  //!
  if (m_connect_callback) {
    m_connect_callback(host, port, connect_state::start);
  }

  auto disconnection_handler = std::bind(
      &client::connection_disconnection_handler, this, std::placeholders::_1);
  auto receive_handler =
      std::bind(&client::connection_receive_handler, this,
                std::placeholders::_1, std::placeholders::_2);
  m_client.connect(host, port, disconnection_handler, receive_handler,
                   timeout_ms);

  __CPP_REDIS_LOG(info, "cpp_redis::client connected");

  //!
  //!  notify end
  //!
  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::ok);
  }
}

void client::disconnect(bool wait_for_removal) {
  __CPP_REDIS_LOG(debug, "cpp_redis::client attempts to disconnect");

  //!
  //!  close connection
  //!
  m_client.disconnect(wait_for_removal);

  //!
  //!  make sure we clear buffer of unsent commands
  //!
  clear_callbacks();

  __CPP_REDIS_LOG(info, "cpp_redis::client disconnected");
}

bool client::is_connected() const { return m_client.is_connected(); }

void client::cancel_reconnect() { m_cancel = true; }

bool client::is_reconnecting() const { return m_reconnecting; }

void client::add_sentinel(const string_t &host, std::size_t port,
                          uint_t timeout_ms) {
  m_sentinel.add_sentinel(host, port, timeout_ms);
}

const sentinel &client::get_sentinel() const { return m_sentinel; }

sentinel &client::get_sentinel() { return m_sentinel; }

void client::clear_sentinels() { m_sentinel.clear_sentinels(); }

client &client::send(const std::vector<string_t> &redis_cmd,
                     const reply_callback_t &callback) {
  std::lock_guard<std::mutex> lock_callback(m_callbacks_mutex);

  __CPP_REDIS_LOG(
      info,
      "cpp_redis::client attempts to store new command in the send buffer");
  unprotected_send(redis_cmd, callback);
  __CPP_REDIS_LOG(info,
                  "cpp_redis::client stored new command in the send buffer");

  return *this;
}

void client::unprotected_send(const std::vector<string_t> &redis_cmd,
                              const reply_callback_t &callback) {
  m_client.send(redis_cmd);
  m_commands.push({redis_cmd, callback});
}

//!
//!  commit pipelined transaction
//!
client &client::commit() {
  //!
  //!  no need to call commit in case of reconnection
  //!  the reconnection flow will do it for us
  //!
  if (!is_reconnecting()) {
    try_commit();
  }

  return *this;
}

client &client::sync_commit() {
  //!
  //!  no need to call commit in case of reconnection
  //!  the reconnection flow will do it for us
  //!
  if (!is_reconnecting()) {
    try_commit();
  }

  std::unique_lock<std::mutex> lock_callback(m_callbacks_mutex);
  __CPP_REDIS_LOG(debug, "cpp_redis::client waiting for callbacks to complete");
  m_sync_cond_var.wait(lock_callback, [=] {
    return m_callbacks_running == 0 && m_commands.empty();
  });
  __CPP_REDIS_LOG(debug,
                  "cpp_redis::client finished waiting for callback completion");
  return *this;
}

void client::try_commit() {
  try {
    __CPP_REDIS_LOG(debug,
                    "cpp_redis::client attempts to send pipelined commands");
    m_client.commit();
    __CPP_REDIS_LOG(info, "cpp_redis::client sent pipelined commands");
  } catch (const cpp_redis::redis_error &) {
    __CPP_REDIS_LOG(error,
                    "cpp_redis::client could not send pipelined commands");
    //!
    //!  ensure commands are flushed
    //!
    clear_callbacks();
    throw;
  }
}

void client::connection_receive_handler(network::redis_connection &,
                                        reply &reply) {
  reply_callback_t callback = nullptr;

  __CPP_REDIS_LOG(info, "cpp_redis::client received reply");
  {
    std::lock_guard<std::mutex> lock(m_callbacks_mutex);
    m_callbacks_running += 1;

    if (!m_commands.empty()) {
      callback = m_commands.front().callback;
      m_commands.pop();
    }
  }

  if (callback) {
    __CPP_REDIS_LOG(debug, "cpp_redis::client executes reply callback");
    callback(reply);
  }

  {
    std::lock_guard<std::mutex> lock(m_callbacks_mutex);
    m_callbacks_running -= 1;
    m_sync_cond_var.notify_all();
  }
}

void client::clear_callbacks() {
  if (m_commands.empty()) {
    return;
  }

  //!
  //!  de-queue commands and move them to a local variable
  //!
  std::queue<command_request> commands = std::move(m_commands);

  m_callbacks_running += __CPP_REDIS_LENGTH(commands.size());

  std::thread t([=]() mutable {
    while (!commands.empty()) {
      const auto &callback = commands.front().callback;

      if (callback) {
        reply r = {"network failure", reply::string_type::error};
        callback(r);
      }

      --m_callbacks_running;
      commands.pop();
    }

    m_sync_cond_var.notify_all();
  });
  t.detach();
}

void client::resend_failed_commands() {
  if (m_commands.empty()) {
    return;
  }

  //!
  //!  de-queue commands and move them to a local variable
  //!
  std::queue<command_request> commands = std::move(m_commands);

  while (!commands.empty()) {
    //!
    //!  Reissue the pending command and its callback.
    //!
    unprotected_send(commands.front().command, commands.front().callback);

    commands.pop();
  }
}

void client::connection_disconnection_handler(network::redis_connection &) {
  //!
  //!  leave right now if we are already dealing with reconnection
  //!
  if (is_reconnecting()) {
    return;
  }

  //!
  //!  initiate reconnection process
  //!
  m_reconnecting = true;
  m_current_reconnect_attempts = 0;

  __CPP_REDIS_LOG(warn, "cpp_redis::client has been disconnected");

  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::dropped);
  }

  //!
  //!  Lock the callbacks mutex of the base class to prevent more client
  //!  commands from being issued until our reconnect has completed.
  //!
  std::lock_guard<std::mutex> lock_callback(m_callbacks_mutex);

  while (should_reconnect()) {
    sleep_before_next_reconnect_attempt();
    reconnect();
  }

  if (!is_connected()) {
    clear_callbacks();

    //!
    //!  Tell the user we gave up!
    //!
    if (m_connect_callback) {
      m_connect_callback(m_redis_server, m_redis_port, connect_state::stopped);
    }
  }

  //!
  //!  terminate reconnection
  //!
  m_reconnecting = false;
}

void client::sleep_before_next_reconnect_attempt() {
  if (m_reconnect_interval_ms <= 0) {
    return;
  }

  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::sleeping);
  }

  std::this_thread::sleep_for(
      std::chrono::milliseconds(m_reconnect_interval_ms));
}

bool client::should_reconnect() const {
  return !is_connected() && !m_cancel &&
         (m_max_reconnects == -1 ||
          m_current_reconnect_attempts < m_max_reconnects);
}

void client::re_auth() {
  if (m_password.empty()) {
    return;
  }

  // In re_auth we do not want to serialize the command to the TCP buffer (i.e.
  // call m_client.send) as it will get serialized to the buffer in
  // resend_failed_commands after re_auth is called in reconnect.  So we just
  // push the command into the command queue.
  //
  // Otherwise we will end up with the AUTH command serialized to
  // the buffer twice and in the command queue once, which causes a problem when
  // we get 2 responses back.  The subsequent responses are off by one in the
  // command callbacks.
  //
  m_commands.push(
      {{"AUTH", m_password}, [&](cpp_redis::reply_t &reply) {
         if (reply.is_string() && reply.as_string() == "OK") {
           __CPP_REDIS_LOG(warn, "client successfully re-authenticated");
         } else {
           __CPP_REDIS_LOG(warn, string_t("client failed to re-authenticate: " +
                                          reply.as_string())
                                     .c_str());
         }
       }});
}

void client::re_select() {
  if (m_database_index <= 0) {
    return;
  }

  unprotected_select(m_database_index, [&](cpp_redis::reply_t &reply) {
    if (reply.is_string() && reply.as_string() == "OK") {
      __CPP_REDIS_LOG(warn, "client successfully re-selected redis database");
    } else {
      __CPP_REDIS_LOG(warn, string_t("client failed to re-select database: " +
                                     reply.as_string())
                                .c_str());
    }
  });
}

void client::reconnect() {
  //!
  //!  increase the number of attempts to reconnect
  //!
  ++m_current_reconnect_attempts;

  //!
  //!  We rely on the sentinel to tell us which redis server is currently the
  //!  master.
  //!
  if (!m_master_name.empty() &&
      !m_sentinel.get_master_addr_by_name(m_master_name, m_redis_server,
                                          m_redis_port, true)) {
    if (m_connect_callback) {
      m_connect_callback(m_redis_server, m_redis_port,
                         connect_state::lookup_failed);
    }
    return;
  }

  //!
  //!  Try catch block because the redis client throws an error if connection
  //!  cannot be made.
  //!
  try {
    connect(m_redis_server, m_redis_port, m_connect_callback,
            m_connect_timeout_ms, m_max_reconnects, m_reconnect_interval_ms);
  } catch (...) {
  }

  if (!is_connected()) {
    if (m_connect_callback) {
      m_connect_callback(m_redis_server, m_redis_port, connect_state::failed);
    }
    return;
  }

  //!
  //!  notify end
  //!
  if (m_connect_callback) {
    m_connect_callback(m_redis_server, m_redis_port, connect_state::ok);
  }

  __CPP_REDIS_LOG(info, "client reconnected ok");

  re_auth();
  re_select();
  resend_failed_commands();
  try_commit();
}

string_t client::aggregate_method_to_string(aggregate_method method) const {
  switch (method) {
  case aggregate_method::sum:
    return "SUM";
  case aggregate_method::min:
    return "MIN";
  case aggregate_method::max:
    return "MAX";
  default:
    return "";
  }
}

string_t client::geo_unit_to_string(geo_unit unit) const {
  switch (unit) {
  case geo_unit::m:
    return "m";
  case geo_unit::km:
    return "km";
  case geo_unit::ft:
    return "ft";
  case geo_unit::mi:
    return "mi";
  default:
    return "";
  }
}

string_t client::bitfield_operation_type_to_string(
    bitfield_operation_type operation) const {
  switch (operation) {
  case bitfield_operation_type::get:
    return "GET";
  case bitfield_operation_type::set:
    return "SET";
  case bitfield_operation_type::incrby:
    return "INCRBY";
  default:
    return "";
  }
}

string_t client::overflow_type_to_string(overflow_type type) const {
  switch (type) {
  case overflow_type::wrap:
    return "WRAP";
  case overflow_type::sat:
    return "SAT";
  case overflow_type::fail:
    return "FAIL";
  default:
    return "";
  }
}

client::bitfield_operation
client::bitfield_operation::get(const string_t &type, int offset,
                                overflow_type overflow) {
  return {bitfield_operation_type::get, type, offset, 0, overflow};
}

client::bitfield_operation
client::bitfield_operation::set(const string_t &type, int offset, int value,
                                overflow_type overflow) {
  return {bitfield_operation_type::set, type, offset, value, overflow};
}

client::bitfield_operation
client::bitfield_operation::incrby(const string_t &type, int offset,
                                   int increment, overflow_type overflow) {
  return {bitfield_operation_type::incrby, type, offset, increment, overflow};
}

//!
//!  Redis commands
//!  Callback-based
//!

client &client::append(const string_t &key, const string_t &value,
                       const reply_callback_t &reply_callback) {
  send({"APPEND", key, value}, reply_callback);
  return *this;
}

client &client::auth(const string_t &password,
                     const reply_callback_t &reply_callback) {
  std::lock_guard<std::mutex> lock(m_callbacks_mutex);

  unprotected_auth(password, reply_callback);

  return *this;
}

void client::unprotected_auth(const string_t &password,
                              const reply_callback_t &reply_callback) {
  //!
  //!  save the password for reconnect attempts.
  //!
  m_password = password;
  //!
  //!  store command in pipeline
  //!
  unprotected_send({"AUTH", password}, reply_callback);
}

client &client::bgrewriteaof(const reply_callback_t &reply_callback) {
  send({"BGREWRITEAOF"}, reply_callback);
  return *this;
}

client &client::bgsave(const reply_callback_t &reply_callback) {
  send({"BGSAVE"}, reply_callback);
  return *this;
}

client &client::bitcount(const string_t &key,
                         const reply_callback_t &reply_callback) {
  send({"BITCOUNT", key}, reply_callback);
  return *this;
}

client &client::bitcount(const string_t &key, int start, int end,
                         const reply_callback_t &reply_callback) {
  send({"BITCOUNT", key, std::to_string(start), std::to_string(end)},
       reply_callback);
  return *this;
}

client &client::bitfield(const string_t &key,
                         const std::vector<bitfield_operation> &operations,
                         const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"BITFIELD", key};

  for (const auto &operation : operations) {
    cmd.push_back(bitfield_operation_type_to_string(operation.operation_type));
    cmd.push_back(operation.type);
    cmd.push_back(std::to_string(operation.offset));

    if (operation.operation_type == bitfield_operation_type::set ||
        operation.operation_type == bitfield_operation_type::incrby) {
      cmd.push_back(std::to_string(operation.value));
    }

    if (operation.overflow != overflow_type::server_default) {
      cmd.emplace_back("OVERFLOW");
      cmd.push_back(overflow_type_to_string(operation.overflow));
    }
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::bitop(const string_t &operation, const string_t &destkey,
                      const std::vector<string_t> &keys,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"BITOP", operation, destkey};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::bitpos(const string_t &key, int bit,
                       const reply_callback_t &reply_callback) {
  send({"BITPOS", key, std::to_string(bit)}, reply_callback);
  return *this;
}

client &client::bitpos(const string_t &key, int bit, int start,
                       const reply_callback_t &reply_callback) {
  send({"BITPOS", key, std::to_string(bit), std::to_string(start)},
       reply_callback);
  return *this;
}

client &client::bitpos(const string_t &key, int bit, int start, int end,
                       const reply_callback_t &reply_callback) {
  send({"BITPOS", key, std::to_string(bit), std::to_string(start),
        std::to_string(end)},
       reply_callback);
  return *this;
}

client &client::blpop(const std::vector<string_t> &keys, int timeout,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"BLPOP"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.push_back(std::to_string(timeout));
  send(cmd, reply_callback);
  return *this;
}

client &client::brpop(const std::vector<string_t> &keys, int timeout,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"BRPOP"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.push_back(std::to_string(timeout));
  send(cmd, reply_callback);
  return *this;
}

client &client::brpoplpush(const string_t &src, const string_t &dst,
                           int timeout,
                           const reply_callback_t &reply_callback) {
  send({"BRPOPLPUSH", src, dst, std::to_string(timeout)}, reply_callback);
  return *this;
}

client &client::bzpopmin(const std::vector<string_t> &keys, int timeout,
                         const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"BZPOPMIN"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.push_back(std::to_string(timeout));
  send(cmd, reply_callback);
  return *this;
}

client &client::bzpopmax(const std::vector<string_t> &keys, int timeout,
                         const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"BZPOPMAX"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.push_back(std::to_string(timeout));
  send(cmd, reply_callback);
  return *this;
}

client &client::client_id(const reply_callback_t &reply_callback) {
  send({"CLIENT", "ID"}, reply_callback);
  return *this;
}

client &client::client_list(const reply_callback_t &reply_callback) {
  send({"CLIENT", "LIST"}, reply_callback);
  return *this;
}

client &
client::client_list_test(const client_list_reply_callback_t &reply_callback) {
  send({"CLIENT", "GETNAME"}, [&](const reply_t &repl) {
    client_list_payload_t clp(repl);
    reply_callback(clp);
  });
  return *this;
}

client &client::client_getname(const reply_callback_t &reply_callback) {
  send({"CLIENT", "GETNAME"}, reply_callback);
  return *this;
}

client &client::client_pause(int timeout,
                             const reply_callback_t &reply_callback) {
  send({"CLIENT", "PAUSE", std::to_string(timeout)}, reply_callback);
  return *this;
}

client &client::client_reply(const string_t &mode,
                             const reply_callback_t &reply_callback) {
  send({"CLIENT", "REPLY", mode}, reply_callback);
  return *this;
}

client &client::client_setname(const string_t &name,
                               const reply_callback_t &reply_callback) {
  send({"CLIENT", "SETNAME", name}, reply_callback);
  return *this;
}

client &client::client_unblock(int id, const reply_callback_t &reply_callback) {
  send({"CLIENT", "UNBLOCK", std::to_string(id)}, reply_callback);
  return *this;
}

client &client::client_unblock(int id, bool witherror,
                               const reply_callback_t &reply_callback) {
  if (witherror)
    send({"CLIENT", "UNBLOCK", std::to_string(id), "ERROR"}, reply_callback);
  else
    send({"CLIENT", "UNBLOCK", std::to_string(id)}, reply_callback);
  return *this;
}

client &client::cluster_addslots(const std::vector<string_t> &p_slots,
                                 const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"CLUSTER", "ADDSLOTS"};
  cmd.insert(cmd.end(), p_slots.begin(), p_slots.end());
  send(cmd, reply_callback);
  return *this;
}

client &
client::cluster_count_failure_reports(const string_t &node_id,
                                      const reply_callback_t &reply_callback) {
  send({"CLUSTER", "COUNT-FAILURE-REPORTS", node_id}, reply_callback);
  return *this;
}

client &
client::cluster_countkeysinslot(const string_t &slot,
                                const reply_callback_t &reply_callback) {
  send({"CLUSTER", "COUNTKEYSINSLOT", slot}, reply_callback);
  return *this;
}

client &client::cluster_delslots(const std::vector<string_t> &p_slots,
                                 const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"CLUSTER", "DELSLOTS"};
  cmd.insert(cmd.end(), p_slots.begin(), p_slots.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::cluster_failover(const reply_callback_t &reply_callback) {
  send({"CLUSTER", "FAILOVER"}, reply_callback);
  return *this;
}

client &client::cluster_failover(const string_t &mode,
                                 const reply_callback_t &reply_callback) {
  send({"CLUSTER", "FAILOVER", mode}, reply_callback);
  return *this;
}

client &client::cluster_forget(const string_t &node_id,
                               const reply_callback_t &reply_callback) {
  send({"CLUSTER", "FORGET", node_id}, reply_callback);
  return *this;
}

client &client::cluster_getkeysinslot(const string_t &slot, int count,
                                      const reply_callback_t &reply_callback) {
  send({"CLUSTER", "GETKEYSINSLOT", slot, std::to_string(count)},
       reply_callback);
  return *this;
}

client &client::cluster_info(const reply_callback_t &reply_callback) {
  send({"CLUSTER", "INFO"}, reply_callback);
  return *this;
}

client &client::cluster_keyslot(const string_t &key,
                                const reply_callback_t &reply_callback) {
  send({"CLUSTER", "KEYSLOT", key}, reply_callback);
  return *this;
}

client &client::cluster_meet(const string_t &ip, int port,
                             const reply_callback_t &reply_callback) {
  send({"CLUSTER", "MEET", ip, std::to_string(port)}, reply_callback);
  return *this;
}

client &client::cluster_nodes(const reply_callback_t &reply_callback) {
  send({"CLUSTER", "NODES"}, reply_callback);
  return *this;
}

client &client::cluster_replicate(const string_t &node_id,
                                  const reply_callback_t &reply_callback) {
  send({"CLUSTER", "REPLICATE", node_id}, reply_callback);
  return *this;
}

client &client::cluster_reset(const reply_callback_t &reply_callback) {
  send({"CLUSTER", "RESET"}, reply_callback);
  return *this;
}

client &client::cluster_reset(const string_t &mode,
                              const reply_callback_t &reply_callback) {
  send({"CLUSTER", "RESET", mode}, reply_callback);
  return *this;
}

client &client::cluster_saveconfig(const reply_callback_t &reply_callback) {
  send({"CLUSTER", "SAVECONFIG"}, reply_callback);
  return *this;
}

client &
client::cluster_set_config_epoch(const string_t &epoch,
                                 const reply_callback_t &reply_callback) {
  send({"CLUSTER", "SET-CONFIG-EPOCH", epoch}, reply_callback);
  return *this;
}

client &client::cluster_setslot(const string_t &slot, const string_t &mode,
                                const reply_callback_t &reply_callback) {
  send({"CLUSTER", "SETSLOT", slot, mode}, reply_callback);
  return *this;
}

client &client::cluster_setslot(const string_t &slot, const string_t &mode,
                                const string_t &node_id,
                                const reply_callback_t &reply_callback) {
  send({"CLUSTER", "SETSLOT", slot, mode, node_id}, reply_callback);
  return *this;
}

client &client::cluster_slaves(const string_t &node_id,
                               const reply_callback_t &reply_callback) {
  send({"CLUSTER", "SLAVES", node_id}, reply_callback);
  return *this;
}

client &client::cluster_slots(const reply_callback_t &reply_callback) {
  send({"CLUSTER", "SLOTS"}, reply_callback);
  return *this;
}

client &client::command(const reply_callback_t &reply_callback) {
  send({"COMMAND"}, reply_callback);
  return *this;
}

client &client::command_count(const reply_callback_t &reply_callback) {
  send({"COMMAND", "COUNT"}, reply_callback);
  return *this;
}

client &client::command_getkeys(const reply_callback_t &reply_callback) {
  send({"COMMAND", "GETKEYS"}, reply_callback);
  return *this;
}

client &client::command_info(const std::vector<string_t> &command_name,
                             const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"COMMAND", "COUNT"};
  cmd.insert(cmd.end(), command_name.begin(), command_name.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::config_get(const string_t &param,
                           const reply_callback_t &reply_callback) {
  send({"CONFIG", "GET", param}, reply_callback);
  return *this;
}

client &client::config_rewrite(const reply_callback_t &reply_callback) {
  send({"CONFIG", "REWRITE"}, reply_callback);
  return *this;
}

client &client::config_set(const string_t &param, const string_t &val,
                           const reply_callback_t &reply_callback) {
  send({"CONFIG", "SET", param, val}, reply_callback);
  return *this;
}

client &client::config_resetstat(const reply_callback_t &reply_callback) {
  send({"CONFIG", "RESETSTAT"}, reply_callback);
  return *this;
}

client &client::dbsize(const reply_callback_t &reply_callback) {
  send({"DBSIZE"}, reply_callback);
  return *this;
}

client &client::debug_object(const string_t &key,
                             const reply_callback_t &reply_callback) {
  send({"DEBUG", "OBJECT", key}, reply_callback);
  return *this;
}

client &client::debug_segfault(const reply_callback_t &reply_callback) {
  send({"DEBUG", "SEGFAULT"}, reply_callback);
  return *this;
}

client &client::decr(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"DECR", key}, reply_callback);
  return *this;
}

client &client::decrby(const string_t &key, int val,
                       const reply_callback_t &reply_callback) {
  send({"DECRBY", key, std::to_string(val)}, reply_callback);
  return *this;
}

client &client::del(const std::vector<string_t> &key,
                    const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"DEL"};
  cmd.insert(cmd.end(), key.begin(), key.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::discard(const reply_callback_t &reply_callback) {
  send({"DISCARD"}, reply_callback);
  return *this;
}

client &client::dump(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"DUMP", key}, reply_callback);
  return *this;
}

client &client::echo(const string_t &msg,
                     const reply_callback_t &reply_callback) {
  send({"ECHO", msg}, reply_callback);
  return *this;
}

client &client::eval(const string_t &script, int numkeys,
                     const std::vector<string_t> &keys,
                     const std::vector<string_t> &args,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"EVAL", script, std::to_string(numkeys)};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.insert(cmd.end(), args.begin(), args.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::eval(const string_t &script, const std::vector<string_t> &keys,
                     const std::vector<string_t> &args,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"EVAL", script, std::to_string(keys.size())};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.insert(cmd.end(), args.begin(), args.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::evalsha(const string_t &sha1, int numkeys,
                        const std::vector<string_t> &keys,
                        const std::vector<string_t> &args,
                        const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"EVALSHA", sha1, std::to_string(numkeys)};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.insert(cmd.end(), args.begin(), args.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::evalsha(const string_t &sha1, const std::vector<string_t> &keys,
                        const std::vector<string_t> &args,
                        const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"EVALSHA", sha1, std::to_string(keys.size())};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  cmd.insert(cmd.end(), args.begin(), args.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::exec(const reply_callback_t &reply_callback) {
  send({"EXEC"}, reply_callback);
  return *this;
}

client &client::exists(const std::vector<string_t> &keys,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"EXISTS"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::expire(const string_t &key, int seconds,
                       const reply_callback_t &reply_callback) {
  send({"EXPIRE", key, std::to_string(seconds)}, reply_callback);
  return *this;
}

client &client::expireat(const string_t &key, int timestamp,
                         const reply_callback_t &reply_callback) {
  send({"EXPIREAT", key, std::to_string(timestamp)}, reply_callback);
  return *this;
}

client &client::flushall(const reply_callback_t &reply_callback) {
  send({"FLUSHALL"}, reply_callback);
  return *this;
}

client &client::flushdb(const reply_callback_t &reply_callback) {
  send({"FLUSHDB"}, reply_callback);
  return *this;
}

client &client::geoadd(
    const string_t &key,
    const std::vector<std::tuple<string_t, string_t, string_t>> &long_lat_memb,
    const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"GEOADD", key};
  for (const auto &obj : long_lat_memb) {
    cmd.push_back(std::get<0>(obj));
    cmd.push_back(std::get<1>(obj));
    cmd.push_back(std::get<2>(obj));
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::geohash(const string_t &key,
                        const std::vector<string_t> &members,
                        const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"GEOHASH", key};
  cmd.insert(cmd.end(), members.begin(), members.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::geopos(const string_t &key,
                       const std::vector<string_t> &members,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"GEOPOS", key};
  cmd.insert(cmd.end(), members.begin(), members.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::geodist(const string_t &key, const string_t &member_1,
                        const string_t &member_2,
                        const reply_callback_t &reply_callback) {
  send({"GEODIST", key, member_1, member_2}, reply_callback);
  return *this;
}

client &client::geodist(const string_t &key, const string_t &member_1,
                        const string_t &member_2, const string_t &unit,
                        const reply_callback_t &reply_callback) {
  send({"GEODIST", key, member_1, member_2, unit}, reply_callback);
  return *this;
}

client &client::georadius(const string_t &key, double longitude,
                          double latitude, double radius, geo_unit unit,
                          bool with_coord, bool with_dist, bool with_hash,
                          bool asc_order,
                          const reply_callback_t &reply_callback) {
  return georadius(key, longitude, latitude, radius, unit, with_coord,
                   with_dist, with_hash, asc_order, 0, "", "", reply_callback);
}

client &client::georadius(const string_t &key, double longitude,
                          double latitude, double radius, geo_unit unit,
                          bool with_coord, bool with_dist, bool with_hash,
                          bool asc_order, std::size_t count,
                          const reply_callback_t &reply_callback) {
  return georadius(key, longitude, latitude, radius, unit, with_coord,
                   with_dist, with_hash, asc_order, count, "", "",
                   reply_callback);
}

client &client::georadius(const string_t &key, double longitude,
                          double latitude, double radius, geo_unit unit,
                          bool with_coord, bool with_dist, bool with_hash,
                          bool asc_order, const string_t &store_key,
                          const reply_callback_t &reply_callback) {
  return georadius(key, longitude, latitude, radius, unit, with_coord,
                   with_dist, with_hash, asc_order, 0, store_key, "",
                   reply_callback);
}

client &client::georadius(const string_t &key, double longitude,
                          double latitude, double radius, geo_unit unit,
                          bool with_coord, bool with_dist, bool with_hash,
                          bool asc_order, const string_t &store_key,
                          const string_t &storedist_key,
                          const reply_callback_t &reply_callback) {
  return georadius(key, longitude, latitude, radius, unit, with_coord,
                   with_dist, with_hash, asc_order, 0, store_key, storedist_key,
                   reply_callback);
}

client &client::georadius(const string_t &key, double longitude,
                          double latitude, double radius, geo_unit unit,
                          bool with_coord, bool with_dist, bool with_hash,
                          bool asc_order, std::size_t count,
                          const string_t &store_key,
                          const reply_callback_t &reply_callback) {
  return georadius(key, longitude, latitude, radius, unit, with_coord,
                   with_dist, with_hash, asc_order, count, store_key, "",
                   reply_callback);
}

client &client::georadius(const string_t &key, double longitude,
                          double latitude, double radius, geo_unit unit,
                          bool with_coord, bool with_dist, bool with_hash,
                          bool asc_order, std::size_t count,
                          const string_t &store_key,
                          const string_t &storedist_key,
                          const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"GEORADIUS",
                               key,
                               std::to_string(longitude),
                               std::to_string(latitude),
                               std::to_string(radius),
                               geo_unit_to_string(unit)};

  //!
  //!  with_coord (optional)
  //!
  if (with_coord) {
    cmd.emplace_back("WITHCOORD");
  }

  //!
  //!  with_dist (optional)
  //!
  if (with_dist) {
    cmd.emplace_back("WITHDIST");
  }

  //!
  //!  with_hash (optional)
  //!
  if (with_hash) {
    cmd.emplace_back("WITHHASH");
  }

  //!
  //!  order (optional)
  //!
  cmd.emplace_back(asc_order ? "ASC" : "DESC");

  //!
  //!  count (optional)
  //!
  if (count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(count));
  }

  //!
  //!  store_key (optional)
  //!
  if (!store_key.empty()) {
    cmd.emplace_back("STOREDIST");
    cmd.push_back(storedist_key);
  }

  //!
  //!  storedist_key (optional)
  //!
  if (!storedist_key.empty()) {
    cmd.emplace_back("STOREDIST");
    cmd.push_back(storedist_key);
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::georadiusbymember(const string_t &key, const string_t &member,
                                  double radius, geo_unit unit, bool with_coord,
                                  bool with_dist, bool with_hash,
                                  bool asc_order,
                                  const reply_callback_t &reply_callback) {
  return georadiusbymember(key, member, radius, unit, with_coord, with_dist,
                           with_hash, asc_order, 0, "", "", reply_callback);
}

client &client::georadiusbymember(const string_t &key, const string_t &member,
                                  double radius, geo_unit unit, bool with_coord,
                                  bool with_dist, bool with_hash,
                                  bool asc_order, std::size_t count,
                                  const reply_callback_t &reply_callback) {
  return georadiusbymember(key, member, radius, unit, with_coord, with_dist,
                           with_hash, asc_order, count, "", "", reply_callback);
}

client &client::georadiusbymember(const string_t &key, const string_t &member,
                                  double radius, geo_unit unit, bool with_coord,
                                  bool with_dist, bool with_hash,
                                  bool asc_order, const string_t &store_key,
                                  const reply_callback_t &reply_callback) {
  return georadiusbymember(key, member, radius, unit, with_coord, with_dist,
                           with_hash, asc_order, 0, store_key, "",
                           reply_callback);
}

client &client::georadiusbymember(const string_t &key, const string_t &member,
                                  double radius, geo_unit unit, bool with_coord,
                                  bool with_dist, bool with_hash,
                                  bool asc_order, const string_t &store_key,
                                  const string_t &storedist_key,
                                  const reply_callback_t &reply_callback) {
  return georadiusbymember(key, member, radius, unit, with_coord, with_dist,
                           with_hash, asc_order, 0, store_key, storedist_key,
                           reply_callback);
}

client &client::georadiusbymember(const string_t &key, const string_t &member,
                                  double radius, geo_unit unit, bool with_coord,
                                  bool with_dist, bool with_hash,
                                  bool asc_order, std::size_t count,
                                  const string_t &store_key,
                                  const reply_callback_t &reply_callback) {
  return georadiusbymember(key, member, radius, unit, with_coord, with_dist,
                           with_hash, asc_order, count, store_key, "",
                           reply_callback);
}

client &client::georadiusbymember(const string_t &key, const string_t &member,
                                  double radius, geo_unit unit, bool with_coord,
                                  bool with_dist, bool with_hash,
                                  bool asc_order, std::size_t count,
                                  const string_t &store_key,
                                  const string_t &storedist_key,
                                  const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"GEORADIUSBYMEMBER", key, member,
                               std::to_string(radius),
                               geo_unit_to_string(unit)};

  //!
  //!  with_coord (optional)
  //!
  if (with_coord) {
    cmd.emplace_back("WITHCOORD");
  }

  //!
  //!  with_dist (optional)
  //!
  if (with_dist) {
    cmd.emplace_back("WITHDIST");
  }

  //!
  //!  with_hash (optional)
  //!
  if (with_hash) {
    cmd.emplace_back("WITHHASH");
  }

  //!
  //!  order (optional)
  //!
  cmd.emplace_back(asc_order ? "ASC" : "DESC");

  //!
  //!  count (optional)
  //!
  if (count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(count));
  }

  //!
  //!  store_key (optional)
  //!
  if (!store_key.empty()) {
    cmd.emplace_back("STOREDIST");
    cmd.push_back(storedist_key);
  }

  //!
  //!  storedist_key (optional)
  //!
  if (!storedist_key.empty()) {
    cmd.emplace_back("STOREDIST");
    cmd.push_back(storedist_key);
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::get(const string_t &key,
                    const reply_callback_t &reply_callback) {
  send({"GET", key}, reply_callback);
  return *this;
}

client &client::getbit(const string_t &key, int offset,
                       const reply_callback_t &reply_callback) {
  send({"GETBIT", key, std::to_string(offset)}, reply_callback);
  return *this;
}

client &client::getrange(const string_t &key, int start, int end,
                         const reply_callback_t &reply_callback) {
  send({"GETRANGE", key, std::to_string(start), std::to_string(end)},
       reply_callback);
  return *this;
}

client &client::getset(const string_t &key, const string_t &val,
                       const reply_callback_t &reply_callback) {
  send({"GETSET", key, val}, reply_callback);
  return *this;
}

client &client::hdel(const string_t &key, const std::vector<string_t> &fields,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"HDEL", key};
  cmd.insert(cmd.end(), fields.begin(), fields.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::hexists(const string_t &key, const string_t &field,
                        const reply_callback_t &reply_callback) {
  send({"HEXISTS", key, field}, reply_callback);
  return *this;
}

client &client::hget(const string_t &key, const string_t &field,
                     const reply_callback_t &reply_callback) {
  send({"HGET", key, field}, reply_callback);
  return *this;
}

client &client::hgetall(const string_t &key,
                        const reply_callback_t &reply_callback) {
  send({"HGETALL", key}, reply_callback);
  return *this;
}

client &client::hincrby(const string_t &key, const string_t &field, int incr,
                        const reply_callback_t &reply_callback) {
  send({"HINCRBY", key, field, std::to_string(incr)}, reply_callback);
  return *this;
}

client &client::hincrbyfloat(const string_t &key, const string_t &field,
                             float incr,
                             const reply_callback_t &reply_callback) {
  send({"HINCRBYFLOAT", key, field, std::to_string(incr)}, reply_callback);
  return *this;
}

client &client::hkeys(const string_t &key,
                      const reply_callback_t &reply_callback) {
  send({"HKEYS", key}, reply_callback);
  return *this;
}

client &client::hlen(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"HLEN", key}, reply_callback);
  return *this;
}

client &client::hmget(const string_t &key, const std::vector<string_t> &fields,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"HMGET", key};
  cmd.insert(cmd.end(), fields.begin(), fields.end());
  send(cmd, reply_callback);
  return *this;
}

client &
client::hmset(const string_t &key,
              const std::vector<std::pair<string_t, string_t>> &field_val,
              const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"HMSET", key};
  for (const auto &obj : field_val) {
    cmd.push_back(obj.first);
    cmd.push_back(obj.second);
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::hscan(const string_t &key, std::size_t cursor,
                      const reply_callback_t &reply_callback) {
  return hscan(key, cursor, "", 0, reply_callback);
}

client &client::hscan(const string_t &key, std::size_t cursor,
                      const string_t &pattern,
                      const reply_callback_t &reply_callback) {
  return hscan(key, cursor, pattern, 0, reply_callback);
}

client &client::hscan(const string_t &key, std::size_t cursor,
                      std::size_t count,
                      const reply_callback_t &reply_callback) {
  return hscan(key, cursor, "", count, reply_callback);
}

client &client::hscan(const string_t &key, std::size_t cursor,
                      const string_t &pattern, std::size_t count,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"HSCAN", key, std::to_string(cursor)};

  if (!pattern.empty()) {
    cmd.emplace_back("MATCH");
    cmd.push_back(pattern);
  }

  if (count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::hset(const string_t &key, const string_t &field,
                     const string_t &value,
                     const reply_callback_t &reply_callback) {
  send({"HSET", key, field, value}, reply_callback);
  return *this;
}

client &client::hsetnx(const string_t &key, const string_t &field,
                       const string_t &value,
                       const reply_callback_t &reply_callback) {
  send({"HSETNX", key, field, value}, reply_callback);
  return *this;
}

client &client::hstrlen(const string_t &key, const string_t &field,
                        const reply_callback_t &reply_callback) {
  send({"HSTRLEN", key, field}, reply_callback);
  return *this;
}

client &client::hvals(const string_t &key,
                      const reply_callback_t &reply_callback) {
  send({"HVALS", key}, reply_callback);
  return *this;
}

client &client::incr(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"INCR", key}, reply_callback);
  return *this;
}

client &client::incrby(const string_t &key, int incr,
                       const reply_callback_t &reply_callback) {
  send({"INCRBY", key, std::to_string(incr)}, reply_callback);
  return *this;
}

client &client::incrbyfloat(const string_t &key, float incr,
                            const reply_callback_t &reply_callback) {
  send({"INCRBYFLOAT", key, std::to_string(incr)}, reply_callback);
  return *this;
}

client &client::info(const reply_callback_t &reply_callback) {
  send({"INFO"}, reply_callback);
  return *this;
}

client &client::info(const string_t &section,
                     const reply_callback_t &reply_callback) {
  send({"INFO", section}, reply_callback);
  return *this;
}

client &client::keys(const string_t &pattern,
                     const reply_callback_t &reply_callback) {
  send({"KEYS", pattern}, reply_callback);
  return *this;
}

client &client::lastsave(const reply_callback_t &reply_callback) {
  send({"LASTSAVE"}, reply_callback);
  return *this;
}

client &client::lindex(const string_t &key, int index,
                       const reply_callback_t &reply_callback) {
  send({"LINDEX", key, std::to_string(index)}, reply_callback);
  return *this;
}

client &client::linsert(const string_t &key, const string_t &before_after,
                        const string_t &pivot, const string_t &value,
                        const reply_callback_t &reply_callback) {
  send({"LINSERT", key, before_after, pivot, value}, reply_callback);
  return *this;
}

client &client::llen(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"LLEN", key}, reply_callback);
  return *this;
}

client &client::lpop(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"LPOP", key}, reply_callback);
  return *this;
}

client &client::lpush(const string_t &key, const std::vector<string_t> &values,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"LPUSH", key};
  cmd.insert(cmd.end(), values.begin(), values.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::lpushx(const string_t &key, const string_t &value,
                       const reply_callback_t &reply_callback) {
  send({"LPUSHX", key, value}, reply_callback);
  return *this;
}

client &client::lrange(const string_t &key, int start, int stop,
                       const reply_callback_t &reply_callback) {
  send({"LRANGE", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::lrem(const string_t &key, int count, const string_t &value,
                     const reply_callback_t &reply_callback) {
  send({"LREM", key, std::to_string(count), value}, reply_callback);
  return *this;
}

client &client::lset(const string_t &key, int index, const string_t &value,
                     const reply_callback_t &reply_callback) {
  send({"LSET", key, std::to_string(index), value}, reply_callback);
  return *this;
}

client &client::ltrim(const string_t &key, int start, int stop,
                      const reply_callback_t &reply_callback) {
  send({"LTRIM", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::mget(const std::vector<string_t> &keys,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"MGET"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::migrate(const string_t &host, int port, const string_t &key,
                        const string_t &dest_db, int timeout,
                        const reply_callback_t &reply_callback) {
  send({"MIGRATE", host, std::to_string(port), key, dest_db,
        std::to_string(timeout)},
       reply_callback);
  return *this;
}

client &client::migrate(const string_t &host, int port, const string_t &key,
                        const string_t &dest_db, int timeout, bool copy,
                        bool replace, const std::vector<string_t> &keys,
                        const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"MIGRATE", host,    std::to_string(port),
                               key,       dest_db, std::to_string(timeout)};
  if (copy) {
    cmd.emplace_back("COPY");
  }
  if (replace) {
    cmd.emplace_back("REPLACE");
  }
  if (!keys.empty()) {
    cmd.emplace_back("KEYS");
    cmd.insert(cmd.end(), keys.begin(), keys.end());
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::monitor(const reply_callback_t &reply_callback) {
  send({"MONITOR"}, reply_callback);
  return *this;
}

client &client::move(const string_t &key, const string_t &db,
                     const reply_callback_t &reply_callback) {
  send({"MOVE", key, db}, reply_callback);
  return *this;
}

client &client::mset(const std::vector<std::pair<string_t, string_t>> &key_vals,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"MSET"};
  for (const auto &obj : key_vals) {
    cmd.push_back(obj.first);
    cmd.push_back(obj.second);
  }
  send(cmd, reply_callback);
  return *this;
}

client &
client::msetnx(const std::vector<std::pair<string_t, string_t>> &key_vals,
               const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"MSETNX"};
  for (const auto &obj : key_vals) {
    cmd.push_back(obj.first);
    cmd.push_back(obj.second);
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::multi(const reply_callback_t &reply_callback) {
  send({"MULTI"}, reply_callback);
  return *this;
}

client &client::object(const string_t &subcommand,
                       const std::vector<string_t> &args,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"OBJECT", subcommand};
  cmd.insert(cmd.end(), args.begin(), args.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::persist(const string_t &key,
                        const reply_callback_t &reply_callback) {
  send({"PERSIST", key}, reply_callback);
  return *this;
}

client &client::pexpire(const string_t &key, int ms,
                        const reply_callback_t &reply_callback) {
  send({"PEXPIRE", key, std::to_string(ms)}, reply_callback);
  return *this;
}

client &client::pexpireat(const string_t &key, int ms_timestamp,
                          const reply_callback_t &reply_callback) {
  send({"PEXPIREAT", key, std::to_string(ms_timestamp)}, reply_callback);
  return *this;
}

client &client::pfadd(const string_t &key,
                      const std::vector<string_t> &elements,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"PFADD", key};
  cmd.insert(cmd.end(), elements.begin(), elements.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::pfcount(const std::vector<string_t> &keys,
                        const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"PFCOUNT"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::pfmerge(const string_t &destkey,
                        const std::vector<string_t> &sourcekeys,
                        const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"PFMERGE", destkey};
  cmd.insert(cmd.end(), sourcekeys.begin(), sourcekeys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::ping(const reply_callback_t &reply_callback) {
  send({"PING"}, reply_callback);
  return *this;
}

client &client::ping(const string_t &message,
                     const reply_callback_t &reply_callback) {
  send({"PING", message}, reply_callback);
  return *this;
}

client &client::psetex(const string_t &key, int64_t ms, const string_t &val,
                       const reply_callback_t &reply_callback) {
  send({"PSETEX", key, std::to_string(ms), val}, reply_callback);
  return *this;
}

client &client::publish(const string_t &channel, const string_t &message,
                        const reply_callback_t &reply_callback) {
  send({"PUBLISH", channel, message}, reply_callback);
  return *this;
}

client &client::pubsub(const string_t &subcommand,
                       const std::vector<string_t> &args,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"PUBSUB", subcommand};
  cmd.insert(cmd.end(), args.begin(), args.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::pttl(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"PTTL", key}, reply_callback);
  return *this;
}

client &client::quit(const reply_callback_t &reply_callback) {
  send({"QUIT"}, reply_callback);
  return *this;
}

client &client::randomkey(const reply_callback_t &reply_callback) {
  send({"RANDOMKEY"}, reply_callback);
  return *this;
}

client &client::readonly(const reply_callback_t &reply_callback) {
  send({"READONLY"}, reply_callback);
  return *this;
}

client &client::readwrite(const reply_callback_t &reply_callback) {
  send({"READWRITE"}, reply_callback);
  return *this;
}

client &client::rename(const string_t &key, const string_t &newkey,
                       const reply_callback_t &reply_callback) {
  send({"RENAME", key, newkey}, reply_callback);
  return *this;
}

client &client::renamenx(const string_t &key, const string_t &newkey,
                         const reply_callback_t &reply_callback) {
  send({"RENAMENX", key, newkey}, reply_callback);
  return *this;
}

client &client::restore(const string_t &key, int ttl,
                        const string_t &serialized_value,
                        const reply_callback_t &reply_callback) {
  send({"RESTORE", key, std::to_string(ttl), serialized_value}, reply_callback);
  return *this;
}

client &client::restore(const string_t &key, int ttl,
                        const string_t &serialized_value,
                        const string_t &replace,
                        const reply_callback_t &reply_callback) {
  send({"RESTORE", key, std::to_string(ttl), serialized_value, replace},
       reply_callback);
  return *this;
}

client &client::role(const reply_callback_t &reply_callback) {
  send({"ROLE"}, reply_callback);
  return *this;
}

client &client::rpop(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"RPOP", key}, reply_callback);
  return *this;
}

client &client::rpoplpush(const string_t &source, const string_t &destination,
                          const reply_callback_t &reply_callback) {
  send({"RPOPLPUSH", source, destination}, reply_callback);
  return *this;
}

client &client::rpush(const string_t &key, const std::vector<string_t> &values,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"RPUSH", key};
  cmd.insert(cmd.end(), values.begin(), values.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::rpushx(const string_t &key, const string_t &value,
                       const reply_callback_t &reply_callback) {
  send({"RPUSHX", key, value}, reply_callback);
  return *this;
}

client &client::sadd(const string_t &key, const std::vector<string_t> &members,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SADD", key};
  cmd.insert(cmd.end(), members.begin(), members.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::save(const reply_callback_t &reply_callback) {
  send({"SAVE"}, reply_callback);
  return *this;
}

client &client::scan(std::size_t cursor,
                     const reply_callback_t &reply_callback) {
  return scan(cursor, "", 0, reply_callback);
}

client &client::scan(std::size_t cursor, const string_t &pattern,
                     const reply_callback_t &reply_callback) {
  return scan(cursor, pattern, 0, reply_callback);
}

client &client::scan(std::size_t cursor, std::size_t count,
                     const reply_callback_t &reply_callback) {
  return scan(cursor, "", count, reply_callback);
}

client &client::scan(std::size_t cursor, const string_t &pattern,
                     std::size_t count,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SCAN", std::to_string(cursor)};

  if (!pattern.empty()) {
    cmd.emplace_back("MATCH");
    cmd.push_back(pattern);
  }

  if (count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::scard(const string_t &key,
                      const reply_callback_t &reply_callback) {
  send({"SCARD", key}, reply_callback);
  return *this;
}

client &client::script_debug(const string_t &mode,
                             const reply_callback_t &reply_callback) {
  send({"SCRIPT", "DEBUG", mode}, reply_callback);
  return *this;
}

client &client::script_exists(const std::vector<string_t> &scripts,
                              const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SCRIPT", "EXISTS"};
  cmd.insert(cmd.end(), scripts.begin(), scripts.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::script_flush(const reply_callback_t &reply_callback) {
  send({"SCRIPT", "FLUSH"}, reply_callback);
  return *this;
}

client &client::script_kill(const reply_callback_t &reply_callback) {
  send({"SCRIPT", "KILL"}, reply_callback);
  return *this;
}

client &client::script_load(const string_t &script,
                            const reply_callback_t &reply_callback) {
  send({"SCRIPT", "LOAD", script}, reply_callback);
  return *this;
}

client &client::sdiff(const std::vector<string_t> &keys,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SDIFF"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::sdiffstore(const string_t &destination,
                           const std::vector<string_t> &keys,
                           const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SDIFFSTORE", destination};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::select(int index, const reply_callback_t &reply_callback) {
  std::lock_guard<std::mutex> lock(m_callbacks_mutex);

  unprotected_select(index, reply_callback);

  return *this;
}

void client::unprotected_select(int index,
                                const reply_callback_t &reply_callback) {
  //!
  //!  save the index of the database for reconnect attempts.
  //!
  m_database_index = index;
  //!
  //!  save command in the pipeline
  //!
  unprotected_send({"SELECT", std::to_string(index)}, reply_callback);
}

client &client::set(const string_t &key, const string_t &value,
                    const reply_callback_t &reply_callback) {
  send({"SET", key, value}, reply_callback);
  return *this;
}

client &client::set_advanced(const string_t &key, const string_t &value,
                             const reply_callback_t &reply_callback) {
  send({"SET", key, value}, reply_callback);
  return *this;
}

client &client::set_advanced(const string_t &key, const string_t &value,
                             bool ex, int ex_sec, bool px, int px_milli,
                             bool nx, bool xx,
                             const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SET", key, value};
  if (ex) {
    cmd.emplace_back("EX");
    cmd.push_back(std::to_string(ex_sec));
  }
  if (px) {
    cmd.emplace_back("PX");
    cmd.push_back(std::to_string(px_milli));
  }
  if (nx) {
    cmd.emplace_back("NX");
  }
  if (xx) {
    cmd.emplace_back("XX");
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::setbit(const string_t &key, int offset, const string_t &value,
                       const reply_callback_t &reply_callback) {
  send({"SETBIT", key, std::to_string(offset), value}, reply_callback);
  return *this;
}

client &client::setex(const string_t &key, int64_t seconds,
                      const string_t &value,
                      const reply_callback_t &reply_callback) {
  send({"SETEX", key, std::to_string(seconds), value}, reply_callback);
  return *this;
}

client &client::setnx(const string_t &key, const string_t &value,
                      const reply_callback_t &reply_callback) {
  send({"SETNX", key, value}, reply_callback);
  return *this;
}

client &client::set_range(const string_t &key, int offset,
                          const string_t &value,
                          const reply_callback_t &reply_callback) {
  send({"SETRANGE", key, std::to_string(offset), value}, reply_callback);
  return *this;
}

client &client::shutdown(const reply_callback_t &reply_callback) {
  send({"SHUTDOWN"}, reply_callback);
  return *this;
}

client &client::shutdown(const string_t &save,
                         const reply_callback_t &reply_callback) {
  send({"SHUTDOWN", save}, reply_callback);
  return *this;
}

client &client::sinter(const std::vector<string_t> &keys,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SINTER"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::sinterstore(const string_t &destination,
                            const std::vector<string_t> &keys,
                            const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SINTERSTORE", destination};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::sismember(const string_t &key, const string_t &member,
                          const reply_callback_t &reply_callback) {
  send({"SISMEMBER", key, member}, reply_callback);
  return *this;
}

client &client::slaveof(const string_t &host, int port,
                        const reply_callback_t &reply_callback) {
  send({"REPLICAOF", host, std::to_string(port)}, reply_callback);
  return *this;
}

client &client::replicaof(const string_t &host, int port,
                          const reply_callback_t &reply_callback) {
  send({"SLAVEOF", host, std::to_string(port)}, reply_callback);
  return *this;
}

client &client::slowlog(const string_t subcommand,
                        const reply_callback_t &reply_callback) {
  send({"SLOWLOG", subcommand}, reply_callback);
  return *this;
}

client &client::slowlog(string_t sub_cmd, const string_t &argument,
                        const reply_callback_t &reply_callback) {
  send({"SLOWLOG", sub_cmd, argument}, reply_callback);
  return *this;
}

client &client::smembers(const string_t &key,
                         const reply_callback_t &reply_callback) {
  send({"SMEMBERS", key}, reply_callback);
  return *this;
}

client &client::smove(const string_t &source, const string_t &destination,
                      const string_t &member,
                      const reply_callback_t &reply_callback) {
  send({"SMOVE", source, destination, member}, reply_callback);
  return *this;
}

client &client::sort(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"SORT", key}, reply_callback);
  return *this;
}

client &client::sort(const string_t &key,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const reply_callback_t &reply_callback) {
  return sort(key, "", false, 0, 0, get_patterns, asc_order, alpha, "",
              reply_callback);
}

client &client::sort(const string_t &key, std::size_t offset, std::size_t count,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const reply_callback_t &reply_callback) {
  return sort(key, "", true, offset, count, get_patterns, asc_order, alpha, "",
              reply_callback);
}

client &client::sort(const string_t &key, const string_t &by_pattern,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const reply_callback_t &reply_callback) {
  return sort(key, by_pattern, false, 0, 0, get_patterns, asc_order, alpha, "",
              reply_callback);
}

client &client::sort(const string_t &key,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const string_t &store_dest,
                     const reply_callback_t &reply_callback) {
  return sort(key, "", false, 0, 0, get_patterns, asc_order, alpha, store_dest,
              reply_callback);
}

client &client::sort(const string_t &key, std::size_t offset, std::size_t count,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const string_t &store_dest,
                     const reply_callback_t &reply_callback) {
  return sort(key, "", true, offset, count, get_patterns, asc_order, alpha,
              store_dest, reply_callback);
}

client &client::sort(const string_t &key, const string_t &by_pattern,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const string_t &store_dest,
                     const reply_callback_t &reply_callback) {
  return sort(key, by_pattern, false, 0, 0, get_patterns, asc_order, alpha,
              store_dest, reply_callback);
}

client &client::sort(const string_t &key, const string_t &by_pattern,
                     std::size_t offset, std::size_t count,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const reply_callback_t &reply_callback) {
  return sort(key, by_pattern, true, offset, count, get_patterns, asc_order,
              alpha, "", reply_callback);
}

client &client::sort(const string_t &key, const string_t &by_pattern,
                     std::size_t offset, std::size_t count,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const string_t &store_dest,
                     const reply_callback_t &reply_callback) {
  return sort(key, by_pattern, true, offset, count, get_patterns, asc_order,
              alpha, store_dest, reply_callback);
}

client &client::sort(const string_t &key, const string_t &by_pattern,
                     bool limit, std::size_t offset, std::size_t count,
                     const std::vector<string_t> &get_patterns, bool asc_order,
                     bool alpha, const string_t &store_dest,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SORT", key};

  //!
  //!  add by pattern (optional)
  //!
  if (!by_pattern.empty()) {
    cmd.emplace_back("BY");
    cmd.push_back(by_pattern);
  }

  //!
  //!  add limit (optional)
  //!
  if (limit) {
    cmd.emplace_back("LIMIT");
    cmd.push_back(std::to_string(offset));
    cmd.push_back(std::to_string(count));
  }

  //!
  //!  add get pattern (optional)
  //!
  for (const auto &get_pattern : get_patterns) {
    if (get_pattern.empty()) {
      continue;
    }

    cmd.emplace_back("GET");
    cmd.push_back(get_pattern);
  }

  //!
  //!  add order by (optional)
  //!
  cmd.emplace_back(asc_order ? "ASC" : "DESC");

  //!
  //!  add alpha (optional)
  //!
  if (alpha) {
    cmd.emplace_back("ALPHA");
  }

  //!
  //!  add store dest (optional)
  //!
  if (!store_dest.empty()) {
    cmd.emplace_back("STORE");
    cmd.push_back(store_dest);
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::spop(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"SPOP", key}, reply_callback);
  return *this;
}

client &client::spop(const string_t &key, int count,
                     const reply_callback_t &reply_callback) {
  send({"SPOP", key, std::to_string(count)}, reply_callback);
  return *this;
}

client &client::srandmember(const string_t &key,
                            const reply_callback_t &reply_callback) {
  send({"SRANDMEMBER", key}, reply_callback);
  return *this;
}

client &client::srandmember(const string_t &key, int count,
                            const reply_callback_t &reply_callback) {
  send({"SRANDMEMBER", key, std::to_string(count)}, reply_callback);
  return *this;
}

client &client::srem(const string_t &key, const std::vector<string_t> &members,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SREM", key};
  cmd.insert(cmd.end(), members.begin(), members.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::sscan(const string_t &key, std::size_t cursor,
                      const reply_callback_t &reply_callback) {
  return sscan(key, cursor, "", 0, reply_callback);
}

client &client::sscan(const string_t &key, std::size_t cursor,
                      const string_t &pattern,
                      const reply_callback_t &reply_callback) {
  return sscan(key, cursor, pattern, 0, reply_callback);
}

client &client::sscan(const string_t &key, std::size_t cursor,
                      std::size_t count,
                      const reply_callback_t &reply_callback) {
  return sscan(key, cursor, "", count, reply_callback);
}

client &client::sscan(const string_t &key, std::size_t cursor,
                      const string_t &pattern, std::size_t count,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SSCAN", key, std::to_string(cursor)};

  if (!pattern.empty()) {
    cmd.emplace_back("MATCH");
    cmd.push_back(pattern);
  }

  if (count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::strlen(const string_t &key,
                       const reply_callback_t &reply_callback) {
  send({"STRLEN", key}, reply_callback);
  return *this;
}

client &client::sunion(const std::vector<string_t> &keys,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SUNION"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::sunionstore(const string_t &destination,
                            const std::vector<string_t> &keys,
                            const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"SUNIONSTORE", destination};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::sync(const reply_callback_t &reply_callback) {
  send({"SYNC"}, reply_callback);
  return *this;
}

client &client::time(const reply_callback_t &reply_callback) {
  send({"TIME"}, reply_callback);
  return *this;
}

client &client::ttl(const string_t &key,
                    const reply_callback_t &reply_callback) {
  send({"TTL", key}, reply_callback);
  return *this;
}

client &client::type(const string_t &key,
                     const reply_callback_t &reply_callback) {
  send({"TYPE", key}, reply_callback);
  return *this;
}

client &client::unwatch(const reply_callback_t &reply_callback) {
  send({"UNWATCH"}, reply_callback);
  return *this;
}

client &client::wait(int num_replicas, int timeout,
                     const reply_callback_t &reply_callback) {
  send({"WAIT", std::to_string(num_replicas), std::to_string(timeout)},
       reply_callback);
  return *this;
}

client &client::watch(const std::vector<string_t> &keys,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"WATCH"};
  cmd.insert(cmd.end(), keys.begin(), keys.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::xack(const string_t &stream, const string_t &group,
                     const std::vector<string_t> &message_ids,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XACK", stream, group};

  //!
  //!  ids
  //!
  for (auto &id : message_ids) {
    cmd.push_back(id);
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::xadd(const string_t &key, const string_t &id,
                     const std::multimap<string_t, string_t> &field_members,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XADD", key, id};

  //!
  //!  score members
  //!
  for (auto &sm : field_members) {
    cmd.push_back(sm.first);
    cmd.push_back(sm.second);
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::xclaim(const string_t &stream, const string_t &group,
                       const string_t &consumer, int min_idle_time,
                       const std::vector<string_t> &message_ids,
                       const xclaim_options_t &options,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XCLAIM", stream, group, consumer,
                               std::to_string(min_idle_time)};

  //!
  //!  ids
  //!
  for (auto &id : message_ids) {
    cmd.push_back(id);
  }
  if (options.Idle > 0) {
    cmd.emplace_back("IDLE");
    cmd.push_back(std::to_string(options.Idle));
  }
  if (options.Time != nullptr) {
    cmd.emplace_back("TIME");
    cmd.push_back(std::to_string(static_cast<long int>(*options.Time)));
  }
  if (options.RetryCount > 0) {
    cmd.emplace_back("RETRYCOUNT");
    cmd.push_back(std::to_string(options.RetryCount));
  }
  if (options.Force) {
    cmd.emplace_back("FORCE");
  }
  if (options.JustId) {
    cmd.emplace_back("JUSTID");
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::xdel(const string_t &key,
                     const std::vector<string_t> &id_members,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XDEL", key};

  //!
  //!  ids
  //!
  for (auto &id : id_members) {
    cmd.push_back(id);
  }

  send(cmd, reply_callback);
  return *this;
}

//<editor-fold desc="XGroup">
client &client::xgroup_create(const string_t &key, const string_t &group_name,
                              const reply_callback_t &reply_callback) {
  send({"XGROUP", "CREATE", key, group_name, "$"}, reply_callback);
  return *this;
}

client &client::xgroup_create(const string_t &key, const string_t &group_name,
                              const string_t &id,
                              const reply_callback_t &reply_callback) {
  send({"XGROUP", "CREATE", key, group_name, id}, reply_callback);
  return *this;
}

client &client::xgroup_set_id(const string_t &key, const string_t &group_name,
                              const reply_callback_t &reply_callback) {
  send({"XGROUP", "SETID", key, group_name, "$"}, reply_callback);
  return *this;
}

client &client::xgroup_set_id(const string_t &key, const string_t &group_name,
                              const string_t &id,
                              const reply_callback_t &reply_callback) {
  send({"XGROUP", "SETID", key, group_name, id}, reply_callback);
  return *this;
}

client &client::xgroup_destroy(const string_t &key, const string_t &group_name,
                               const reply_callback_t &reply_callback) {
  send({"XGROUP", "DESTROY", key, group_name}, reply_callback);
  return *this;
}

client &client::xgroup_del_consumer(const string_t &key,
                                    const string_t &group_name,
                                    const string_t &consumer_name,
                                    const reply_callback_t &reply_callback) {
  send({"XGROUP", "DELCONSUMER", key, group_name, consumer_name},
       reply_callback);
  return *this;
}
//</editor-fold>

client &client::xinfo_consumers(const string_t &key, const string_t &group_name,
                                const reply_callback_t &reply_callback) {
  send({"XINFO", "CONSUMERS", key, group_name}, reply_callback);
  return *this;
}

client &client::xinfo_groups(const string_t &key,
                             const reply_callback_t &reply_callback) {
  send({"XINFO", "GROUPS", key}, reply_callback);
  return *this;
}

client &client::xinfo_stream(const string_t &stream,
                             const reply_callback_t &reply_callback) {
  send({"XINFO", "STREAM", stream}, reply_callback);
  return *this;
}

//!
//!  @brief Returns the number of entries inside a stream.
//!  If the specified key does not exist the command returns zero, as if the
//!  stream was empty. However note that unlike other Redis types, zero-length
//!  streams are possible, so you should call TYPE or EXISTS in order to check
//!  if a key exists or not. Streams are not auto-deleted once they have no
//!  entries inside (for instance after an XDEL call), because the stream may
//!  have consumer groups associated with it.
//!  @param stream
//!  @param reply_callback
//!  @return Integer reply: the number of entries of the stream at key.
//!
client &client::xlen(const string_t &stream,
                     const reply_callback_t &reply_callback) {
  send({"XLEN", stream}, reply_callback);
  return *this;
}

client &client::xpending(const string_t &stream, const string_t &group,
                         const xpending_options_t &options,
                         const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XPENDING", stream, group};
  if (!options.Range.Start.empty()) {
    cmd.emplace_back(options.Range.Start);
    cmd.emplace_back(options.Range.Stop);
    cmd.emplace_back(std::to_string(options.Range.Count));
  }

  if (!options.Consumer.empty()) {
    cmd.push_back(options.Consumer);
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::xrange(const string_t &stream, const range_options_t &options,
                       const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XRANGE", stream, options.Start, options.Stop};
  if (options.Count > 0) {
    cmd.emplace_back("COUNT");
    cmd.emplace_back(std::to_string(options.Count));
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::xread(const xread_options_t &a,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XREAD"};
  if (a.Count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(a.Count));
  }

  if (a.Block >= 0) {
    cmd.emplace_back("BLOCK");
    cmd.push_back(std::to_string(a.Block));
  }

  // Add streams
  cmd.emplace_back("STREAMS");
  cmd.insert(cmd.end(), a.Streams.first.begin(), a.Streams.first.end());
  // Add ids
  cmd.insert(cmd.end(), a.Streams.second.begin(), a.Streams.second.end());

  send(cmd, reply_callback);
  return *this;
}

client &client::xreadgroup(const xreadgroup_options_t &a,
                           const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XREADGROUP", "GROUP", a.Group, a.Consumer};
  if (a.Count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(a.Count));
  }

  if (a.Block >= 0) {
    cmd.emplace_back("BLOCK");
    cmd.push_back(std::to_string(a.Block));
  }

  if (a.NoAck) {
    cmd.emplace_back("NOACK");
  }

  // Add streams
  cmd.emplace_back("STREAMS");
  cmd.insert(cmd.end(), a.Streams.first.begin(), a.Streams.first.end());
  // Add ids
  cmd.insert(cmd.end(), a.Streams.second.begin(), a.Streams.second.end());

  send(cmd, reply_callback);
  return *this;
}

client &client::xrevrange(const string_t &key,
                          const range_options_t &range_args,
                          const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XREVRANGE", key, range_args.Start,
                               range_args.Stop};
  if (range_args.Count > 0) {
    cmd.emplace_back(std::to_string(range_args.Count));
  }
  send(cmd, reply_callback);
  return *this;
}

client &client::xtrim(const string_t &stream, int max_len,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XTRIM", stream, "MAXLEN",
                               std::to_string(max_len)};
  send(cmd, reply_callback);
  return *this;
}

client &
client::xtrim_approx(const string_t &key, int max_len,
                     const cpp_redis::reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"XTRIM", key, "MAXLEN", "~",
                               std::to_string(max_len)};
  send(cmd, reply_callback);
  return *this;
}

client &client::zadd(const string_t &key, const std::vector<string_t> &options,
                     const std::multimap<string_t, string_t> &score_members,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZADD", key};

  //!
  //!  options
  //!
  cmd.insert(cmd.end(), options.begin(), options.end());

  //!
  //!  score members
  //!
  for (auto &sm : score_members) {
    cmd.push_back(sm.first);
    cmd.push_back(sm.second);
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::zcard(const string_t &key,
                      const reply_callback_t &reply_callback) {
  send({"ZCARD", key}, reply_callback);
  return *this;
}

client &client::zcount(const string_t &key, int min, int max,
                       const reply_callback_t &reply_callback) {
  send({"ZCOUNT", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zcount(const string_t &key, double min, double max,
                       const reply_callback_t &reply_callback) {
  send({"ZCOUNT", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zcount(const string_t &key, const string_t &min,
                       const string_t &max,
                       const reply_callback_t &reply_callback) {
  send({"ZCOUNT", key, min, max}, reply_callback);
  return *this;
}

client &client::zincrby(const string_t &key, int incr, const string_t &member,
                        const reply_callback_t &reply_callback) {
  send({"ZINCRBY", key, std::to_string(incr), member}, reply_callback);
  return *this;
}

client &client::zincrby(const string_t &key, double incr,
                        const string_t &member,
                        const reply_callback_t &reply_callback) {
  send({"ZINCRBY", key, std::to_string(incr), member}, reply_callback);
  return *this;
}

client &client::zincrby(const string_t &key, const string_t &incr,
                        const string_t &member,
                        const reply_callback_t &reply_callback) {
  send({"ZINCRBY", key, incr, member}, reply_callback);
  return *this;
}

client &client::zinterstore(const string_t &destination, std::size_t numkeys,
                            const std::vector<string_t> &keys,
                            const std::vector<std::size_t> weights,
                            aggregate_method method,
                            const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZINTERSTORE", destination,
                               std::to_string(numkeys)};

  //!
  //!  keys
  //!
  for (const auto &key : keys) {
    cmd.push_back(key);
  }

  //!
  //!  weights (optional)
  //!
  if (!weights.empty()) {
    cmd.emplace_back("WEIGHTS");

    for (auto weight : weights) {
      cmd.push_back(std::to_string(weight));
    }
  }

  //!
  //!  aggregate method
  //!
  if (method != aggregate_method::server_default) {
    cmd.emplace_back("AGGREGATE");
    cmd.push_back(aggregate_method_to_string(method));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::zlexcount(const string_t &key, int min, int max,
                          const reply_callback_t &reply_callback) {
  send({"ZLEXCOUNT", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zlexcount(const string_t &key, double min, double max,
                          const reply_callback_t &reply_callback) {
  send({"ZLEXCOUNT", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zlexcount(const string_t &key, const string_t &min,
                          const string_t &max,
                          const reply_callback_t &reply_callback) {
  send({"ZLEXCOUNT", key, min, max}, reply_callback);
  return *this;
}

client &client::zpopmin(const string_t &key, int count,
                        const reply_callback_t &reply_callback) {
  send({"ZPOPMIN", key, std::to_string(count)}, reply_callback);
  return *this;
}

client &client::zpopmax(const string_t &key, int count,
                        const reply_callback_t &reply_callback) {
  send({"ZPOPMAX", key, std::to_string(count)}, reply_callback);
  return *this;
}

client &client::zrange(const string_t &key, int start, int stop,
                       const reply_callback_t &reply_callback) {
  send({"ZRANGE", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::zrange(const string_t &key, int start, int stop,
                       bool with_scores,
                       const reply_callback_t &reply_callback) {
  if (with_scores)
    send({"ZRANGE", key, std::to_string(start), std::to_string(stop),
          "WITHSCORES"},
         reply_callback);
  else
    send({"ZRANGE", key, std::to_string(start), std::to_string(stop)},
         reply_callback);
  return *this;
}

client &client::zrange(const string_t &key, double start, double stop,
                       const reply_callback_t &reply_callback) {
  send({"ZRANGE", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::zrange(const string_t &key, double start, double stop,
                       bool with_scores,
                       const reply_callback_t &reply_callback) {
  if (with_scores)
    send({"ZRANGE", key, std::to_string(start), std::to_string(stop),
          "WITHSCORES"},
         reply_callback);
  else
    send({"ZRANGE", key, std::to_string(start), std::to_string(stop)},
         reply_callback);
  return *this;
}

client &client::zrange(const string_t &key, const string_t &start,
                       const string_t &stop,
                       const reply_callback_t &reply_callback) {
  send({"ZRANGE", key, start, stop}, reply_callback);
  return *this;
}

client &client::zrange(const string_t &key, const string_t &start,
                       const string_t &stop, bool with_scores,
                       const reply_callback_t &reply_callback) {
  if (with_scores)
    send({"ZRANGE", key, start, stop, "WITHSCORES"}, reply_callback);
  else
    send({"ZRANGE", key, start, stop}, reply_callback);
  return *this;
}

client &client::zrangebylex(const string_t &key, int min, int max,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), false, 0, 0,
                     false, reply_callback);
}

client &client::zrangebylex(const string_t &key, int min, int max,
                            bool with_scores,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), false, 0, 0,
                     with_scores, reply_callback);
}

client &client::zrangebylex(const string_t &key, double min, double max,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), false, 0, 0,
                     false, reply_callback);
}

client &client::zrangebylex(const string_t &key, double min, double max,
                            bool with_scores,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), false, 0, 0,
                     with_scores, reply_callback);
}

client &client::zrangebylex(const string_t &key, const string_t &min,
                            const string_t &max,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, min, max, false, 0, 0, false, reply_callback);
}

client &client::zrangebylex(const string_t &key, const string_t &min,
                            const string_t &max, bool with_scores,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, min, max, false, 0, 0, with_scores, reply_callback);
}

client &client::zrangebylex(const string_t &key, int min, int max,
                            std::size_t offset, std::size_t count,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), true,
                     offset, count, false, reply_callback);
}

client &client::zrangebylex(const string_t &key, int min, int max,
                            std::size_t offset, std::size_t count,
                            bool with_scores,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), true,
                     offset, count, with_scores, reply_callback);
}

client &client::zrangebylex(const string_t &key, double min, double max,
                            std::size_t offset, std::size_t count,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), true,
                     offset, count, false, reply_callback);
}

client &client::zrangebylex(const string_t &key, double min, double max,
                            std::size_t offset, std::size_t count,
                            bool with_scores,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, std::to_string(min), std::to_string(max), true,
                     offset, count, with_scores, reply_callback);
}

client &client::zrangebylex(const string_t &key, const string_t &min,
                            const string_t &max, std::size_t offset,
                            std::size_t count,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, min, max, true, offset, count, false, reply_callback);
}

client &client::zrangebylex(const string_t &key, const string_t &min,
                            const string_t &max, std::size_t offset,
                            std::size_t count, bool with_scores,
                            const reply_callback_t &reply_callback) {
  return zrangebylex(key, min, max, true, offset, count, with_scores,
                     reply_callback);
}

client &client::zrangebylex(const string_t &key, const string_t &min,
                            const string_t &max, bool limit, std::size_t offset,
                            std::size_t count, bool with_scores,
                            const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZRANGEBYLEX", key, min, max};

  //!
  //!  with_scores (optional)
  //!
  if (with_scores) {
    cmd.emplace_back("WITHSCORES");
  }

  //!
  //!  limit (optional)
  //!
  if (limit) {
    cmd.emplace_back("LIMIT");
    cmd.push_back(std::to_string(offset));
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::zrangebyscore(const string_t &key, int min, int max,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), false, 0,
                       0, false, reply_callback);
}

client &client::zrangebyscore(const string_t &key, int min, int max,
                              bool with_scores,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), false, 0,
                       0, with_scores, reply_callback);
}

client &client::zrangebyscore(const string_t &key, double min, double max,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), false, 0,
                       0, false, reply_callback);
}

client &client::zrangebyscore(const string_t &key, double min, double max,
                              bool with_scores,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), false, 0,
                       0, with_scores, reply_callback);
}

client &client::zrangebyscore(const string_t &key, const string_t &min,
                              const string_t &max,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, min, max, false, 0, 0, false, reply_callback);
}

client &client::zrangebyscore(const string_t &key, const string_t &min,
                              const string_t &max, bool with_scores,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, min, max, false, 0, 0, with_scores, reply_callback);
}

client &client::zrangebyscore(const string_t &key, int min, int max,
                              std::size_t offset, std::size_t count,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), true,
                       offset, count, false, reply_callback);
}

client &client::zrangebyscore(const string_t &key, int min, int max,
                              std::size_t offset, std::size_t count,
                              bool with_scores,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), true,
                       offset, count, with_scores, reply_callback);
}

client &client::zrangebyscore(const string_t &key, double min, double max,
                              std::size_t offset, std::size_t count,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), true,
                       offset, count, false, reply_callback);
}

client &client::zrangebyscore(const string_t &key, double min, double max,
                              std::size_t offset, std::size_t count,
                              bool with_scores,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, std::to_string(min), std::to_string(max), true,
                       offset, count, with_scores, reply_callback);
}

client &client::zrangebyscore(const string_t &key, const string_t &min,
                              const string_t &max, std::size_t offset,
                              std::size_t count,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, min, max, true, offset, count, false,
                       reply_callback);
}

client &client::zrangebyscore(const string_t &key, const string_t &min,
                              const string_t &max, std::size_t offset,
                              std::size_t count, bool with_scores,
                              const reply_callback_t &reply_callback) {
  return zrangebyscore(key, min, max, true, offset, count, with_scores,
                       reply_callback);
}

client &client::zrangebyscore(const string_t &key, const string_t &min,
                              const string_t &max, bool limit,
                              std::size_t offset, std::size_t count,
                              bool with_scores,
                              const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZRANGEBYSCORE", key, min, max};

  //!
  //!  with_scores (optional)
  //!
  if (with_scores) {
    cmd.emplace_back("WITHSCORES");
  }

  //!
  //!  limit (optional)
  //!
  if (limit) {
    cmd.emplace_back("LIMIT");
    cmd.push_back(std::to_string(offset));
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::zrank(const string_t &key, const string_t &member,
                      const reply_callback_t &reply_callback) {
  send({"ZRANK", key, member}, reply_callback);
  return *this;
}

client &client::zrem(const string_t &key, const std::vector<string_t> &members,
                     const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZREM", key};
  cmd.insert(cmd.end(), members.begin(), members.end());
  send(cmd, reply_callback);
  return *this;
}

client &client::zremrangebylex(const string_t &key, int min, int max,
                               const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYLEX", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zremrangebylex(const string_t &key, double min, double max,
                               const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYLEX", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zremrangebylex(const string_t &key, const string_t &min,
                               const string_t &max,
                               const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYLEX", key, min, max}, reply_callback);
  return *this;
}

client &client::zremrangebyrank(const string_t &key, int start, int stop,
                                const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYRANK", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::zremrangebyrank(const string_t &key, double start, double stop,
                                const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYRANK", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::zremrangebyrank(const string_t &key, const string_t &start,
                                const string_t &stop,
                                const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYRANK", key, start, stop}, reply_callback);
  return *this;
}

client &client::zremrangebyscore(const string_t &key, int min, int max,
                                 const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYSCORE", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zremrangebyscore(const string_t &key, double min, double max,
                                 const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYSCORE", key, std::to_string(min), std::to_string(max)},
       reply_callback);
  return *this;
}

client &client::zremrangebyscore(const string_t &key, const string_t &min,
                                 const string_t &max,
                                 const reply_callback_t &reply_callback) {
  send({"ZREMRANGEBYSCORE", key, min, max}, reply_callback);
  return *this;
}

client &client::zrevrange(const string_t &key, int start, int stop,
                          const reply_callback_t &reply_callback) {
  send({"ZREVRANGE", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::zrevrange(const string_t &key, int start, int stop,
                          bool with_scores,
                          const reply_callback_t &reply_callback) {
  if (with_scores)
    send({"ZREVRANGE", key, std::to_string(start), std::to_string(stop),
          "WITHSCORES"},
         reply_callback);
  else
    send({"ZREVRANGE", key, std::to_string(start), std::to_string(stop)},
         reply_callback);
  return *this;
}

client &client::zrevrange(const string_t &key, double start, double stop,
                          const reply_callback_t &reply_callback) {
  send({"ZREVRANGE", key, std::to_string(start), std::to_string(stop)},
       reply_callback);
  return *this;
}

client &client::zrevrange(const string_t &key, double start, double stop,
                          bool with_scores,
                          const reply_callback_t &reply_callback) {
  if (with_scores)
    send({"ZREVRANGE", key, std::to_string(start), std::to_string(stop),
          "WITHSCORES"},
         reply_callback);
  else
    send({"ZREVRANGE", key, std::to_string(start), std::to_string(stop)},
         reply_callback);
  return *this;
}

client &client::zrevrange(const string_t &key, const string_t &start,
                          const string_t &stop,
                          const reply_callback_t &reply_callback) {
  send({"ZREVRANGE", key, start, stop}, reply_callback);
  return *this;
}

client &client::zrevrange(const string_t &key, const string_t &start,
                          const string_t &stop, bool with_scores,
                          const reply_callback_t &reply_callback) {
  if (with_scores)
    send({"ZREVRANGE", key, start, stop, "WITHSCORES"}, reply_callback);
  else
    send({"ZREVRANGE", key, start, stop}, reply_callback);
  return *this;
}

client &client::zrevrangebylex(const string_t &key, int max, int min,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), false, 0,
                        0, false, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, int max, int min,
                               bool with_scores,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), false, 0,
                        0, with_scores, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, double max, double min,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), false, 0,
                        0, false, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, double max, double min,
                               bool with_scores,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), false, 0,
                        0, with_scores, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, const string_t &max,
                               const string_t &min,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, max, min, false, 0, 0, false, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, const string_t &max,
                               const string_t &min, bool with_scores,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, max, min, false, 0, 0, with_scores,
                        reply_callback);
}

client &client::zrevrangebylex(const string_t &key, int max, int min,
                               std::size_t offset, std::size_t count,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), true,
                        offset, count, false, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, int max, int min,
                               std::size_t offset, std::size_t count,
                               bool with_scores,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), true,
                        offset, count, with_scores, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, double max, double min,
                               std::size_t offset, std::size_t count,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), true,
                        offset, count, false, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, double max, double min,
                               std::size_t offset, std::size_t count,
                               bool with_scores,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, std::to_string(max), std::to_string(min), true,
                        offset, count, with_scores, reply_callback);
}

client &client::zrevrangebylex(const string_t &key, const string_t &max,
                               const string_t &min, std::size_t offset,
                               std::size_t count,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, max, min, true, offset, count, false,
                        reply_callback);
}

client &client::zrevrangebylex(const string_t &key, const string_t &max,
                               const string_t &min, std::size_t offset,
                               std::size_t count, bool with_scores,
                               const reply_callback_t &reply_callback) {
  return zrevrangebylex(key, max, min, true, offset, count, with_scores,
                        reply_callback);
}

client &client::zrevrangebylex(const string_t &key, const string_t &max,
                               const string_t &min, bool limit,
                               std::size_t offset, std::size_t count,
                               bool with_scores,
                               const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZREVRANGEBYLEX", key, max, min};

  //!
  //!  with_scores (optional)
  //!
  if (with_scores) {
    cmd.emplace_back("WITHSCORES");
  }

  //!
  //!  limit (optional)
  //!
  if (limit) {
    cmd.emplace_back("LIMIT");
    cmd.push_back(std::to_string(offset));
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::zrevrangebyscore(const string_t &key, int max, int min,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), false,
                          0, 0, false, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, int max, int min,
                                 bool with_scores,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), false,
                          0, 0, with_scores, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, double max, double min,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), false,
                          0, 0, false, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, double max, double min,
                                 bool with_scores,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), false,
                          0, 0, with_scores, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, const string_t &max,
                                 const string_t &min,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, max, min, false, 0, 0, false, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, const string_t &max,
                                 const string_t &min, bool with_scores,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, max, min, false, 0, 0, with_scores,
                          reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, int max, int min,
                                 std::size_t offset, std::size_t count,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), true,
                          offset, count, false, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, int max, int min,
                                 std::size_t offset, std::size_t count,
                                 bool with_scores,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), true,
                          offset, count, with_scores, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, double max, double min,
                                 std::size_t offset, std::size_t count,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), true,
                          offset, count, false, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, double max, double min,
                                 std::size_t offset, std::size_t count,
                                 bool with_scores,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, std::to_string(max), std::to_string(min), true,
                          offset, count, with_scores, reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, const string_t &max,
                                 const string_t &min, std::size_t offset,
                                 std::size_t count,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, max, min, true, offset, count, false,
                          reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, const string_t &max,
                                 const string_t &min, std::size_t offset,
                                 std::size_t count, bool with_scores,
                                 const reply_callback_t &reply_callback) {
  return zrevrangebyscore(key, max, min, true, offset, count, with_scores,
                          reply_callback);
}

client &client::zrevrangebyscore(const string_t &key, const string_t &max,
                                 const string_t &min, bool limit,
                                 std::size_t offset, std::size_t count,
                                 bool with_scores,
                                 const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZREVRANGEBYSCORE", key, max, min};

  //!
  //!  with_scores (optional)
  //!
  if (with_scores) {
    cmd.emplace_back("WITHSCORES");
  }

  //!
  //!  limit (optional)
  //!
  if (limit) {
    cmd.emplace_back("LIMIT");
    cmd.push_back(std::to_string(offset));
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::zrevrank(const string_t &key, const string_t &member,
                         const reply_callback_t &reply_callback) {
  send({"ZREVRANK", key, member}, reply_callback);
  return *this;
}

client &client::zscan(const string_t &key, std::size_t cursor,
                      const reply_callback_t &reply_callback) {
  return zscan(key, cursor, "", 0, reply_callback);
}

client &client::zscan(const string_t &key, std::size_t cursor,
                      const string_t &pattern,
                      const reply_callback_t &reply_callback) {
  return zscan(key, cursor, pattern, 0, reply_callback);
}

client &client::zscan(const string_t &key, std::size_t cursor,
                      std::size_t count,
                      const reply_callback_t &reply_callback) {
  return zscan(key, cursor, "", count, reply_callback);
}

client &client::zscan(const string_t &key, std::size_t cursor,
                      const string_t &pattern, std::size_t count,
                      const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZSCAN", key, std::to_string(cursor)};

  if (!pattern.empty()) {
    cmd.emplace_back("MATCH");
    cmd.push_back(pattern);
  }

  if (count > 0) {
    cmd.emplace_back("COUNT");
    cmd.push_back(std::to_string(count));
  }

  send(cmd, reply_callback);
  return *this;
}

client &client::zscore(const string_t &key, const string_t &member,
                       const reply_callback_t &reply_callback) {
  send({"ZSCORE", key, member}, reply_callback);
  return *this;
}

client &client::zunionstore(const string_t &destination, std::size_t numkeys,
                            const std::vector<string_t> &keys,
                            const std::vector<std::size_t> weights,
                            aggregate_method method,
                            const reply_callback_t &reply_callback) {
  std::vector<string_t> cmd = {"ZUNIONSTORE", destination,
                               std::to_string(numkeys)};

  //!
  //!  keys
  //!
  for (const auto &key : keys) {
    cmd.push_back(key);
  }

  //!
  //!  weights (optional)
  //!
  if (!weights.empty()) {
    cmd.emplace_back("WEIGHTS");

    for (auto weight : weights) {
      cmd.push_back(std::to_string(weight));
    }
  }

  //!
  //!  aggregate method
  //!
  if (method != aggregate_method::server_default) {
    cmd.emplace_back("AGGREGATE");
    cmd.push_back(aggregate_method_to_string(method));
  }

  send(cmd, reply_callback);
  return *this;
}

//!
//!  Redis Commands
//!  std::future-based
//!

future_reply_t
client::exec_cmd(const std::function<client &(const reply_callback_t &)> &f) {
  auto prms = std::make_shared<std::promise<reply>>();

  f([prms](reply &reply) { prms->set_value(reply); });

  return prms->get_future();
}

future_reply_t client::send(const std::vector<string_t> &redis_cmd) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return send(redis_cmd, cb);
  });
}

future_reply_t client::append(const string_t &key, const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return append(key, value, cb);
  });
}

future_reply_t client::auth(const string_t &password) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return auth(password, cb);
  });
}

future_reply_t client::bgrewriteaof() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return bgrewriteaof(cb); });
}

future_reply_t client::bgsave() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return bgsave(cb); });
}

future_reply_t client::bitcount(const string_t &key) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bitcount(key, cb);
  });
}

future_reply_t client::bitcount(const string_t &key, int start, int end) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bitcount(key, start, end, cb);
  });
}

future_reply_t
client::bitfield(const string_t &key,
                 const std::vector<bitfield_operation> &operations) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bitfield(key, operations, cb);
  });
}

future_reply_t client::bitop(const string_t &operation, const string_t &destkey,
                             const std::vector<string_t> &keys) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bitop(operation, destkey, keys, cb);
  });
}

future_reply_t client::bitpos(const string_t &key, int bit) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bitpos(key, bit, cb);
  });
}

future_reply_t client::bitpos(const string_t &key, int bit, int start) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bitpos(key, bit, start, cb);
  });
}

future_reply_t client::bitpos(const string_t &key, int bit, int start,
                              int end) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bitpos(key, bit, start, end, cb);
  });
}

future_reply_t client::blpop(const std::vector<string_t> &keys, int timeout) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return blpop(keys, timeout, cb);
  });
}

future_reply_t client::brpop(const std::vector<string_t> &keys, int timeout) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return brpop(keys, timeout, cb);
  });
}

future_reply_t client::brpoplpush(const string_t &src, const string_t &dst,
                                  int timeout) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return brpoplpush(src, dst, timeout, cb);
  });
}

future_reply_t client::bzpopmin(const std::vector<string_t> &keys,
                                int timeout) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bzpopmin(keys, timeout, cb);
  });
}

future_reply_t client::bzpopmax(const std::vector<string_t> &keys,
                                int timeout) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return bzpopmax(keys, timeout, cb);
  });
}

future_reply_t client::client_id() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return client_id(cb); });
}

future_reply_t client::client_list() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return client_list(cb); });
}

future_reply_t client::client_getname() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return client_getname(cb);
  });
}

future_reply_t client::client_pause(int timeout) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return client_pause(timeout, cb);
  });
}

future_reply_t client::client_reply(const string_t &mode) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return client_reply(mode, cb);
  });
}

future_reply_t client::client_setname(const string_t &name) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return client_setname(name, cb);
  });
}

future_reply_t client::client_unblock(int id, bool witherror) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return client_unblock(id, witherror, cb);
  });
}

future_reply_t client::cluster_addslots(const std::vector<string_t> &p_slots) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_addslots(p_slots, cb);
  });
}

future_reply_t client::cluster_count_failure_reports(const string_t &node_id) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_count_failure_reports(node_id, cb);
  });
}

future_reply_t client::cluster_countkeysinslot(const string_t &slot) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_countkeysinslot(slot, cb);
  });
}

future_reply_t client::cluster_delslots(const std::vector<string_t> &p_slots) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_delslots(p_slots, cb);
  });
}

future_reply_t client::cluster_failover() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_failover(cb);
  });
}

future_reply_t client::cluster_failover(const string_t &mode) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_failover(mode, cb);
  });
}

future_reply_t client::cluster_forget(const string_t &node_id) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_forget(node_id, cb);
  });
}

future_reply_t client::cluster_getkeysinslot(const string_t &slot, int count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_getkeysinslot(slot, count, cb);
  });
}

future_reply_t client::cluster_info() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return cluster_info(cb); });
}

future_reply_t client::cluster_keyslot(const string_t &key) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_keyslot(key, cb);
  });
}

future_reply_t client::cluster_meet(const string_t &ip, int port) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_meet(ip, port, cb);
  });
}

future_reply_t client::cluster_nodes() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_nodes(cb);
  });
}

future_reply_t client::cluster_replicate(const string_t &node_id) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_replicate(node_id, cb);
  });
}

future_reply_t client::cluster_reset(const string_t &mode) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_reset(mode, cb);
  });
}

future_reply_t client::cluster_saveconfig() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_saveconfig(cb);
  });
}

future_reply_t client::cluster_set_config_epoch(const string_t &epoch) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_set_config_epoch(epoch, cb);
  });
}

future_reply_t client::cluster_setslot(const string_t &slot,
                                       const string_t &mode) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_setslot(slot, mode, cb);
  });
}

future_reply_t client::cluster_setslot(const string_t &slot,
                                       const string_t &mode,
                                       const string_t &node_id) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_setslot(slot, mode, node_id, cb);
  });
}

future_reply_t client::cluster_slaves(const string_t &node_id) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_slaves(node_id, cb);
  });
}

future_reply_t client::cluster_slots() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return cluster_slots(cb);
  });
}

future_reply_t client::command() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return command(cb); });
}

future_reply_t client::command_count() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return command_count(cb);
  });
}

future_reply_t client::command_getkeys() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return command_getkeys(cb);
  });
}

future_reply_t client::command_info(const std::vector<string_t> &command_name) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return command_info(command_name, cb);
  });
}

future_reply_t client::config_get(const string_t &param) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return config_get(param, cb);
  });
}

future_reply_t client::config_rewrite() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return config_rewrite(cb);
  });
}

future_reply_t client::config_set(const string_t &param, const string_t &val) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return config_set(param, val, cb);
  });
}

future_reply_t client::config_resetstat() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return config_resetstat(cb);
  });
}

future_reply_t client::dbsize() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return dbsize(cb); });
}

future_reply_t client::debug_object(const string_t &key) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return debug_object(key, cb);
  });
}

future_reply_t client::debug_segfault() {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return debug_segfault(cb);
  });
}

future_reply_t client::decr(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return decr(key, cb); });
}

future_reply_t client::decrby(const string_t &key, int val) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return decrby(key, val, cb);
  });
}

future_reply_t client::del(const std::vector<string_t> &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return del(key, cb); });
}

future_reply_t client::discard() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return discard(cb); });
}

future_reply_t client::dump(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return dump(key, cb); });
}

future_reply_t client::echo(const string_t &msg) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return echo(msg, cb); });
}

future_reply_t client::eval(const string_t &script, int numkeys,
                            const std::vector<string_t> &keys,
                            const std::vector<string_t> &args) {
  (void)numkeys;
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return eval(script, keys, args, cb);
  });
}

future_reply_t client::eval(const string_t &script,
                            const std::vector<string_t> &keys,
                            const std::vector<string_t> &args) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return eval(script, keys, args, cb);
  });
}

future_reply_t client::evalsha(const string_t &sha1,
                               const std::vector<string_t> &keys,
                               const std::vector<string_t> &args) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return evalsha(sha1, keys, args, cb);
  });
}

future_reply_t client::evalsha(const string_t &sha1, int numkeys,
                               const std::vector<string_t> &keys,
                               const std::vector<string_t> &args) {
  (void)numkeys;
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return evalsha(sha1, keys, args, cb);
  });
}

future_reply_t client::exec() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return exec(cb); });
}

future_reply_t client::exists(const std::vector<string_t> &keys) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return exists(keys, cb); });
}

future_reply_t client::expire(const string_t &key, int seconds) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return expire(key, seconds, cb);
  });
}

future_reply_t client::expireat(const string_t &key, int timestamp) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return expireat(key, timestamp, cb);
  });
}

future_reply_t client::flushall() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return flushall(cb); });
}

future_reply_t client::flushdb() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return flushdb(cb); });
}

future_reply_t
client::geoadd(const string_t &key,
               const std::vector<std::tuple<string_t, string_t, string_t>>
                   &long_lat_memb) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return geoadd(key, long_lat_memb, cb);
  });
}

future_reply_t client::geohash(const string_t &key,
                               const std::vector<string_t> &members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return geohash(key, members, cb);
  });
}

future_reply_t client::geopos(const string_t &key,
                              const std::vector<string_t> &members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return geopos(key, members, cb);
  });
}

future_reply_t client::geodist(const string_t &key, const string_t &member_1,
                               const string_t &member_2, const string_t &unit) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return geodist(key, member_1, member_2, unit, cb);
  });
}

future_reply_t client::georadius(const string_t &key, double longitude,
                                 double latitude, double radius, geo_unit unit,
                                 bool with_coord, bool with_dist,
                                 bool with_hash, bool asc_order,
                                 std::size_t count, const string_t &store_key,
                                 const string_t &storedist_key) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return georadius(key, longitude, latitude, radius, unit, with_coord,
                     with_dist, with_hash, asc_order, count, store_key,
                     storedist_key, cb);
  });
}

future_reply_t client::georadiusbymember(const string_t &key,
                                         const string_t &member, double radius,
                                         geo_unit unit, bool with_coord,
                                         bool with_dist, bool with_hash,
                                         bool asc_order, std::size_t count,
                                         const string_t &store_key,
                                         const string_t &storedist_key) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return georadiusbymember(key, member, radius, unit, with_coord, with_dist,
                             with_hash, asc_order, count, store_key,
                             storedist_key, cb);
  });
}

future_reply_t client::get(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return get(key, cb); });
}

future_reply_t client::getbit(const string_t &key, int offset) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return getbit(key, offset, cb);
  });
}

future_reply_t client::getrange(const string_t &key, int start, int end) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return getrange(key, start, end, cb);
  });
}

future_reply_t client::getset(const string_t &key, const string_t &val) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return getset(key, val, cb);
  });
}

future_reply_t client::hdel(const string_t &key,
                            const std::vector<string_t> &fields) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hdel(key, fields, cb);
  });
}

future_reply_t client::hexists(const string_t &key, const string_t &field) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hexists(key, field, cb);
  });
}

future_reply_t client::hget(const string_t &key, const string_t &field) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hget(key, field, cb);
  });
}

future_reply_t client::hgetall(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return hgetall(key, cb); });
}

future_reply_t client::hincrby(const string_t &key, const string_t &field,
                               int incr) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hincrby(key, field, incr, cb);
  });
}

future_reply_t client::hincrbyfloat(const string_t &key, const string_t &field,
                                    float incr) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hincrbyfloat(key, field, incr, cb);
  });
}

future_reply_t client::hkeys(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return hkeys(key, cb); });
}

future_reply_t client::hlen(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return hlen(key, cb); });
}

future_reply_t client::hmget(const string_t &key,
                             const std::vector<string_t> &fields) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hmget(key, fields, cb);
  });
}

future_reply_t
client::hmset(const string_t &key,
              const std::vector<std::pair<string_t, string_t>> &field_val) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hmset(key, field_val, cb);
  });
}

future_reply_t client::hscan(const string_t &key, std::size_t cursor) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hscan(key, cursor, cb);
  });
}

future_reply_t client::hscan(const string_t &key, std::size_t cursor,
                             const string_t &pattern) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hscan(key, cursor, pattern, cb);
  });
}

future_reply_t client::hscan(const string_t &key, std::size_t cursor,
                             std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hscan(key, cursor, count, cb);
  });
}

future_reply_t client::hscan(const string_t &key, std::size_t cursor,
                             const string_t &pattern, std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hscan(key, cursor, pattern, count, cb);
  });
}

future_reply_t client::hset(const string_t &key, const string_t &field,
                            const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hset(key, field, value, cb);
  });
}

future_reply_t client::hsetnx(const string_t &key, const string_t &field,
                              const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hsetnx(key, field, value, cb);
  });
}

future_reply_t client::hstrlen(const string_t &key, const string_t &field) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return hstrlen(key, field, cb);
  });
}

future_reply_t client::hvals(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return hvals(key, cb); });
}

future_reply_t client::incr(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return incr(key, cb); });
}

future_reply_t client::incrby(const string_t &key, int incr) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return incrby(key, incr, cb);
  });
}

future_reply_t client::incrbyfloat(const string_t &key, float incr) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return incrbyfloat(key, incr, cb);
  });
}

future_reply_t client::info(const string_t &section) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return info(section, cb);
  });
}

future_reply_t client::keys(const string_t &pattern) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return keys(pattern, cb);
  });
}

future_reply_t client::lastsave() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return lastsave(cb); });
}

future_reply_t client::lindex(const string_t &key, int index) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return lindex(key, index, cb);
  });
}

future_reply_t client::linsert(const string_t &key,
                               const string_t &before_after,
                               const string_t &pivot, const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return linsert(key, before_after, pivot, value, cb);
  });
}

future_reply_t client::llen(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return llen(key, cb); });
}

future_reply_t client::lpop(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return lpop(key, cb); });
}

future_reply_t client::lpush(const string_t &key,
                             const std::vector<string_t> &values) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return lpush(key, values, cb);
  });
}

future_reply_t client::lpushx(const string_t &key, const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return lpushx(key, value, cb);
  });
}

future_reply_t client::lrange(const string_t &key, int start, int stop) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return lrange(key, start, stop, cb);
  });
}

future_reply_t client::lrem(const string_t &key, int count,
                            const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return lrem(key, count, value, cb);
  });
}

future_reply_t client::lset(const string_t &key, int index,
                            const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return lset(key, index, value, cb);
  });
}

future_reply_t client::ltrim(const string_t &key, int start, int stop) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return ltrim(key, start, stop, cb);
  });
}

future_reply_t client::mget(const std::vector<string_t> &keys) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return mget(keys, cb); });
}

future_reply_t client::migrate(const string_t &host, int port,
                               const string_t &key, const string_t &dest_db,
                               int timeout, bool copy, bool replace,
                               const std::vector<string_t> &keys) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return migrate(host, port, key, dest_db, timeout, copy, replace, keys, cb);
  });
}

future_reply_t client::monitor() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return monitor(cb); });
}

future_reply_t client::move(const string_t &key, const string_t &db) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return move(key, db, cb);
  });
}

future_reply_t
client::mset(const std::vector<std::pair<string_t, string_t>> &key_vals) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return mset(key_vals, cb);
  });
}

future_reply_t
client::msetnx(const std::vector<std::pair<string_t, string_t>> &key_vals) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return msetnx(key_vals, cb);
  });
}

future_reply_t client::multi() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return multi(cb); });
}

future_reply_t client::object(const string_t &subcommand,
                              const std::vector<string_t> &args) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return object(subcommand, args, cb);
  });
}

future_reply_t client::persist(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return persist(key, cb); });
}

future_reply_t client::pexpire(const string_t &key, int ms) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return pexpire(key, ms, cb);
  });
}

future_reply_t client::pexpireat(const string_t &key, int ms_timestamp) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return pexpireat(key, ms_timestamp, cb);
  });
}

future_reply_t client::pfadd(const string_t &key,
                             const std::vector<string_t> &elements) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return pfadd(key, elements, cb);
  });
}

future_reply_t client::pfcount(const std::vector<string_t> &keys) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return pfcount(keys, cb);
  });
}

future_reply_t client::pfmerge(const string_t &destkey,
                               const std::vector<string_t> &sourcekeys) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return pfmerge(destkey, sourcekeys, cb);
  });
}

future_reply_t client::ping() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return ping(cb); });
}

future_reply_t client::ping(const string_t &message) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return ping(message, cb);
  });
}

future_reply_t client::psetex(const string_t &key, int64_t ms,
                              const string_t &val) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return psetex(key, ms, val, cb);
  });
}

future_reply_t client::publish(const string_t &channel,
                               const string_t &message) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return publish(channel, message, cb);
  });
}

future_reply_t client::pubsub(const string_t &subcommand,
                              const std::vector<string_t> &args) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return pubsub(subcommand, args, cb);
  });
}

future_reply_t client::pttl(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return pttl(key, cb); });
}

future_reply_t client::quit() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return quit(cb); });
}

future_reply_t client::randomkey() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return randomkey(cb); });
}

future_reply_t client::readonly() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return readonly(cb); });
}

future_reply_t client::readwrite() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return readwrite(cb); });
}

future_reply_t client::rename(const string_t &key, const string_t &newkey) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return rename(key, newkey, cb);
  });
}

future_reply_t client::renamenx(const string_t &key, const string_t &newkey) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return renamenx(key, newkey, cb);
  });
}

future_reply_t client::restore(const string_t &key, int ttl,
                               const string_t &serialized_value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return restore(key, ttl, serialized_value, cb);
  });
}

future_reply_t client::restore(const string_t &key, int ttl,
                               const string_t &serialized_value,
                               const string_t &replace) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return restore(key, ttl, serialized_value, replace, cb);
  });
}

future_reply_t client::role() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return role(cb); });
}

future_reply_t client::rpop(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return rpop(key, cb); });
}

future_reply_t client::rpoplpush(const string_t &src, const string_t &dst) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return rpoplpush(src, dst, cb);
  });
}

future_reply_t client::rpush(const string_t &key,
                             const std::vector<string_t> &values) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return rpush(key, values, cb);
  });
}

future_reply_t client::rpushx(const string_t &key, const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return rpushx(key, value, cb);
  });
}

future_reply_t client::sadd(const string_t &key,
                            const std::vector<string_t> &members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sadd(key, members, cb);
  });
}

future_reply_t client::scan(std::size_t cursor) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return scan(cursor, cb); });
}

future_reply_t client::scan(std::size_t cursor, const string_t &pattern) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return scan(cursor, pattern, cb);
  });
}

future_reply_t client::scan(std::size_t cursor, std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return scan(cursor, count, cb);
  });
}

future_reply_t client::scan(std::size_t cursor, const string_t &pattern,
                            std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return scan(cursor, pattern, count, cb);
  });
}

future_reply_t client::save() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return save(cb); });
}

future_reply_t client::scard(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return scard(key, cb); });
}

future_reply_t client::script_debug(const string_t &mode) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return script_debug(mode, cb);
  });
}

future_reply_t client::script_exists(const std::vector<string_t> &scripts) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return script_exists(scripts, cb);
  });
}

future_reply_t client::script_flush() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return script_flush(cb); });
}

future_reply_t client::script_kill() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return script_kill(cb); });
}

future_reply_t client::script_load(const string_t &script) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return script_load(script, cb);
  });
}

future_reply_t client::sdiff(const std::vector<string_t> &keys) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return sdiff(keys, cb); });
}

future_reply_t client::sdiffstore(const string_t &dst,
                                  const std::vector<string_t> &keys) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sdiffstore(dst, keys, cb);
  });
}

future_reply_t client::select(int index) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return select(index, cb);
  });
}

future_reply_t client::set(const string_t &key, const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return set(key, value, cb);
  });
}

future_reply_t client::set_advanced(const string_t &key, const string_t &value,
                                    bool ex, int ex_sec, bool px, int px_milli,
                                    bool nx, bool xx) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return set_advanced(key, value, ex, ex_sec, px, px_milli, nx, xx, cb);
  });
}

future_reply_t client::setbit(const string_t &key, int offset,
                              const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return setbit(key, offset, value, cb);
  });
}

future_reply_t client::setex(const string_t &key, int64_t seconds,
                             const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return setex(key, seconds, value, cb);
  });
}

future_reply_t client::setnx(const string_t &key, const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return setnx(key, value, cb);
  });
}

future_reply_t client::set_range(const string_t &key, int offset,
                                 const string_t &value) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return set_range(key, offset, value, cb);
  });
}

future_reply_t client::shutdown() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return shutdown(cb); });
}

future_reply_t client::shutdown(const string_t &save) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return shutdown(save, cb);
  });
}

future_reply_t client::sinter(const std::vector<string_t> &keys) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return sinter(keys, cb); });
}

future_reply_t client::sinterstore(const string_t &dst,
                                   const std::vector<string_t> &keys) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sinterstore(dst, keys, cb);
  });
}

future_reply_t client::sismember(const string_t &key, const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sismember(key, member, cb);
  });
}

future_reply_t client::slaveof(const string_t &host, int port) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return slaveof(host, port, cb);
  });
}

future_reply_t client::replicaof(const string_t &host, int port) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return replicaof(host, port, cb);
  });
}

future_reply_t client::slowlog(const string_t &subcommand) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return slowlog(subcommand, cb);
  });
}

future_reply_t client::slowlog(const string_t &sub_cmd,
                               const string_t &argument) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return slowlog(sub_cmd, argument, cb);
  });
}

future_reply_t client::smembers(const string_t &key) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return smembers(key, cb);
  });
}

future_reply_t client::smove(const string_t &src, const string_t &dst,
                             const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return smove(src, dst, member, cb);
  });
}

future_reply_t client::sort(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return sort(key, cb); });
}

future_reply_t client::sort(const string_t &key,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, get_patterns, asc_order, alpha, cb);
  });
}

future_reply_t client::sort(const string_t &key, std::size_t offset,
                            std::size_t count,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, offset, count, get_patterns, asc_order, alpha, cb);
  });
}

future_reply_t client::sort(const string_t &key, const string_t &by_pattern,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, by_pattern, get_patterns, asc_order, alpha, cb);
  });
}

future_reply_t client::sort(const string_t &key,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha,
                            const string_t &store_dest) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, get_patterns, asc_order, alpha, store_dest, cb);
  });
}

future_reply_t client::sort(const string_t &key, std::size_t offset,
                            std::size_t count,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha,
                            const string_t &store_dest) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, offset, count, get_patterns, asc_order, alpha, store_dest,
                cb);
  });
}

future_reply_t client::sort(const string_t &key, const string_t &by_pattern,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha,
                            const string_t &store_dest) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, by_pattern, get_patterns, asc_order, alpha, store_dest,
                cb);
  });
}

future_reply_t client::sort(const string_t &key, const string_t &by_pattern,
                            std::size_t offset, std::size_t count,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, by_pattern, offset, count, get_patterns, asc_order, alpha,
                cb);
  });
}

future_reply_t client::sort(const string_t &key, const string_t &by_pattern,
                            std::size_t offset, std::size_t count,
                            const std::vector<string_t> &get_patterns,
                            bool asc_order, bool alpha,
                            const string_t &store_dest) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sort(key, by_pattern, offset, count, get_patterns, asc_order, alpha,
                store_dest, cb);
  });
}

future_reply_t client::spop(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return spop(key, cb); });
}

future_reply_t client::spop(const string_t &key, int count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return spop(key, count, cb);
  });
}

future_reply_t client::srandmember(const string_t &key) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return srandmember(key, cb);
  });
}

future_reply_t client::srandmember(const string_t &key, int count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return srandmember(key, count, cb);
  });
}

future_reply_t client::srem(const string_t &key,
                            const std::vector<string_t> &members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return srem(key, members, cb);
  });
}

future_reply_t client::sscan(const string_t &key, std::size_t cursor) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sscan(key, cursor, cb);
  });
}

future_reply_t client::sscan(const string_t &key, std::size_t cursor,
                             const string_t &pattern) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sscan(key, cursor, pattern, cb);
  });
}

future_reply_t client::sscan(const string_t &key, std::size_t cursor,
                             std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sscan(key, cursor, count, cb);
  });
}

future_reply_t client::sscan(const string_t &key, std::size_t cursor,
                             const string_t &pattern, std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sscan(key, cursor, pattern, count, cb);
  });
}

future_reply_t client::strlen(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return strlen(key, cb); });
}

future_reply_t client::sunion(const std::vector<string_t> &keys) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return sunion(keys, cb); });
}

future_reply_t client::sunionstore(const string_t &dst,
                                   const std::vector<string_t> &keys) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return sunionstore(dst, keys, cb);
  });
}

future_reply_t client::sync() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return sync(cb); });
}

future_reply_t client::time() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return time(cb); });
}

future_reply_t client::ttl(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return ttl(key, cb); });
}

future_reply_t client::type(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return type(key, cb); });
}

future_reply_t client::unwatch() {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return unwatch(cb); });
}

future_reply_t client::wait(int num_replicas, int timeout) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return wait(num_replicas, timeout, cb);
  });
}

future_reply_t client::watch(const std::vector<string_t> &keys) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return watch(keys, cb); });
}

future_reply_t client::xack(const string_t &key, const string_t &group,
                            const std::vector<string_t> &id_members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xack(key, group, id_members, cb);
  });
}

future_reply_t
client::xadd(const string_t &key, const string_t &id,
             const std::multimap<string_t, string_t> &field_members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xadd(key, id, field_members, cb);
  });
}

future_reply_t client::xclaim(const string_t &key, const string_t &group,
                              const string_t &consumer,
                              const int &min_idle_time,
                              const std::vector<string_t> &id_members,
                              const xclaim_options_t &options) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xclaim(key, group, consumer, min_idle_time, id_members, options, cb);
  });
}

future_reply_t client::xdel(const string_t &key,
                            const std::vector<string_t> &id_members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xdel(key, id_members, cb);
  });
}

future_reply_t client::xgroup_create(const string_t &key,
                                     const string_t &group_name,
                                     const string_t &id) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xgroup_create(key, group_name, id, cb);
  });
}

future_reply_t client::xgroup_set_id(const string_t &key,
                                     const string_t &group_name,
                                     const string_t &id) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xgroup_set_id(key, group_name, id, cb);
  });
}

future_reply_t client::xgroup_destroy(const string_t &key,
                                      const string_t &group_name) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xgroup_destroy(key, group_name, cb);
  });
}

future_reply_t client::xgroup_del_consumer(const string_t &key,
                                           const string_t &group_name,
                                           const string_t &consumer_name) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xgroup_del_consumer(key, group_name, consumer_name, cb);
  });
}

//!  @htmlinclude https://redis.io/commands/xdel
//!  @brief introspection command used in order to retrieve different
//!  information about the consumer groups
//!  @param key
//!  @param group_name stream consumer group name
//!  @return
//!
future_reply_t client::xinfo_consumers(const string_t &key,
                                       const string_t &group_name) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xinfo_consumers(key, group_name, cb);
  });
}

future_reply_t client::xinfo_groups(const string_t &stream) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xinfo_groups(stream, cb);
  });
}

future_reply_t client::xinfo_stream(const string_t &stream) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xinfo_stream(stream, cb);
  });
}

future_reply_t client::xlen(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return xlen(key, cb); });
}

future_reply_t client::xpending(const string_t &stream, const string_t &group,
                                const xpending_options_t &options) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xpending(stream, group, options, cb);
  });
}

future_reply_t client::xrange(const string_t &stream,
                              const range_options_t &range_args) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xrange(stream, range_args, cb);
  });
}

future_reply_t client::xreadgroup(const xreadgroup_options_t &a) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xreadgroup(a, cb);
  });
}

future_reply_t client::xread(const xread_options_t &a) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return xread(a, cb); });
}

future_reply_t client::xrevrange(const string_t &key,
                                 const range_options_t &range_args) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xrevrange(key, range_args, cb);
  });
}

future_reply_t client::xtrim(const string_t &key, int max_len) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xtrim(key, max_len, cb);
  });
}
future_reply_t client::xtrim_approx(const string_t &key, int max_len) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return xtrim_approx(key, max_len, cb);
  });
}

future_reply_t
client::zadd(const string_t &key, const std::vector<string_t> &options,
             const std::multimap<string_t, string_t> &score_members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zadd(key, options, score_members, cb);
  });
}

future_reply_t client::zcard(const string_t &key) {
  return exec_cmd(
      [=](const reply_callback_t &cb) -> client & { return zcard(key, cb); });
}

future_reply_t client::zcount(const string_t &key, int min, int max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zcount(key, min, max, cb);
  });
}

future_reply_t client::zcount(const string_t &key, double min, double max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zcount(key, min, max, cb);
  });
}

future_reply_t client::zcount(const string_t &key, const string_t &min,
                              const string_t &max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zcount(key, min, max, cb);
  });
}

future_reply_t client::zincrby(const string_t &key, int incr,
                               const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zincrby(key, incr, member, cb);
  });
}

future_reply_t client::zincrby(const string_t &key, double incr,
                               const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zincrby(key, incr, member, cb);
  });
}

future_reply_t client::zincrby(const string_t &key, const string_t &incr,
                               const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zincrby(key, incr, member, cb);
  });
}

future_reply_t client::zinterstore(const string_t &destination,
                                   std::size_t numkeys,
                                   const std::vector<string_t> &keys,
                                   const std::vector<std::size_t> weights,
                                   aggregate_method method) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zinterstore(destination, numkeys, keys, weights, method, cb);
  });
}

future_reply_t client::zlexcount(const string_t &key, int min, int max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zlexcount(key, min, max, cb);
  });
}

future_reply_t client::zlexcount(const string_t &key, double min, double max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zlexcount(key, min, max, cb);
  });
}

future_reply_t client::zlexcount(const string_t &key, const string_t &min,
                                 const string_t &max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zlexcount(key, min, max, cb);
  });
}

future_reply_t client::zpopmin(const string_t &key, int count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zpopmin(key, count, cb);
  });
}
future_reply_t client::zpopmax(const string_t &key, int count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zpopmax(key, count, cb);
  });
}

future_reply_t client::zrange(const string_t &key, int start, int stop,
                              bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrange(key, start, stop, with_scores, cb);
  });
}

future_reply_t client::zrange(const string_t &key, double start, double stop,
                              bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrange(key, start, stop, with_scores, cb);
  });
}

future_reply_t client::zrange(const string_t &key, const string_t &start,
                              const string_t &stop, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrange(key, start, stop, with_scores, cb);
  });
}

future_reply_t client::zrangebylex(const string_t &key, int min, int max,
                                   bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebylex(key, min, max, with_scores, cb);
  });
}

future_reply_t client::zrangebylex(const string_t &key, double min, double max,
                                   bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebylex(key, min, max, with_scores, cb);
  });
}

future_reply_t client::zrangebylex(const string_t &key, const string_t &min,
                                   const string_t &max, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebylex(key, min, max, with_scores, cb);
  });
}

future_reply_t client::zrangebylex(const string_t &key, int min, int max,
                                   std::size_t offset, std::size_t count,
                                   bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebylex(key, min, max, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrangebylex(const string_t &key, double min, double max,
                                   std::size_t offset, std::size_t count,
                                   bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebylex(key, min, max, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrangebylex(const string_t &key, const string_t &min,
                                   const string_t &max, std::size_t offset,
                                   std::size_t count, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebylex(key, min, max, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrangebyscore(const string_t &key, int min, int max,
                                     bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebyscore(key, min, max, with_scores, cb);
  });
}

future_reply_t client::zrangebyscore(const string_t &key, double min,
                                     double max, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebyscore(key, min, max, with_scores, cb);
  });
}

future_reply_t client::zrangebyscore(const string_t &key, const string_t &min,
                                     const string_t &max, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebyscore(key, min, max, with_scores, cb);
  });
}

future_reply_t client::zrangebyscore(const string_t &key, int min, int max,
                                     std::size_t offset, std::size_t count,
                                     bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebyscore(key, min, max, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrangebyscore(const string_t &key, double min,
                                     double max, std::size_t offset,
                                     std::size_t count, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebyscore(key, min, max, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrangebyscore(const string_t &key, const string_t &min,
                                     const string_t &max, std::size_t offset,
                                     std::size_t count, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrangebyscore(key, min, max, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrank(const string_t &key, const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrank(key, member, cb);
  });
}

future_reply_t client::zrem(const string_t &key,
                            const std::vector<string_t> &members) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrem(key, members, cb);
  });
}

future_reply_t client::zremrangebylex(const string_t &key, int min, int max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebylex(key, min, max, cb);
  });
}

future_reply_t client::zremrangebylex(const string_t &key, double min,
                                      double max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebylex(key, min, max, cb);
  });
}

future_reply_t client::zremrangebylex(const string_t &key, const string_t &min,
                                      const string_t &max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebylex(key, min, max, cb);
  });
}

future_reply_t client::zremrangebyrank(const string_t &key, int start,
                                       int stop) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebyrank(key, start, stop, cb);
  });
}

future_reply_t client::zremrangebyrank(const string_t &key, double start,
                                       double stop) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebyrank(key, start, stop, cb);
  });
}

future_reply_t client::zremrangebyrank(const string_t &key,
                                       const string_t &start,
                                       const string_t &stop) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebyrank(key, start, stop, cb);
  });
}

future_reply_t client::zremrangebyscore(const string_t &key, int min, int max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebyscore(key, min, max, cb);
  });
}

future_reply_t client::zremrangebyscore(const string_t &key, double min,
                                        double max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebyscore(key, min, max, cb);
  });
}

future_reply_t client::zremrangebyscore(const string_t &key,
                                        const string_t &min,
                                        const string_t &max) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zremrangebyscore(key, min, max, cb);
  });
}

future_reply_t client::zrevrange(const string_t &key, int start, int stop,
                                 bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrange(key, start, stop, with_scores, cb);
  });
}

future_reply_t client::zrevrange(const string_t &key, double start, double stop,
                                 bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrange(key, start, stop, with_scores, cb);
  });
}

future_reply_t client::zrevrange(const string_t &key, const string_t &start,
                                 const string_t &stop, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrange(key, start, stop, with_scores, cb);
  });
}

future_reply_t client::zrevrangebylex(const string_t &key, int max, int min,
                                      bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebylex(key, max, min, with_scores, cb);
  });
}

future_reply_t client::zrevrangebylex(const string_t &key, double max,
                                      double min, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebylex(key, max, min, with_scores, cb);
  });
}

future_reply_t client::zrevrangebylex(const string_t &key, const string_t &max,
                                      const string_t &min, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebylex(key, max, min, with_scores, cb);
  });
}

future_reply_t client::zrevrangebylex(const string_t &key, int max, int min,
                                      std::size_t offset, std::size_t count,
                                      bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebylex(key, max, min, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrevrangebylex(const string_t &key, double max,
                                      double min, std::size_t offset,
                                      std::size_t count, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebylex(key, max, min, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrevrangebylex(const string_t &key, const string_t &max,
                                      const string_t &min, std::size_t offset,
                                      std::size_t count, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebylex(key, max, min, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrevrangebyscore(const string_t &key, int max, int min,
                                        bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebyscore(key, max, min, with_scores, cb);
  });
}

future_reply_t client::zrevrangebyscore(const string_t &key, double max,
                                        double min, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebyscore(key, max, min, with_scores, cb);
  });
}

future_reply_t client::zrevrangebyscore(const string_t &key,
                                        const string_t &max,
                                        const string_t &min, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebyscore(key, max, min, with_scores, cb);
  });
}

future_reply_t client::zrevrangebyscore(const string_t &key, int max, int min,
                                        std::size_t offset, std::size_t count,
                                        bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebyscore(key, max, min, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrevrangebyscore(const string_t &key, double max,
                                        double min, std::size_t offset,
                                        std::size_t count, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebyscore(key, max, min, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrevrangebyscore(const string_t &key,
                                        const string_t &max,
                                        const string_t &min, std::size_t offset,
                                        std::size_t count, bool with_scores) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrangebyscore(key, max, min, offset, count, with_scores, cb);
  });
}

future_reply_t client::zrevrank(const string_t &key, const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zrevrank(key, member, cb);
  });
}

future_reply_t client::zscan(const string_t &key, std::size_t cursor) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zscan(key, cursor, cb);
  });
}

future_reply_t client::zscan(const string_t &key, std::size_t cursor,
                             const string_t &pattern) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zscan(key, cursor, pattern, cb);
  });
}

future_reply_t client::zscan(const string_t &key, std::size_t cursor,
                             std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zscan(key, cursor, count, cb);
  });
}

future_reply_t client::zscan(const string_t &key, std::size_t cursor,
                             const string_t &pattern, std::size_t count) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zscan(key, cursor, pattern, count, cb);
  });
}

future_reply_t client::zscore(const string_t &key, const string_t &member) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zscore(key, member, cb);
  });
}

future_reply_t client::zunionstore(const string_t &destination,
                                   std::size_t numkeys,
                                   const std::vector<string_t> &keys,
                                   const std::vector<std::size_t> weights,
                                   aggregate_method method) {
  return exec_cmd([=](const reply_callback_t &cb) -> client & {
    return zunionstore(destination, numkeys, keys, weights, method, cb);
  });
}

} // namespace cpp_redis
