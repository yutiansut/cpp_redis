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

#ifndef CPP_REDIS_CORE_CLIENT_HPP_
#define CPP_REDIS_CORE_CLIENT_HPP_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <cpp_redis/core/reply.hpp>
#include <cpp_redis/core/sentinel.hpp>
#include <cpp_redis/helpers/variadic_template.hpp>
#include <cpp_redis/misc/logger.hpp>
#include <cpp_redis/network/redis_connection.hpp>
#include <cpp_redis/network/tcp_client_iface.hpp>
#include <cpp_redis/types/streams_types.hpp>

#include <cpp_redis/misc/deprecated.hpp>

#include <cpp_redis/impl/reply.ipp>

#define __METER "m"

#define __CPP_REDIS_DEFAULT_HOST "127.0.0.1"
#define __CPP_REDIS_DEFAULT_PORT 6379

namespace cpp_redis {

struct client_list_reply {
  string_t id;
  string_t addr;
  string_t name;
};
using client_list_reply_t = client_list_reply;

class client_list_payload
    : public virtual reply_payload_iface<client_list_reply_t> {
public:
  explicit client_list_payload(reply_t repl)
      : reply_payload_iface(std::move(repl)) {}

  client_list_reply_t get_payload() override {
    client_list_reply_t resp;
    if (m_reply.is_bulk_string()) {
      string_t repl_str = m_reply.as_string();
      auto sep = repl_str.find(' ');
      resp.id = repl_str.substr(0, sep);
      return resp;
    }
    throw string_t("Bad reply");
  }
};

using client_list_payload_t = client_list_payload;

//!
//!  reply callback called whenever a reply is received
//!  takes as parameter the received reply
//!
using reply_callback_t = function<void(reply_t &)>;

using client_list_reply_callback_t =
    function<void(const client_list_payload_t &)>;

using future_reply_t = future<reply_t>;

//!
//!  client is the class providing communication with a Redis server.
//!  It is meant to be used for sending commands to the remote server and
//!  receiving its replies. The client support asynchronous requests, as well as
//!  synchronous ones. Moreover, commands pipelining is supported.
//!
class client {
public:
  //!
  //!  client type
  //!  used for client kill
  //!
  enum class client_type { normal, master, pubsub, slave, cluster };

public:
#ifndef __CPP_REDIS_USE_CUSTOM_TCP_CLIENT

  //!
  //!  ctor
  //!
  client();

#endif // __CPP_REDIS_USE_CUSTOM_TCP_CLIENT //!

  //!
  //!  custom ctor to specify custom tcp_client
  //!
  //!  @param tcp_client tcp client to be used for network communications
  //!
  explicit client(const shared_ptr<tcp_client_iface_t> &tcp_client);

  //!
  //!  dtor
  //!
  ~client();

  //!
  //!  copy ctor
  //!
  client(const client &) = delete;

  //!
  //!  assignment operator
  //!
  client &operator=(const client &) = delete;

public:
  //!
  //!  Connect to redis server
  //!
  //!  @param host host to be connected to
  //!  @param port port to be connected to
  //!  @param connect_callback connect handler to be called on connect events
  //!  (may be null)
  //!  @param timeout_ms maximum time to connect
  //!  @param max_reconnects maximum attempts of reconnection if connection
  //!  dropped
  //!  @param reconnect_interval_ms time between two attempts of reconnection
  //!
  void connect(const string_t &host = "127.0.0.1", size_t port = 6379,
               const connect_callback_t &connect_callback = nullptr,
               uint_t timeout_ms = 0, int_t max_reconnects = 0,
               uint_t reconnect_interval_ms = 0);

  //!
  //!  Connect to redis server
  //!
  //!  @param name sentinel name
  //!  @param connect_callback connect handler to be called on connect events
  //!  (may be null)
  //!  @param timeout_ms maximum time to connect
  //!  @param max_reconnects maximum attempts of reconnection if connection
  //!  dropped
  //!  @param reconnect_interval_ms time between two attempts of reconnection
  //!
  void connect(const string_t &name,
               const connect_callback_t &connect_callback = nullptr,
               uint_t timeout_ms = 0, int_t max_reconnects = 0,
               uint_t reconnect_interval_ms = 0);

  //!
  //!  @return whether we are connected to the redis server
  //!
  bool is_connected() const;

  //!
  //!  disconnect from redis server
  //!
  //!  @param wait_for_removal when sets to true, disconnect blocks until the
  //!  underlying TCP client has been effectively removed from the io_service
  //!  and that all the underlying callbacks have completed.
  //!
  void disconnect(bool wait_for_removal = false);

  //!
  //!  @return whether an attempt to reconnect is in progress
  //!
  bool is_reconnecting() const;

  //!
  //!  stop any reconnect in progress
  //!
  void cancel_reconnect();

public:
  //!
  //!  send the given command
  //!  the command is actually pipelined and only buffered, so nothing is sent
  //!  to the network please call commit() / sync_commit() to flush the buffer
  //!
  //!  @param redis_cmd command to be sent
  //!  @param callback callback to be called on received reply
  //!  @return current instance
  //!
  client &send(const vector<string_t> &redis_cmd,
               const reply_callback_t &callback);

  //!
  //!  same as the other send method
  //!  but future based: does not take any callback and return an std:;future to
  //!  handle the reply
  //!
  //!  @param redis_cmd command to be sent
  //!  @return future to handler redis reply
  //!
  future_reply_t send(const vector<string_t> &redis_cmd);

  //!
  //!  Sends all the commands that have been stored by calling send() since the
  //!  last commit() call to the redis server. That is, pipelining is supported
  //!  in a very simple and efficient way:
  //!  client.send(...).send(...).send(...).commit() will send the 3 commands at
  //!  once (instead of sending 3 network requests, one for each command, as it
  //!  would have been done without pipelining). Pipelined commands are always
  //!  removed from the buffer, even in the case of an error (for example,
  //!  calling commit while the client is not connected, something that throws
  //!  an exception). commit() works asynchronously: it returns immediately
  //!  after sending the queued requests and replies are processed
  //!  asynchronously.
  //!
  //!  Please note that, while commit() can safely be called from inside a reply
  //!  callback, calling sync_commit() from inside a reply callback is not
  //!  permitted and will lead to undefined behavior, mostly deadlock.
  //!
  client &commit();

  //!
  //!  same as commit(), but synchronous
  //!  will block until all pending commands have been sent and that a reply has
  //!  been received for each of them and all underlying callbacks completed
  //!
  //!  @return current instance
  //!
  client &sync_commit();

  //!
  //!  same as sync_commit, but with a timeout
  //!  will simply block until it completes or timeout expires
  //!
  //!  @return current instance
  //!
  template <class Rep, class Period>
  client &sync_commit(const duration<Rep, Period> &timeout) {
    //!
    //!  no need to call commit in case of reconnection
    //!  the reconnection flow will do it for us
    //!
    if (!is_reconnecting()) {
      try_commit();
    }

    unique_lock<mutex_t> lock_callback(m_callbacks_mutex);
    __CPP_REDIS_LOG(debug, "client waiting for callbacks to complete");
    if (!m_sync_cond_var.wait_for(lock_callback, timeout, [=] {
          return m_callbacks_running == 0 && m_commands.empty();
        })) {
      __CPP_REDIS_LOG(debug, "client finished waiting for callback");
    } else {
      __CPP_REDIS_LOG(debug, "client timed out waiting for callback");
    }

    return *this;
  }

private:
  //!
  //!  @return whether a reconnection attempt should be performed
  //!
  bool should_reconnect() const;

  //!
  //!  resend all pending commands that failed to be sent due to disconnection
  //!
  void resend_failed_commands();

  //!
  //!  sleep between two reconnect attempts if necessary
  //!
  void sleep_before_next_reconnect_attempt();

  //!
  //!  reconnect to the previously connected host
  //!  automatically re authenticate and resubscribe to subscribed channel in
  //!  case of success
  //!
  void reconnect();

  //!
  //!  re authenticate to redis server based on previously used password
  //!
  void re_auth();

  //!
  //!  re select db to redis server based on previously selected db
  //!
  void re_select();

protected:
  //!
  //!  unprotected send
  //!  same as send, but without any mutex lock
  //!
  //!  @param redis_cmd cmd to be sent
  //!  @param callback callback to be called whenever a reply is received
  //!
  virtual void unprotected_send(const vector<string_t> &redis_cmd,
                        const reply_callback_t &callback);

  //!
  //!  unprotected auth
  //!  same as auth, but without any mutex lock
  //!
  //!  @param password password to be used for authentication
  //!  @param reply_callback callback to be called whenever a reply is received
  //!
  void unprotected_auth(const string_t &password,
                        const reply_callback_t &reply_callback);

  //!
  //!  unprotected select
  //!  same as select, but without any mutex lock
  //!
  //!  @param index index to be used for db select
  //!  @param reply_callback callback to be called whenever a reply is received
  //!
  void unprotected_select(int index, const reply_callback_t &reply_callback);

  //!
  //!  redis_connection disconnection handler, triggered whenever a
  //!  disconnection occurred
  //!
  //!  @param connection redis_connection instance
  //!
  void connection_disconnection_handler(redis_connection_t &connection);
public:
  //!
  //!  add a sentinel definition. Required for connect() or
  //!  get_master_addr_by_name() when autoconnect is enabled.
  //!
  //!  @param host sentinel host
  //!  @param port sentinel port
  //!  @param timeout_ms maximum time to connect
  //!
  void add_sentinel(const string_t &host, size_t port, uint_t timeout_ms = 0);

  //!
  //!  retrieve sentinel for current client
  //!
  //!  @return sentinel associated to current client
  //!
  const sentinel &get_sentinel() const;

  //!
  //!  retrieve sentinel for current client
  //!  non-const version
  //!
  //!  @return sentinel associated to current client
  //!
  sentinel &get_sentinel();

  //!
  //!  clear all existing sentinels.
  //!
  void clear_sentinels();

public:
  //!
  //!  aggregate method to be used for some commands (like zunionstore)
  //!  these match the aggregate methods supported by redis
  //!  use server_default if you are not willing to specify this parameter and
  //!  let the server defaults
  //!
  enum class aggregate_method { sum, min, max, server_default };

  //!
  //!  convert an aggregate_method enum to its equivalent redis-server string
  //!
  //!  @param method aggregate_method to convert
  //!  @return conversion
  //!
  string_t aggregate_method_to_string(aggregate_method method) const;

public:
  //!
  //!  geographic unit to be used for some commands (like georadius)
  //!  these match the geo units supported by redis-server
  //!
  enum class geo_unit { m, km, ft, mi };

  //!
  //!  convert a geo unit to its equivalent redis-server string
  //!
  //!  @param unit geo_unit to convert
  //!  @return conversion
  //!
  string_t geo_unit_to_string(geo_unit unit) const;

public:
  //!
  //!  overflow type to be used for some commands (like bitfield)
  //!  these match the overflow types supported by redis-server
  //!  use server_default if you are not willing to specify this parameter and
  //!  let the server defaults
  //!
  enum class overflow_type { wrap, sat, fail, server_default };

  using overflow_type_t = overflow_type;

  //!
  //!  convert an overflow type to its equivalent redis-server string
  //!
  //!  @param type overflow type to convert
  //!  @return conversion
  //!
  string_t overflow_type_to_string(overflow_type_t type) const;

public:
  //!
  //!  bitfield operation type to be used for some commands (like bitfield)
  //!  these match the bitfield operation types supported by redis-server
  //!
  enum class bitfield_operation_type { get, set, incrby };

  using bitfield_operation_type_t = bitfield_operation_type;

  //!
  //!  convert a bitfield operation type to its equivalent redis-server string
  //!
  //!  @param operation operation type to convert
  //!  @return conversion
  //!
  string_t
  bitfield_operation_type_to_string(bitfield_operation_type operation) const;

public:
  //!
  //!  used to store a get, set or incrby bitfield operation (for bitfield
  //!  command)
  //!
  struct bitfield_operation {
    //!
    //!  operation type (get, set, incrby)
    //!
    bitfield_operation_type operation_type;
    using bitfield_operation_type_t = bitfield_operation_type;

    //!
    //!  redis type parameter for get, set or incrby operations
    //!
    string_t type;

    //!
    //!  redis offset parameter for get, set or incrby operations
    //!
    int offset;

    //!
    //!  redis value parameter for set operation, or increment parameter for
    //!  incrby operation
    //!
    int value;

    //!
    //!  overflow optional specification
    //!
    overflow_type_t overflow;

    //!
    //!  build a bitfield_operation for a bitfield get operation
    //!
    //!  @param type type param of a get operation
    //!  @param offset offset param of a get operation
    //!  @param overflow overflow specification (leave to server_default if you
    //!  do not want to specify it)
    //!  @return corresponding get bitfield_operation
    //!
    static bitfield_operation
    get(const string_t &type, int offset,
        overflow_type_t overflow = overflow_type::server_default);

    //!
    //!  build a bitfield_operation for a bitfield set operation
    //!
    //!  @param type type param of a set operation
    //!  @param offset offset param of a set operation
    //!  @param value value param of a set operation
    //!  @param overflow overflow specification (leave to server_default if you
    //!  do not want to specify it)
    //!  @return corresponding set bitfield_operation
    //!
    static bitfield_operation
    set(const string_t &type, int offset, int value,
        overflow_type_t overflow = overflow_type::server_default);

    //!
    //!  build a bitfield_operation for a bitfield incrby operation
    //!
    //!  @param type type param of a incrby operation
    //!  @param offset offset param of a incrby operation
    //!  @param increment increment param of a incrby operation
    //!  @param overflow overflow specification (leave to server_default if you
    //!  do not want to specify it)
    //!  @return corresponding incrby bitfield_operation
    //!
    static bitfield_operation
    incrby(const string_t &type, int offset, int increment,
           overflow_type_t overflow = overflow_type::server_default);
  };

public:
  client &append(const string_t &key, const string_t &value,
                 const reply_callback_t &reply_callback);

  future_reply_t append(const string_t &key, const string_t &value);

  client &auth(const string_t &password,
               const reply_callback_t &reply_callback);

  future_reply_t auth(const string_t &password);

  client &bgrewriteaof(const reply_callback_t &reply_callback);

  future_reply_t bgrewriteaof();

  client &bgsave(const reply_callback_t &reply_callback);

  future_reply_t bgsave();

  client &bitcount(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t bitcount(const string_t &key);

  client &bitcount(const string_t &key, int start, int end,
                   const reply_callback_t &reply_callback);

  future_reply_t bitcount(const string_t &key, int start, int end);

  client &bitfield(const string_t &key,
                   const vector<bitfield_operation> &operations,
                   const reply_callback_t &reply_callback);

  future_reply_t bitfield(const string_t &key,
                          const vector<bitfield_operation> &operations);

  client &bitop(const string_t &operation, const string_t &destkey,
                const vector<string_t> &keys,
                const reply_callback_t &reply_callback);

  future_reply_t bitop(const string_t &operation, const string_t &destkey,
                       const vector<string_t> &keys);

  client &bitpos(const string_t &key, int bit,
                 const reply_callback_t &reply_callback);

  future_reply_t bitpos(const string_t &key, int bit);

  client &bitpos(const string_t &key, int bit, int start,
                 const reply_callback_t &reply_callback);

  future_reply_t bitpos(const string_t &key, int bit, int start);

  client &bitpos(const string_t &key, int bit, int start, int end,
                 const reply_callback_t &reply_callback);

  future_reply_t bitpos(const string_t &key, int bit, int start, int end);

  client &blpop(const vector<string_t> &keys, int timeout,
                const reply_callback_t &reply_callback);

  future_reply_t blpop(const vector<string_t> &keys, int timeout);

  client &brpop(const vector<string_t> &keys, int timeout,
                const reply_callback_t &reply_callback);

  future_reply_t brpop(const vector<string_t> &keys, int timeout);

  client &brpoplpush(const string_t &src, const string_t &dst, int timeout,
                     const reply_callback_t &reply_callback);

  future_reply_t brpoplpush(const string_t &src, const string_t &dst,
                            int timeout);

  client &bzpopmin(const vector<string_t> &keys, int timeout,
                   const reply_callback_t &reply_callback);

  future_reply_t bzpopmin(const vector<string_t> &keys, int timeout);

  client &bzpopmax(const vector<string_t> &keys, int timeout,
                   const reply_callback_t &reply_callback);

  future_reply_t bzpopmax(const vector<string_t> &keys, int timeout);

  client &client_id(const reply_callback_t &reply_callback);

  future_reply_t client_id();

  //<editor-fold desc="client">
  template <typename T, typename... Ts>
  client &client_kill(const string_t &host, int port, const T &arg,
                      const Ts &... args);

  client &client_kill(const string_t &host, int port);

  template <typename... Ts>
  client &client_kill(const char *host, int port, const Ts &... args);

  template <typename T, typename... Ts>
  client &client_kill(const T &, const Ts &...);

  template <typename T, typename... Ts>
  future_reply_t client_kill_future(T, const Ts...);

  client &client_list(const reply_callback_t &reply_callback);

  client &client_list_test(const client_list_reply_callback_t &reply_callback);

  future_reply_t client_list();

  client &client_getname(const reply_callback_t &reply_callback);

  future_reply_t client_getname();

  client &client_pause(int timeout, const reply_callback_t &reply_callback);

  future_reply_t client_pause(int timeout);

  client &client_reply(const string_t &mode,
                       const reply_callback_t &reply_callback);

  future_reply_t client_reply(const string_t &mode);

  client &client_setname(const string_t &name,
                         const reply_callback_t &reply_callback);

  future_reply_t client_setname(const string_t &name);
  //</editor-fold>

  client &client_unblock(int id, const reply_callback_t &reply_callback);

  client &client_unblock(int id, bool witherror,
                         const reply_callback_t &reply_callback);

  future_reply_t client_unblock(int id, bool witherror = false);

  client &cluster_addslots(const vector<string_t> &p_slots,
                           const reply_callback_t &reply_callback);

  future_reply_t cluster_addslots(const vector<string_t> &p_slots);

  client &cluster_count_failure_reports(const string_t &node_id,
                                        const reply_callback_t &reply_callback);

  future_reply_t cluster_count_failure_reports(const string_t &node_id);

  client &cluster_countkeysinslot(const string_t &slot,
                                  const reply_callback_t &reply_callback);

  future_reply_t cluster_countkeysinslot(const string_t &slot);

  client &cluster_delslots(const vector<string_t> &p_slots,
                           const reply_callback_t &reply_callback);

  future_reply_t cluster_delslots(const vector<string_t> &p_slots);

  client &cluster_failover(const reply_callback_t &reply_callback);

  future_reply_t cluster_failover();

  client &cluster_failover(const string_t &mode,
                           const reply_callback_t &reply_callback);

  future_reply_t cluster_failover(const string_t &mode);

  client &cluster_forget(const string_t &node_id,
                         const reply_callback_t &reply_callback);

  future_reply_t cluster_forget(const string_t &node_id);

  client &cluster_getkeysinslot(const string_t &slot, int count,
                                const reply_callback_t &reply_callback);

  future_reply_t cluster_getkeysinslot(const string_t &slot, int count);

  client &cluster_info(const reply_callback_t &reply_callback);

  future_reply_t cluster_info();

  client &cluster_keyslot(const string_t &key,
                          const reply_callback_t &reply_callback);

  future_reply_t cluster_keyslot(const string_t &key);

  client &cluster_meet(const string_t &ip, int port,
                       const reply_callback_t &reply_callback);

  future_reply_t cluster_meet(const string_t &ip, int port);

  client &cluster_nodes(const reply_callback_t &reply_callback);

  future_reply_t cluster_nodes();

  client &cluster_replicate(const string_t &node_id,
                            const reply_callback_t &reply_callback);

  future_reply_t cluster_replicate(const string_t &node_id);

  client &cluster_reset(const reply_callback_t &reply_callback);

  client &cluster_reset(const string_t &mode,
                        const reply_callback_t &reply_callback);

  future_reply_t cluster_reset(const string_t &mode = "soft");

  client &cluster_saveconfig(const reply_callback_t &reply_callback);

  future_reply_t cluster_saveconfig();

  client &cluster_set_config_epoch(const string_t &epoch,
                                   const reply_callback_t &reply_callback);

  future_reply_t cluster_set_config_epoch(const string_t &epoch);

  client &cluster_setslot(const string_t &slot, const string_t &mode,
                          const reply_callback_t &reply_callback);

  future_reply_t cluster_setslot(const string_t &slot, const string_t &mode);

  client &cluster_setslot(const string_t &slot, const string_t &mode,
                          const string_t &node_id,
                          const reply_callback_t &reply_callback);

  future_reply_t cluster_setslot(const string_t &slot, const string_t &mode,
                                 const string_t &node_id);

  client &cluster_slaves(const string_t &node_id,
                         const reply_callback_t &reply_callback);

  future_reply_t cluster_slaves(const string_t &node_id);

  client &cluster_slots(const reply_callback_t &reply_callback);

  future_reply_t cluster_slots();

  client &command(const reply_callback_t &reply_callback);

  future_reply_t command();

  client &command_count(const reply_callback_t &reply_callback);

  future_reply_t command_count();

  client &command_getkeys(const reply_callback_t &reply_callback);

  future_reply_t command_getkeys();

  client &command_info(const vector<string_t> &command_name,
                       const reply_callback_t &reply_callback);

  future_reply_t command_info(const vector<string_t> &command_name);

  client &config_get(const string_t &param,
                     const reply_callback_t &reply_callback);

  future_reply_t config_get(const string_t &param);

  client &config_rewrite(const reply_callback_t &reply_callback);

  future_reply_t config_rewrite();

  client &config_set(const string_t &param, const string_t &val,
                     const reply_callback_t &reply_callback);

  future_reply_t config_set(const string_t &param, const string_t &val);

  client &config_resetstat(const reply_callback_t &reply_callback);

  future_reply_t config_resetstat();

  client &dbsize(const reply_callback_t &reply_callback);

  future_reply_t dbsize();

  client &debug_object(const string_t &key,
                       const reply_callback_t &reply_callback);

  future_reply_t debug_object(const string_t &key);

  client &debug_segfault(const reply_callback_t &reply_callback);

  future_reply_t debug_segfault();

  client &decr(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t decr(const string_t &key);

  client &decrby(const string_t &key, int val,
                 const reply_callback_t &reply_callback);

  future_reply_t decrby(const string_t &key, int val);

  client &del(const vector<string_t> &key,
              const reply_callback_t &reply_callback);

  future_reply_t del(const vector<string_t> &key);

  client &discard(const reply_callback_t &reply_callback);

  future_reply_t discard();

  client &dump(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t dump(const string_t &key);

  client &echo(const string_t &msg, const reply_callback_t &reply_callback);

  future_reply_t echo(const string_t &msg);

  client &eval(const string_t &script, const vector<string_t> &keys,
               const vector<string_t> &args,
               const reply_callback_t &reply_callback);

  DEPRECATED client &eval(const string_t &script, int num_keys,
                          const vector<string_t> &keys,
                          const vector<string_t> &args,
                          const reply_callback_t &reply_callback);

  future_reply_t eval(const string_t &script, const vector<string_t> &keys,
                      const vector<string_t> &args);

  DEPRECATED future_reply_t eval(const string_t &script, int num_keys,
                                 const vector<string_t> &keys,
                                 const vector<string_t> &args);

  client &evalsha(const string_t &sha1, const vector<string_t> &keys,
                  const vector<string_t> &args,
                  const reply_callback_t &reply_callback);

  DEPRECATED client &evalsha(const string_t &sha1, int num_keys,
                             const vector<string_t> &keys,
                             const vector<string_t> &args,
                             const reply_callback_t &reply_callback);

  future_reply_t evalsha(const string_t &sha1, const vector<string_t> &keys,
                         const vector<string_t> &args);

  DEPRECATED future_reply_t evalsha(const string_t &sha1, int num_keys,
                                    const vector<string_t> &keys,
                                    const vector<string_t> &args);

  client &exec(const reply_callback_t &reply_callback);

  future_reply_t exec();

  client &exists(const vector<string_t> &keys,
                 const reply_callback_t &reply_callback);

  future_reply_t exists(const vector<string_t> &keys);

  client &expire(const string_t &key, int seconds,
                 const reply_callback_t &reply_callback);

  future_reply_t expire(const string_t &key, int seconds);

  client &expireat(const string_t &key, int timestamp,
                   const reply_callback_t &reply_callback);

  future_reply_t expireat(const string_t &key, int timestamp);

  client &flushall(const reply_callback_t &reply_callback);

  future_reply_t flushall();

  client &flushdb(const reply_callback_t &reply_callback);

  future_reply_t flushdb();

  client &
  geoadd(const string_t &key,
         const vector<tuple<string_t, string_t, string_t>> &long_lat_memb,
         const reply_callback_t &reply_callback);

  future_reply_t
  geoadd(const string_t &key,
         const vector<tuple<string_t, string_t, string_t>> &long_lat_memb);

  client &geohash(const string_t &key, const vector<string_t> &members,
                  const reply_callback_t &reply_callback);

  future_reply_t geohash(const string_t &key, const vector<string_t> &members);

  client &geopos(const string_t &key, const vector<string_t> &members,
                 const reply_callback_t &reply_callback);

  future_reply_t geopos(const string_t &key, const vector<string_t> &members);

  client &geodist(const string_t &key, const string_t &member_1,
                  const string_t &member_2,
                  const reply_callback_t &reply_callback);

  client &geodist(const string_t &key, const string_t &member_1,
                  const string_t &member_2, const string_t &unit,
                  const reply_callback_t &reply_callback);

  future_reply_t geodist(const string_t &key, const string_t &member_1,
                         const string_t &member_2,
                         const string_t &unit = __METER);

  client &georadius(const string_t &key, double longitude, double latitude,
                    double radius, geo_unit unit, bool with_coord,
                    bool with_dist, bool with_hash, bool asc_order,
                    const reply_callback_t &reply_callback);

  client &georadius(const string_t &key, double longitude, double latitude,
                    double radius, geo_unit unit, bool with_coord,
                    bool with_dist, bool with_hash, bool asc_order,
                    size_t count, const reply_callback_t &reply_callback);

  client &georadius(const string_t &key, double longitude, double latitude,
                    double radius, geo_unit unit, bool with_coord,
                    bool with_dist, bool with_hash, bool asc_order,
                    const string_t &store_key,
                    const reply_callback_t &reply_callback);

  client &georadius(const string_t &key, double longitude, double latitude,
                    double radius, geo_unit unit, bool with_coord,
                    bool with_dist, bool with_hash, bool asc_order,
                    const string_t &store_key, const string_t &storedist_key,
                    const reply_callback_t &reply_callback);

  client &georadius(const string_t &key, double longitude, double latitude,
                    double radius, geo_unit unit, bool with_coord,
                    bool with_dist, bool with_hash, bool asc_order,
                    size_t count, const string_t &store_key,
                    const reply_callback_t &reply_callback);

  client &georadius(const string_t &key, double longitude, double latitude,
                    double radius, geo_unit unit, bool with_coord,
                    bool with_dist, bool with_hash, bool asc_order,
                    size_t count, const string_t &store_key,
                    const string_t &storedist_key,
                    const reply_callback_t &reply_callback);

  future_reply_t georadius(const string_t &key, double longitude,
                           double latitude, double radius, geo_unit unit,
                           bool with_coord = false, bool with_dist = false,
                           bool with_hash = false, bool asc_order = false,
                           size_t count = 0, const string_t &store_key = "",
                           const string_t &storedist_key = "");

  client &georadiusbymember(const string_t &key, const string_t &member,
                            double radius, geo_unit unit, bool with_coord,
                            bool with_dist, bool with_hash, bool asc_order,
                            const reply_callback_t &reply_callback);

  client &georadiusbymember(const string_t &key, const string_t &member,
                            double radius, geo_unit unit, bool with_coord,
                            bool with_dist, bool with_hash, bool asc_order,
                            size_t count,
                            const reply_callback_t &reply_callback);

  client &georadiusbymember(const string_t &key, const string_t &member,
                            double radius, geo_unit unit, bool with_coord,
                            bool with_dist, bool with_hash, bool asc_order,
                            const string_t &store_key,
                            const reply_callback_t &reply_callback);

  client &georadiusbymember(const string_t &key, const string_t &member,
                            double radius, geo_unit unit, bool with_coord,
                            bool with_dist, bool with_hash, bool asc_order,
                            const string_t &store_key,
                            const string_t &storedist_key,
                            const reply_callback_t &reply_callback);

  client &georadiusbymember(const string_t &key, const string_t &member,
                            double radius, geo_unit unit, bool with_coord,
                            bool with_dist, bool with_hash, bool asc_order,
                            size_t count, const string_t &store_key,
                            const reply_callback_t &reply_callback);

  client &georadiusbymember(const string_t &key, const string_t &member,
                            double radius, geo_unit unit, bool with_coord,
                            bool with_dist, bool with_hash, bool asc_order,
                            size_t count, const string_t &store_key,
                            const string_t &storedist_key,
                            const reply_callback_t &reply_callback);

  future_reply_t georadiusbymember(
      const string_t &key, const string_t &member, double radius, geo_unit unit,
      bool with_coord = false, bool with_dist = false, bool with_hash = false,
      bool asc_order = false, size_t count = 0, const string_t &store_key = "",
      const string_t &storedist_key = "");

  client &get(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t get(const string_t &key);

  client &getbit(const string_t &key, int offset,
                 const reply_callback_t &reply_callback);

  future_reply_t getbit(const string_t &key, int offset);

  client &getrange(const string_t &key, int start, int end,
                   const reply_callback_t &reply_callback);

  future_reply_t getrange(const string_t &key, int start, int end);

  client &getset(const string_t &key, const string_t &val,
                 const reply_callback_t &reply_callback);

  future_reply_t getset(const string_t &key, const string_t &val);

  client &hdel(const string_t &key, const vector<string_t> &fields,
               const reply_callback_t &reply_callback);

  future_reply_t hdel(const string_t &key, const vector<string_t> &fields);

  client &hexists(const string_t &key, const string_t &field,
                  const reply_callback_t &reply_callback);

  future_reply_t hexists(const string_t &key, const string_t &field);

  client &hget(const string_t &key, const string_t &field,
               const reply_callback_t &reply_callback);

  future_reply_t hget(const string_t &key, const string_t &field);

  client &hgetall(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t hgetall(const string_t &key);

  client &hincrby(const string_t &key, const string_t &field, int incr,
                  const reply_callback_t &reply_callback);

  future_reply_t hincrby(const string_t &key, const string_t &field, int incr);

  client &hincrbyfloat(const string_t &key, const string_t &field, float incr,
                       const reply_callback_t &reply_callback);

  future_reply_t hincrbyfloat(const string_t &key, const string_t &field,
                              float incr);

  client &hkeys(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t hkeys(const string_t &key);

  client &hlen(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t hlen(const string_t &key);

  client &hmget(const string_t &key, const vector<string_t> &fields,
                const reply_callback_t &reply_callback);

  future_reply_t hmget(const string_t &key, const vector<string_t> &fields);

  client &hmset(const string_t &key,
                const vector<pair<string_t, string_t>> &field_val,
                const reply_callback_t &reply_callback);

  future_reply_t hmset(const string_t &key,
                       const vector<pair<string_t, string_t>> &field_val);

  client &hscan(const string_t &key, size_t cursor,
                const reply_callback_t &reply_callback);

  future_reply_t hscan(const string_t &key, size_t cursor);

  client &hscan(const string_t &key, size_t cursor, const string_t &pattern,
                const reply_callback_t &reply_callback);

  future_reply_t hscan(const string_t &key, size_t cursor,
                       const string_t &pattern);

  client &hscan(const string_t &key, size_t cursor, size_t count,
                const reply_callback_t &reply_callback);

  future_reply_t hscan(const string_t &key, size_t cursor, size_t count);

  client &hscan(const string_t &key, size_t cursor, const string_t &pattern,
                size_t count, const reply_callback_t &reply_callback);

  future_reply_t hscan(const string_t &key, size_t cursor,
                       const string_t &pattern, size_t count);

  client &hset(const string_t &key, const string_t &field,
               const string_t &value, const reply_callback_t &reply_callback);

  future_reply_t hset(const string_t &key, const string_t &field,
                      const string_t &value);

  client &hsetnx(const string_t &key, const string_t &field,
                 const string_t &value, const reply_callback_t &reply_callback);

  future_reply_t hsetnx(const string_t &key, const string_t &field,
                        const string_t &value);

  client &hstrlen(const string_t &key, const string_t &field,
                  const reply_callback_t &reply_callback);

  future_reply_t hstrlen(const string_t &key, const string_t &field);

  client &hvals(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t hvals(const string_t &key);

  client &incr(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t incr(const string_t &key);

  client &incrby(const string_t &key, int incr,
                 const reply_callback_t &reply_callback);

  future_reply_t incrby(const string_t &key, int incr);

  client &incrbyfloat(const string_t &key, float incr,
                      const reply_callback_t &reply_callback);

  future_reply_t incrbyfloat(const string_t &key, float incr);

  client &info(const reply_callback_t &reply_callback);

  client &info(const string_t &section, const reply_callback_t &reply_callback);

  future_reply_t info(const string_t &section = "default");

  client &keys(const string_t &pattern, const reply_callback_t &reply_callback);

  future_reply_t keys(const string_t &pattern);

  client &lastsave(const reply_callback_t &reply_callback);

  future_reply_t lastsave();

  client &lindex(const string_t &key, int index,
                 const reply_callback_t &reply_callback);

  future_reply_t lindex(const string_t &key, int index);

  client &linsert(const string_t &key, const string_t &before_after,
                  const string_t &pivot, const string_t &value,
                  const reply_callback_t &reply_callback);

  future_reply_t linsert(const string_t &key, const string_t &before_after,
                         const string_t &pivot, const string_t &value);

  client &llen(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t llen(const string_t &key);

  client &lpop(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t lpop(const string_t &key);

  client &lpush(const string_t &key, const vector<string_t> &values,
                const reply_callback_t &reply_callback);

  future_reply_t lpush(const string_t &key, const vector<string_t> &values);

  client &lpushx(const string_t &key, const string_t &value,
                 const reply_callback_t &reply_callback);

  future_reply_t lpushx(const string_t &key, const string_t &value);

  client &lrange(const string_t &key, int start, int stop,
                 const reply_callback_t &reply_callback);

  future_reply_t lrange(const string_t &key, int start, int stop);

  client &lrem(const string_t &key, int count, const string_t &value,
               const reply_callback_t &reply_callback);

  future_reply_t lrem(const string_t &key, int count, const string_t &value);

  client &lset(const string_t &key, int index, const string_t &value,
               const reply_callback_t &reply_callback);

  future_reply_t lset(const string_t &key, int index, const string_t &value);

  client &ltrim(const string_t &key, int start, int stop,
                const reply_callback_t &reply_callback);

  future_reply_t ltrim(const string_t &key, int start, int stop);

  client &mget(const vector<string_t> &keys,
               const reply_callback_t &reply_callback);

  future_reply_t mget(const vector<string_t> &keys);

  client &migrate(const string_t &host, int port, const string_t &key,
                  const string_t &dest_db, int timeout,
                  const reply_callback_t &reply_callback);

  client &migrate(const string_t &host, int port, const string_t &key,
                  const string_t &dest_db, int timeout, bool copy, bool replace,
                  const vector<string_t> &keys,
                  const reply_callback_t &reply_callback);

  future_reply_t migrate(const string_t &host, int port, const string_t &key,
                         const string_t &dest_db, int timeout,
                         bool copy = false, bool replace = false,
                         const vector<string_t> &keys = {});

  client &monitor(const reply_callback_t &reply_callback);

  future_reply_t monitor();

  client &move(const string_t &key, const string_t &db,
               const reply_callback_t &reply_callback);

  future_reply_t move(const string_t &key, const string_t &db);

  client &mset(const vector<pair<string_t, string_t>> &key_vals,
               const reply_callback_t &reply_callback);

  future_reply_t mset(const vector<pair<string_t, string_t>> &key_vals);

  client &msetnx(const vector<pair<string_t, string_t>> &key_vals,
                 const reply_callback_t &reply_callback);

  future_reply_t msetnx(const vector<pair<string_t, string_t>> &key_vals);

  client &multi(const reply_callback_t &reply_callback);

  future_reply_t multi();

  client &object(const string_t &sub_cmd, const vector<string_t> &args,
                 const reply_callback_t &reply_callback);

  future_reply_t object(const string_t &sub_cmd, const vector<string_t> &args);

  client &persist(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t persist(const string_t &key);

  client &pexpire(const string_t &key, int ms,
                  const reply_callback_t &reply_callback);

  future_reply_t pexpire(const string_t &key, int ms);

  client &pexpireat(const string_t &key, int ms_timestamp,
                    const reply_callback_t &reply_callback);

  future_reply_t pexpireat(const string_t &key, int ms_timestamp);

  client &pfadd(const string_t &key, const vector<string_t> &elements,
                const reply_callback_t &reply_callback);

  future_reply_t pfadd(const string_t &key, const vector<string_t> &elements);

  client &pfcount(const vector<string_t> &keys,
                  const reply_callback_t &reply_callback);

  future_reply_t pfcount(const vector<string_t> &keys);

  client &pfmerge(const string_t &destkey, const vector<string_t> &sourcekeys,
                  const reply_callback_t &reply_callback);

  future_reply_t pfmerge(const string_t &destkey,
                         const vector<string_t> &sourcekeys);

  client &ping(const reply_callback_t &reply_callback);

  future_reply_t ping();

  client &ping(const string_t &message, const reply_callback_t &reply_callback);

  future_reply_t ping(const string_t &message);

  client &psetex(const string_t &key, int64_t ms, const string_t &val,
                 const reply_callback_t &reply_callback);

  future_reply_t psetex(const string_t &key, int64_t ms, const string_t &val);

  client &publish(const string_t &channel, const string_t &message,
                  const reply_callback_t &reply_callback);

  future_reply_t publish(const string_t &channel, const string_t &message);

  client &pubsub(const string_t &sub_cmd, const vector<string_t> &args,
                 const reply_callback_t &reply_callback);

  future_reply_t pubsub(const string_t &sub_cmd, const vector<string_t> &args);

  client &pttl(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t pttl(const string_t &key);

  client &quit(const reply_callback_t &reply_callback);

  future_reply_t quit();

  client &randomkey(const reply_callback_t &reply_callback);

  future_reply_t randomkey();

  client &readonly(const reply_callback_t &reply_callback);

  future_reply_t readonly();

  client &readwrite(const reply_callback_t &reply_callback);

  future_reply_t readwrite();

  client &rename(const string_t &key, const string_t &newkey,
                 const reply_callback_t &reply_callback);

  future_reply_t rename(const string_t &key, const string_t &newkey);

  client &renamenx(const string_t &key, const string_t &newkey,
                   const reply_callback_t &reply_callback);

  future_reply_t renamenx(const string_t &key, const string_t &newkey);

  client &restore(const string_t &key, int ttl,
                  const string_t &serialized_value,
                  const reply_callback_t &reply_callback);

  future_reply_t restore(const string_t &key, int ttl,
                         const string_t &serialized_value);

  client &restore(const string_t &key, int ttl,
                  const string_t &serialized_value, const string_t &replace,
                  const reply_callback_t &reply_callback);

  future_reply_t restore(const string_t &key, int ttl,
                         const string_t &serialized_value,
                         const string_t &replace);

  client &role(const reply_callback_t &reply_callback);

  future_reply_t role();

  client &rpop(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t rpop(const string_t &key);

  client &rpoplpush(const string_t &source, const string_t &destination,
                    const reply_callback_t &reply_callback);

  future_reply_t rpoplpush(const string_t &src, const string_t &dst);

  client &rpush(const string_t &key, const vector<string_t> &values,
                const reply_callback_t &reply_callback);

  future_reply_t rpush(const string_t &key, const vector<string_t> &values);

  client &rpushx(const string_t &key, const string_t &value,
                 const reply_callback_t &reply_callback);

  future_reply_t rpushx(const string_t &key, const string_t &value);

  client &sadd(const string_t &key, const vector<string_t> &members,
               const reply_callback_t &reply_callback);

  future_reply_t sadd(const string_t &key, const vector<string_t> &members);

  client &save(const reply_callback_t &reply_callback);

  future_reply_t save();

  client &scan(size_t cursor, const reply_callback_t &reply_callback);

  future_reply_t scan(size_t cursor);

  client &scan(size_t cursor, const string_t &pattern,
               const reply_callback_t &reply_callback);

  future_reply_t scan(size_t cursor, const string_t &pattern);

  client &scan(size_t cursor, size_t count,
               const reply_callback_t &reply_callback);

  future_reply_t scan(size_t cursor, size_t count);

  client &scan(size_t cursor, const string_t &pattern, size_t count,
               const reply_callback_t &reply_callback);

  future_reply_t scan(size_t cursor, const string_t &pattern, size_t count);

  client &scard(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t scard(const string_t &key);

  client &script_debug(const string_t &mode,
                       const reply_callback_t &reply_callback);

  future_reply_t script_debug(const string_t &mode);

  client &script_exists(const vector<string_t> &scripts,
                        const reply_callback_t &reply_callback);

  future_reply_t script_exists(const vector<string_t> &scripts);

  client &script_flush(const reply_callback_t &reply_callback);

  future_reply_t script_flush();

  client &script_kill(const reply_callback_t &reply_callback);

  future_reply_t script_kill();

  client &script_load(const string_t &script,
                      const reply_callback_t &reply_callback);

  future_reply_t script_load(const string_t &script);

  client &sdiff(const vector<string_t> &keys,
                const reply_callback_t &reply_callback);

  future_reply_t sdiff(const vector<string_t> &keys);

  client &sdiffstore(const string_t &destination, const vector<string_t> &keys,
                     const reply_callback_t &reply_callback);

  future_reply_t sdiffstore(const string_t &dst, const vector<string_t> &keys);

  client &select(int index, const reply_callback_t &reply_callback);

  future_reply_t select(int index);

  client &set(const string_t &key, const string_t &value,
              const reply_callback_t &reply_callback);

  future_reply_t set(const string_t &key, const string_t &value);

  client &set_advanced(const string_t &key, const string_t &value,
                       const reply_callback_t &reply_callback);

  client &set_advanced(const string_t &key, const string_t &value, bool ex,
                       int ex_sec, bool px, int px_milli, bool nx, bool xx,
                       const reply_callback_t &reply_callback);

  future_reply_t set_advanced(const string_t &key, const string_t &value,
                              bool ex = false, int ex_sec = 0, bool px = false,
                              int px_milli = 0, bool nx = false,
                              bool xx = false);

  client &setbit(const string_t &key, int offset, const string_t &value,
                 const reply_callback_t &reply_callback);

  future_reply_t setbit(const string_t &key, int offset, const string_t &value);

  client &setex(const string_t &key, int64_t seconds, const string_t &value,
                const reply_callback_t &reply_callback);

  future_reply_t setex(const string_t &key, int64_t seconds,
                       const string_t &value);

  client &setnx(const string_t &key, const string_t &value,
                const reply_callback_t &reply_callback);

  future_reply_t setnx(const string_t &key, const string_t &value);

  client &set_range(const string_t &key, int offset, const string_t &value,
                    const reply_callback_t &reply_callback);

  future_reply_t set_range(const string_t &key, int offset,
                           const string_t &value);

  client &shutdown(const reply_callback_t &reply_callback);

  future_reply_t shutdown();

  client &shutdown(const string_t &save,
                   const reply_callback_t &reply_callback);

  future_reply_t shutdown(const string_t &save);

  client &sinter(const vector<string_t> &keys,
                 const reply_callback_t &reply_callback);

  future_reply_t sinter(const vector<string_t> &keys);

  client &sinterstore(const string_t &destination, const vector<string_t> &keys,
                      const reply_callback_t &reply_callback);

  future_reply_t sinterstore(const string_t &dst, const vector<string_t> &keys);

  client &sismember(const string_t &key, const string_t &member,
                    const reply_callback_t &reply_callback);

  future_reply_t sismember(const string_t &key, const string_t &member);

  client &replicaof(const string_t &host, int port,
                  const reply_callback_t &reply_callback);

  DEPRECATED client &slaveof(const string_t &host, int port,
                  const reply_callback_t &reply_callback);

  DEPRECATED future_reply_t slaveof(const string_t &host, int port);

  future_reply_t replicaof(const string_t &host, int port);

  client &slowlog(string_t sub_cmd, const reply_callback_t &reply_callback);

  future_reply_t slowlog(const string_t &sub_cmd);

  client &slowlog(string_t sub_cmd, const string_t &argument,
                  const reply_callback_t &reply_callback);

  future_reply_t slowlog(const string_t &sub_cmd, const string_t &argument);

  client &smembers(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t smembers(const string_t &key);

  client &smove(const string_t &source, const string_t &destination,
                const string_t &member, const reply_callback_t &reply_callback);

  future_reply_t smove(const string_t &src, const string_t &dst,
                       const string_t &member);

  client &sort(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key);

  client &sort(const string_t &key, const vector<string_t> &get_patterns,
               bool asc_order, bool alpha,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, const vector<string_t> &get_patterns,
                      bool asc_order, bool alpha);

  client &sort(const string_t &key, size_t offset, size_t count,
               const vector<string_t> &get_patterns, bool asc_order, bool alpha,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, size_t offset, size_t count,
                      const vector<string_t> &get_patterns, bool asc_order,
                      bool alpha);

  client &sort(const string_t &key, const string_t &by_pattern,
               const vector<string_t> &get_patterns, bool asc_order, bool alpha,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, const string_t &by_pattern,
                      const vector<string_t> &get_patterns, bool asc_order,
                      bool alpha);

  client &sort(const string_t &key, const vector<string_t> &get_patterns,
               bool asc_order, bool alpha, const string_t &store_dest,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, const vector<string_t> &get_patterns,
                      bool asc_order, bool alpha, const string_t &store_dest);

  client &sort(const string_t &key, size_t offset, size_t count,
               const vector<string_t> &get_patterns, bool asc_order, bool alpha,
               const string_t &store_dest,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, size_t offset, size_t count,
                      const vector<string_t> &get_patterns, bool asc_order,
                      bool alpha, const string_t &store_dest);

  client &sort(const string_t &key, const string_t &by_pattern,
               const vector<string_t> &get_patterns, bool asc_order, bool alpha,
               const string_t &store_dest,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, const string_t &by_pattern,
                      const vector<string_t> &get_patterns, bool asc_order,
                      bool alpha, const string_t &store_dest);

  client &sort(const string_t &key, const string_t &by_pattern, size_t offset,
               size_t count, const vector<string_t> &get_patterns,
               bool asc_order, bool alpha,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, const string_t &by_pattern,
                      size_t offset, size_t count,
                      const vector<string_t> &get_patterns, bool asc_order,
                      bool alpha);

  client &sort(const string_t &key, const string_t &by_pattern, size_t offset,
               size_t count, const vector<string_t> &get_patterns,
               bool asc_order, bool alpha, const string_t &store_dest,
               const reply_callback_t &reply_callback);

  future_reply_t sort(const string_t &key, const string_t &by_pattern,
                      size_t offset, size_t count,
                      const vector<string_t> &get_patterns, bool asc_order,
                      bool alpha, const string_t &store_dest);

  client &spop(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t spop(const string_t &key);

  client &spop(const string_t &key, int count,
               const reply_callback_t &reply_callback);

  future_reply_t spop(const string_t &key, int count);

  client &srandmember(const string_t &key,
                      const reply_callback_t &reply_callback);

  future_reply_t srandmember(const string_t &key);

  client &srandmember(const string_t &key, int count,
                      const reply_callback_t &reply_callback);

  future_reply_t srandmember(const string_t &key, int count);

  client &srem(const string_t &key, const vector<string_t> &members,
               const reply_callback_t &reply_callback);

  future_reply_t srem(const string_t &key, const vector<string_t> &members);

  client &sscan(const string_t &key, size_t cursor,
                const reply_callback_t &reply_callback);

  future_reply_t sscan(const string_t &key, size_t cursor);

  client &sscan(const string_t &key, size_t cursor, const string_t &pattern,
                const reply_callback_t &reply_callback);

  future_reply_t sscan(const string_t &key, size_t cursor,
                       const string_t &pattern);

  client &sscan(const string_t &key, size_t cursor, size_t count,
                const reply_callback_t &reply_callback);

  future_reply_t sscan(const string_t &key, size_t cursor, size_t count);

  client &sscan(const string_t &key, size_t cursor, const string_t &pattern,
                size_t count, const reply_callback_t &reply_callback);

  future_reply_t sscan(const string_t &key, size_t cursor,
                       const string_t &pattern, size_t count);

  client &strlen(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t strlen(const string_t &key);

  client &sunion(const vector<string_t> &keys,
                 const reply_callback_t &reply_callback);

  future_reply_t sunion(const vector<string_t> &keys);

  client &sunionstore(const string_t &destination, const vector<string_t> &keys,
                      const reply_callback_t &reply_callback);

  future_reply_t sunionstore(const string_t &dst, const vector<string_t> &keys);

  client &sync(const reply_callback_t &reply_callback);

  future_reply_t sync();

  client &time(const reply_callback_t &reply_callback);

  future_reply_t time();

  client &ttl(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t ttl(const string_t &key);

  client &type(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t type(const string_t &key);

  client &unwatch(const reply_callback_t &reply_callback);

  future_reply_t unwatch();

  client &wait(int num_replicas, int timeout,
               const reply_callback_t &reply_callback);

  future_reply_t wait(int num_replicas, int timeout);

  client &watch(const vector<string_t> &keys,
                const reply_callback_t &reply_callback);

  future_reply_t watch(const vector<string_t> &keys);

  //!
  //!  @brief
  //!  @param stream
  //!  @param group
  //!  @param message_ids
  //!  @param reply_callback
  //!  @return
  //!
  client &xack(const string_t &stream, const string_t &group,
               const vector<string_t> &message_ids,
               const reply_callback_t &reply_callback);

  future_reply_t xack(const string_t &key, const string_t &group,
                      const vector<string_t> &id_members);

  client &xadd(const string_t &key, const string_t &id,
               const multimap<string_t, string_t> &field_members,
               const reply_callback_t &reply_callback);

  future_reply_t xadd(const string_t &key, const string_t &id,
                      const multimap<string_t, string_t> &field_members);

  //!
  //!  @brief changes the ownership of a pending message to the specified
  //!  consumer
  //!  @param stream
  //!  @param group
  //!  @param consumer
  //!  @param min_idle_time
  //!  @param message_ids
  //!  @param reply_callback
  //!  @return
  //!
  client &xclaim(const string_t &stream, const string_t &group,
                 const string_t &consumer, int min_idle_time,
                 const vector<string_t> &message_ids,
                 const xclaim_options_t &options,
                 const reply_callback_t &reply_callback);

  future_reply_t xclaim(const string_t &key, const string_t &group,
                        const string_t &consumer, const int &min_idle_time,
                        const vector<string_t> &id_members,
                        const xclaim_options_t &options);

  client &xdel(const string_t &key, const vector<string_t> &id_members,
               const reply_callback_t &reply_callback);

  future_reply_t xdel(const string_t &key, const vector<string_t> &id_members);

  client &xgroup_create(const string_t &key, const string_t &group_name,
                        const reply_callback_t &reply_callback);

  client &xgroup_create(const string_t &key, const string_t &group_name,
                        const string_t &id,
                        const reply_callback_t &reply_callback);

  future_reply_t xgroup_create(const string_t &key, const string_t &group_name,
                               const string_t &id = "$");

  client &xgroup_set_id(const string_t &key, const string_t &group_name,
                        const reply_callback_t &reply_callback);

  client &xgroup_set_id(const string_t &key, const string_t &group_name,
                        const string_t &id,
                        const reply_callback_t &reply_callback);

  future_reply_t xgroup_set_id(const string_t &key, const string_t &group_name,
                               const string_t &id = "$");

  client &xgroup_destroy(const string_t &key, const string_t &group_name,
                         const reply_callback_t &reply_callback);

  future_reply_t xgroup_destroy(const string_t &key,
                                const string_t &group_name);

  client &xgroup_del_consumer(const string_t &key, const string_t &group_name,
                              const string_t &consumer_name,
                              const reply_callback_t &reply_callback);

  future_reply_t xgroup_del_consumer(const string_t &key,
                                     const string_t &group_name,
                                     const string_t &consumer_name);

  //!
  //!  @brief introspection command used in order to retrieve different
  //!  information about the consumer groups
  //!  @param key stream id
  //!  @param group_name stream consumer group name
  //!  @return
  //!
  client &xinfo_consumers(const string_t &key, const string_t &group_name,
                          const reply_callback_t &reply_callback);

  //!
  //!  @brief \copybrief client::xinfo_consumers(key, group_name,
  //!  reply_callback)
  //!  @param key stream id
  //!  @param group_name
  //!  @return
  //!
  future_reply_t xinfo_consumers(const string_t &key,
                                 const string_t &group_name);

  //!
  //!  @brief \copybrief client::xinfo_consumers(key, group_name,
  //!  reply_callback)
  //!  @param key stream id
  //!  @param reply_callback
  //!  @return
  //!
  client &xinfo_groups(const string_t &key,
                       const reply_callback_t &reply_callback);

  //!
  //!  @brief \copybrief client::xinfo_consumers(key, group_name,
  //!  reply_callback)
  //!  @param stream stream id
  //!  @return
  //!
  future_reply_t xinfo_groups(const string_t &stream);

  client &xinfo_stream(const string_t &stream,
                       const reply_callback_t &reply_callback);

  future_reply_t xinfo_stream(const string_t &stream);

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
  client &xlen(const string_t &stream, const reply_callback_t &reply_callback);

  //!
  //!  @copydoc client::xlen(key, reply_callback)
  //!  @param key
  //!  @return
  //!
  future_reply_t xlen(const string_t &key);

  //!
  //!  @brief inspects the list of pending messages for the stream & group
  //!  @param stream
  //!  @param group
  //!  @param options
  //!  @param reply_callback
  //!  @return
  //!
  client &xpending(const string_t &stream, const string_t &group,
                   const xpending_options_t &options,
                   const reply_callback_t &reply_callback);

  future_reply_t xpending(const string_t &stream, const string_t &group,
                          const xpending_options_t &options);
  // endregion

  //!
  //!  @brief
  //!  @param stream
  //!  @param options
  //!  @param reply_callback
  //!  @return
  //!
  client &xrange(const string_t &stream, const range_options_t &options,
                 const reply_callback_t &reply_callback);

  future_reply_t xrange(const string_t &stream,
                        const range_options_t &range_args);

  //!
  //!  @brief
  //!  @param a streams_t Streams int_t Count int_t Block;
  //!  @param reply_callback
  //!  @return
  //!
  client &xread(const xread_options_t &a,
                const reply_callback_t &reply_callback);

  future_reply_t xread(const xread_options_t &a);

  client &xreadgroup(const xreadgroup_options_t &a,
                     const reply_callback_t &reply_callback);

  future_reply_t xreadgroup(const xreadgroup_options_t &a);

  client &xrevrange(const string_t &key, const range_options_t &range_args,
                    const reply_callback_t &reply_callback);

  future_reply_t xrevrange(const string_t &key,
                           const range_options_t &range_args);

  //!
  //!  @brief trims the stream to a given number of items, evicting older items
  //!  (items with lower IDs) if needed
  //!  @param stream
  //!  @param max_len
  //!  @param reply_callback
  //!  @return
  //!
  client &xtrim(const string_t &stream, int max_len,
                const reply_callback_t &reply_callback);

  future_reply_t xtrim(const string_t &key, int max_len);

  //!
  //!  optimizes the xtrim command
  //!
  client &xtrim_approx(const string_t &key, int max_len,
                       const reply_callback_t &reply_callback);

  future_reply_t xtrim_approx(const string_t &key, int max_len);

  client &zadd(const string_t &key, const vector<string_t> &options,
               const multimap<string_t, string_t> &score_members,
               const reply_callback_t &reply_callback);

  future_reply_t zadd(const string_t &key, const vector<string_t> &options,
                      const multimap<string_t, string_t> &score_members);

  client &zcard(const string_t &key, const reply_callback_t &reply_callback);

  future_reply_t zcard(const string_t &key);

  client &zcount(const string_t &key, int min, int max,
                 const reply_callback_t &reply_callback);

  future_reply_t zcount(const string_t &key, int min, int max);

  client &zcount(const string_t &key, double min, double max,
                 const reply_callback_t &reply_callback);

  future_reply_t zcount(const string_t &key, double min, double max);

  client &zcount(const string_t &key, const string_t &min, const string_t &max,
                 const reply_callback_t &reply_callback);

  future_reply_t zcount(const string_t &key, const string_t &min,
                        const string_t &max);

  client &zincrby(const string_t &key, int incr, const string_t &member,
                  const reply_callback_t &reply_callback);

  future_reply_t zincrby(const string_t &key, int incr, const string_t &member);

  client &zincrby(const string_t &key, double incr, const string_t &member,
                  const reply_callback_t &reply_callback);

  future_reply_t zincrby(const string_t &key, double incr,
                         const string_t &member);

  client &zincrby(const string_t &key, const string_t &incr,
                  const string_t &member,
                  const reply_callback_t &reply_callback);

  future_reply_t zincrby(const string_t &key, const string_t &incr,
                         const string_t &member);

  client &zinterstore(const string_t &destination, size_t num_keys,
                      const vector<string_t> &keys, vector<size_t> weights,
                      aggregate_method method,
                      const reply_callback_t &reply_callback);

  future_reply_t zinterstore(const string_t &destination, size_t num_keys,
                             const vector<string_t> &keys,
                             vector<size_t> weights, aggregate_method method);

  client &zlexcount(const string_t &key, int min, int max,
                    const reply_callback_t &reply_callback);

  future_reply_t zlexcount(const string_t &key, int min, int max);

  client &zlexcount(const string_t &key, double min, double max,
                    const reply_callback_t &reply_callback);

  future_reply_t zlexcount(const string_t &key, double min, double max);

  client &zlexcount(const string_t &key, const string_t &min,
                    const string_t &max,
                    const reply_callback_t &reply_callback);

  future_reply_t zlexcount(const string_t &key, const string_t &min,
                           const string_t &max);

  client &zpopmin(const string_t &key, int count,
                  const reply_callback_t &reply_callback);

  future_reply_t zpopmin(const string_t &key, int count);

  client &zpopmax(const string_t &key, int count,
                  const reply_callback_t &reply_callback);

  future_reply_t zpopmax(const string_t &key, int count);

  client &zrange(const string_t &key, int start, int stop,
                 const reply_callback_t &reply_callback);

  client &zrange(const string_t &key, int start, int stop, bool with_scores,
                 const reply_callback_t &reply_callback);

  future_reply_t zrange(const string_t &key, int start, int stop,
                        bool with_scores = false);

  client &zrange(const string_t &key, double start, double stop,
                 const reply_callback_t &reply_callback);

  client &zrange(const string_t &key, double start, double stop,
                 bool with_scores, const reply_callback_t &reply_callback);

  future_reply_t zrange(const string_t &key, double start, double stop,
                        bool with_scores = false);

  client &zrange(const string_t &key, const string_t &start,
                 const string_t &stop, const reply_callback_t &reply_callback);

  client &zrange(const string_t &key, const string_t &start,
                 const string_t &stop, bool with_scores,
                 const reply_callback_t &reply_callback);

  future_reply_t zrange(const string_t &key, const string_t &start,
                        const string_t &stop, bool with_scores = false);

  client &zrangebylex(const string_t &key, int min, int max,
                      const reply_callback_t &reply_callback);

  client &zrangebylex(const string_t &key, int min, int max, bool with_scores,
                      const reply_callback_t &reply_callback);

  future_reply_t zrangebylex(const string_t &key, int min, int max,
                             bool with_scores = false);

  client &zrangebylex(const string_t &key, double min, double max,
                      const reply_callback_t &reply_callback);

  client &zrangebylex(const string_t &key, double min, double max,
                      bool with_scores, const reply_callback_t &reply_callback);

  future_reply_t zrangebylex(const string_t &key, double min, double max,
                             bool with_scores = false);

  client &zrangebylex(const string_t &key, const string_t &min,
                      const string_t &max,
                      const reply_callback_t &reply_callback);

  client &zrangebylex(const string_t &key, const string_t &min,
                      const string_t &max, bool with_scores,
                      const reply_callback_t &reply_callback);

  future_reply_t zrangebylex(const string_t &key, const string_t &min,
                             const string_t &max, bool with_scores = false);

  client &zrangebylex(const string_t &key, int min, int max, size_t offset,
                      size_t count, const reply_callback_t &reply_callback);

  client &zrangebylex(const string_t &key, int min, int max, size_t offset,
                      size_t count, bool with_scores,
                      const reply_callback_t &reply_callback);

  future_reply_t zrangebylex(const string_t &key, int min, int max,
                             size_t offset, size_t count,
                             bool with_scores = false);

  client &zrangebylex(const string_t &key, double min, double max,
                      size_t offset, size_t count,
                      const reply_callback_t &reply_callback);

  client &zrangebylex(const string_t &key, double min, double max,
                      size_t offset, size_t count, bool with_scores,
                      const reply_callback_t &reply_callback);

  future_reply_t zrangebylex(const string_t &key, double min, double max,
                             size_t offset, size_t count,
                             bool with_scores = false);

  client &zrangebylex(const string_t &key, const string_t &min,
                      const string_t &max, size_t offset, size_t count,
                      const reply_callback_t &reply_callback);

  client &zrangebylex(const string_t &key, const string_t &min,
                      const string_t &max, size_t offset, size_t count,
                      bool with_scores, const reply_callback_t &reply_callback);

  future_reply_t zrangebylex(const string_t &key, const string_t &min,
                             const string_t &max, size_t offset, size_t count,
                             bool with_scores = false);

  client &zrangebyscore(const string_t &key, int min, int max,
                        const reply_callback_t &reply_callback);

  client &zrangebyscore(const string_t &key, int min, int max, bool with_scores,
                        const reply_callback_t &reply_callback);

  future_reply_t zrangebyscore(const string_t &key, int min, int max,
                               bool with_scores = false);

  client &zrangebyscore(const string_t &key, double min, double max,
                        const reply_callback_t &reply_callback);

  client &zrangebyscore(const string_t &key, double min, double max,
                        bool with_scores,
                        const reply_callback_t &reply_callback);

  future_reply_t zrangebyscore(const string_t &key, double min, double max,
                               bool with_scores = false);

  client &zrangebyscore(const string_t &key, const string_t &min,
                        const string_t &max,
                        const reply_callback_t &reply_callback);

  client &zrangebyscore(const string_t &key, const string_t &min,
                        const string_t &max, bool with_scores,
                        const reply_callback_t &reply_callback);

  future_reply_t zrangebyscore(const string_t &key, const string_t &min,
                               const string_t &max, bool with_scores = false);

  client &zrangebyscore(const string_t &key, int min, int max, size_t offset,
                        size_t count, const reply_callback_t &reply_callback);

  client &zrangebyscore(const string_t &key, int min, int max, size_t offset,
                        size_t count, bool with_scores,
                        const reply_callback_t &reply_callback);

  future_reply_t zrangebyscore(const string_t &key, int min, int max,
                               size_t offset, size_t count,
                               bool with_scores = false);

  client &zrangebyscore(const string_t &key, double min, double max,
                        size_t offset, size_t count,
                        const reply_callback_t &reply_callback);

  client &zrangebyscore(const string_t &key, double min, double max,
                        size_t offset, size_t count, bool with_scores,
                        const reply_callback_t &reply_callback);

  future_reply_t zrangebyscore(const string_t &key, double min, double max,
                               size_t offset, size_t count,
                               bool with_scores = false);

  client &zrangebyscore(const string_t &key, const string_t &min,
                        const string_t &max, size_t offset, size_t count,
                        const reply_callback_t &reply_callback);

  client &zrangebyscore(const string_t &key, const string_t &min,
                        const string_t &max, size_t offset, size_t count,
                        bool with_scores,
                        const reply_callback_t &reply_callback);

  future_reply_t zrangebyscore(const string_t &key, const string_t &min,
                               const string_t &max, size_t offset, size_t count,
                               bool with_scores = false);

  client &zrank(const string_t &key, const string_t &member,
                const reply_callback_t &reply_callback);

  future_reply_t zrank(const string_t &key, const string_t &member);

  client &zrem(const string_t &key, const vector<string_t> &members,
               const reply_callback_t &reply_callback);

  future_reply_t zrem(const string_t &key, const vector<string_t> &members);

  client &zremrangebylex(const string_t &key, int min, int max,
                         const reply_callback_t &reply_callback);

  future_reply_t zremrangebylex(const string_t &key, int min, int max);

  client &zremrangebylex(const string_t &key, double min, double max,
                         const reply_callback_t &reply_callback);

  future_reply_t zremrangebylex(const string_t &key, double min, double max);

  client &zremrangebylex(const string_t &key, const string_t &min,
                         const string_t &max,
                         const reply_callback_t &reply_callback);

  future_reply_t zremrangebylex(const string_t &key, const string_t &min,
                                const string_t &max);

  client &zremrangebyrank(const string_t &key, int start, int stop,
                          const reply_callback_t &reply_callback);

  future_reply_t zremrangebyrank(const string_t &key, int start, int stop);

  client &zremrangebyrank(const string_t &key, double start, double stop,
                          const reply_callback_t &reply_callback);

  future_reply_t zremrangebyrank(const string_t &key, double start,
                                 double stop);

  client &zremrangebyrank(const string_t &key, const string_t &start,
                          const string_t &stop,
                          const reply_callback_t &reply_callback);

  future_reply_t zremrangebyrank(const string_t &key, const string_t &start,
                                 const string_t &stop);

  client &zremrangebyscore(const string_t &key, int min, int max,
                           const reply_callback_t &reply_callback);

  future_reply_t zremrangebyscore(const string_t &key, int min, int max);

  client &zremrangebyscore(const string_t &key, double min, double max,
                           const reply_callback_t &reply_callback);

  future_reply_t zremrangebyscore(const string_t &key, double min, double max);

  client &zremrangebyscore(const string_t &key, const string_t &min,
                           const string_t &max,
                           const reply_callback_t &reply_callback);

  future_reply_t zremrangebyscore(const string_t &key, const string_t &min,
                                  const string_t &max);

  client &zrevrange(const string_t &key, int start, int stop,
                    const reply_callback_t &reply_callback);

  client &zrevrange(const string_t &key, int start, int stop, bool with_scores,
                    const reply_callback_t &reply_callback);

  future_reply_t zrevrange(const string_t &key, int start, int stop,
                           bool with_scores = false);

  client &zrevrange(const string_t &key, double start, double stop,
                    const reply_callback_t &reply_callback);

  client &zrevrange(const string_t &key, double start, double stop,
                    bool with_scores, const reply_callback_t &reply_callback);

  future_reply_t zrevrange(const string_t &key, double start, double stop,
                           bool with_scores = false);

  client &zrevrange(const string_t &key, const string_t &start,
                    const string_t &stop,
                    const reply_callback_t &reply_callback);

  client &zrevrange(const string_t &key, const string_t &start,
                    const string_t &stop, bool with_scores,
                    const reply_callback_t &reply_callback);

  future_reply_t zrevrange(const string_t &key, const string_t &start,
                           const string_t &stop, bool with_scores = false);

  client &zrevrangebylex(const string_t &key, int max, int min,
                         const reply_callback_t &reply_callback);

  client &zrevrangebylex(const string_t &key, int max, int min,
                         bool with_scores,
                         const reply_callback_t &reply_callback);

  future_reply_t zrevrangebylex(const string_t &key, int max, int min,
                                bool with_scores = false);

  client &zrevrangebylex(const string_t &key, double max, double min,
                         const reply_callback_t &reply_callback);

  client &zrevrangebylex(const string_t &key, double max, double min,
                         bool with_scores,
                         const reply_callback_t &reply_callback);

  future_reply_t zrevrangebylex(const string_t &key, double max, double min,
                                bool with_scores = false);

  client &zrevrangebylex(const string_t &key, const string_t &max,
                         const string_t &min,
                         const reply_callback_t &reply_callback);

  client &zrevrangebylex(const string_t &key, const string_t &max,
                         const string_t &min, bool with_scores,
                         const reply_callback_t &reply_callback);

  future_reply_t zrevrangebylex(const string_t &key, const string_t &max,
                                const string_t &min, bool with_scores = false);

  client &zrevrangebylex(const string_t &key, int max, int min, size_t offset,
                         size_t count, const reply_callback_t &reply_callback);

  client &zrevrangebylex(const string_t &key, int max, int min, size_t offset,
                         size_t count, bool with_scores,
                         const reply_callback_t &reply_callback);

  future_reply_t zrevrangebylex(const string_t &key, int max, int min,
                                size_t offset, size_t count,
                                bool with_scores = false);

  client &zrevrangebylex(const string_t &key, double max, double min,
                         size_t offset, size_t count,
                         const reply_callback_t &reply_callback);

  client &zrevrangebylex(const string_t &key, double max, double min,
                         size_t offset, size_t count, bool with_scores,
                         const reply_callback_t &reply_callback);

  future_reply_t zrevrangebylex(const string_t &key, double max, double min,
                                size_t offset, size_t count,
                                bool with_scores = false);

  client &zrevrangebylex(const string_t &key, const string_t &max,
                         const string_t &min, size_t offset, size_t count,
                         const reply_callback_t &reply_callback);

  client &zrevrangebylex(const string_t &key, const string_t &max,
                         const string_t &min, size_t offset, size_t count,
                         bool with_scores,
                         const reply_callback_t &reply_callback);

  future_reply_t zrevrangebylex(const string_t &key, const string_t &max,
                                const string_t &min, size_t offset,
                                size_t count, bool with_scores = false);

  client &zrevrangebyscore(const string_t &key, int max, int min,
                           const reply_callback_t &reply_callback);

  client &zrevrangebyscore(const string_t &key, int max, int min,
                           bool with_scores,
                           const reply_callback_t &reply_callback);

  future_reply_t zrevrangebyscore(const string_t &key, int max, int min,
                                  bool with_scores = false);

  client &zrevrangebyscore(const string_t &key, double max, double min,
                           const reply_callback_t &reply_callback);

  client &zrevrangebyscore(const string_t &key, double max, double min,
                           bool with_scores,
                           const reply_callback_t &reply_callback);

  future_reply_t zrevrangebyscore(const string_t &key, double max, double min,
                                  bool with_scores = false);

  client &zrevrangebyscore(const string_t &key, const string_t &max,
                           const string_t &min,
                           const reply_callback_t &reply_callback);

  client &zrevrangebyscore(const string_t &key, const string_t &max,
                           const string_t &min, bool with_scores,
                           const reply_callback_t &reply_callback);

  future_reply_t zrevrangebyscore(const string_t &key, const string_t &max,
                                  const string_t &min,
                                  bool with_scores = false);

  client &zrevrangebyscore(const string_t &key, int max, int min, size_t offset,
                           size_t count,
                           const reply_callback_t &reply_callback);

  client &zrevrangebyscore(const string_t &key, int max, int min, size_t offset,
                           size_t count, bool with_scores,
                           const reply_callback_t &reply_callback);

  future_reply_t zrevrangebyscore(const string_t &key, int max, int min,
                                  size_t offset, size_t count,
                                  bool with_scores = false);

  client &zrevrangebyscore(const string_t &key, double max, double min,
                           size_t offset, size_t count,
                           const reply_callback_t &reply_callback);

  client &zrevrangebyscore(const string_t &key, double max, double min,
                           size_t offset, size_t count, bool with_scores,
                           const reply_callback_t &reply_callback);

  future_reply_t zrevrangebyscore(const string_t &key, double max, double min,
                                  size_t offset, size_t count,
                                  bool with_scores = false);

  client &zrevrangebyscore(const string_t &key, const string_t &max,
                           const string_t &min, size_t offset, size_t count,
                           const reply_callback_t &reply_callback);

  client &zrevrangebyscore(const string_t &key, const string_t &max,
                           const string_t &min, size_t offset, size_t count,
                           bool with_scores,
                           const reply_callback_t &reply_callback);

  future_reply_t zrevrangebyscore(const string_t &key, const string_t &max,
                                  const string_t &min, size_t offset,
                                  size_t count, bool with_scores = false);

  client &zrevrank(const string_t &key, const string_t &member,
                   const reply_callback_t &reply_callback);

  future_reply_t zrevrank(const string_t &key, const string_t &member);

  client &zscan(const string_t &key, size_t cursor,
                const reply_callback_t &reply_callback);

  future_reply_t zscan(const string_t &key, size_t cursor);

  client &zscan(const string_t &key, size_t cursor, const string_t &pattern,
                const reply_callback_t &reply_callback);

  future_reply_t zscan(const string_t &key, size_t cursor,
                       const string_t &pattern);

  client &zscan(const string_t &key, size_t cursor, size_t count,
                const reply_callback_t &reply_callback);

  future_reply_t zscan(const string_t &key, size_t cursor, size_t count);

  client &zscan(const string_t &key, size_t cursor, const string_t &pattern,
                size_t count, const reply_callback_t &reply_callback);

  future_reply_t zscan(const string_t &key, size_t cursor,
                       const string_t &pattern, size_t count);

  client &zscore(const string_t &key, const string_t &member,
                 const reply_callback_t &reply_callback);

  future_reply_t zscore(const string_t &key, const string_t &member);

  client &zunionstore(const string_t &destination, size_t num_keys,
                      const vector<string_t> &keys, vector<size_t> weights,
                      aggregate_method method,
                      const reply_callback_t &reply_callback);

  future_reply_t zunionstore(const string_t &destination, size_t num_keys,
                             const vector<string_t> &keys,
                             vector<size_t> weights, aggregate_method method);

private:
  //!
  //!  client kill impl
  //!
  template <typename T>
  typename std::enable_if<std::is_same<T, client_type>::value>::type
  client_kill_unpack_arg(vector<string_t> &redis_cmd, reply_callback_t &,
                         client_type type);

  template <typename T>
  typename std::enable_if<std::is_same<T, bool>::value>::type
  client_kill_unpack_arg(vector<string_t> &redis_cmd, reply_callback_t &,
                         bool skip);

  template <typename T>
  typename std::enable_if<std::is_integral<T>::value>::type
  client_kill_unpack_arg(vector<string_t> &redis_cmd, reply_callback_t &,
                         uint64_t id);

  template <typename T>
  typename std::enable_if<std::is_class<T>::value>::type
  client_kill_unpack_arg(vector<string_t> &, reply_callback_t &reply_callback,
                         const T &cb);

  template <typename T, typename... Ts>
  void client_kill_impl(vector<string_t> &redis_cmd, reply_callback_t &reply,
                        const T &arg, const Ts &... args);

  template <typename T>
  void client_kill_impl(vector<string_t> &redis_cmd, reply_callback_t &reply,
                        const T &arg);

private:
  //!
  //!  sort impl
  //!
  client &sort(const string_t &key, const string_t &by_pattern, bool limit,
               size_t offset, size_t count,
               const vector<string_t> &get_patterns, bool asc_order, bool alpha,
               const string_t &store_dest,
               const reply_callback_t &reply_callback);

  //!
  //!  zrevrangebyscore impl
  //!
  client &zrevrangebyscore(const string_t &key, const string_t &max,
                           const string_t &min, bool limit, size_t offset,
                           size_t count, bool with_scores,
                           const reply_callback_t &reply_callback);

  //!
  //!  zrangebyscore impl
  //!
  client &zrangebyscore(const string_t &key, const string_t &min,
                        const string_t &max, bool limit, size_t offset,
                        size_t count, bool with_scores,
                        const reply_callback_t &reply_callback);

  //!
  //!  zrevrangebylex impl
  //!
  client &zrevrangebylex(const string_t &key, const string_t &max,
                         const string_t &min, bool limit, size_t offset,
                         size_t count, bool with_scores,
                         const reply_callback_t &reply_callback);

  //!
  //!  zrangebylex impl
  //!
  client &zrangebylex(const string_t &key, const string_t &min,
                      const string_t &max, bool limit, size_t offset,
                      size_t count, bool with_scores,
                      const reply_callback_t &reply_callback);

private:
  //!
  //!  redis connection receive handler, triggered whenever a reply has been
  //!  read by the redis connection
  //!
  //!  @param connection redis_connection instance
  //!  @param reply parsed reply
  //!
  void connection_receive_handler(redis_connection_t &connection, reply &reply);

  //!
  //!  reset the queue of pending callbacks
  //!
  void clear_callbacks();

  //!
  //!  try to commit the pending pipelined
  //!  if client is disconnected, will throw an exception and clear all pending
  //!  callbacks (call clear_callbacks())
  //!
  void try_commit();

  //!
  //!  Execute a command on the client and tie the callback to a future
  //!
  future_reply_t
  exec_cmd(const function<client &(const reply_callback_t &)> &f);

private:
  //!
  //!  struct to store commands information (command to be sent and callback to
  //!  be called)
  //!
  struct command_request {
    vector<string_t> command;
    reply_callback_t callback;
  };

  using command_request_t = command_request;

  using command_request_queue_t = queue<command_request_t>;

protected:
  //!
  //!  server we are connected to
  //!
  string_t m_redis_server;
  //!
  //!  port we are connected to
  //!
  size_t m_redis_port = 0;
  //!
  //!  master name (if we are using sentinel) we are connected to
  //!
  string_t m_master_name;
  //!
  //!  password used to authenticate
  //!
  string_t m_password;
  //!
  //!  selected redis db
  //!
  int m_database_index = 0;

  //!
  //!  tcp client for redis connection
  //!
  redis_connection_t m_client;

  //!
  //!  redis sentinel
  //!
  sentinel_t m_sentinel;

  //!
  //!  max time to connect
  //!
  uint_t m_connect_timeout_ms = 0;
  //!
  //!  max number of reconnection attempts
  //!
  int_t m_max_reconnects = 0;
  //!
  //!  current number of attempts to reconnect
  //!
  int_t m_current_reconnect_attempts = 0;
  //!
  //!  time between two reconnection attempts
  //!
  uint_t m_reconnect_interval_ms = 0;

  //!
  //!  reconnection status
  //!
  atomic_bool m_reconnecting;
  //!
  //!  to force cancel reconnection
  //!
  atomic_bool m_cancel;

  //!
  //!  sent commands waiting to be executed
  //!
  command_request_queue_t m_commands;

  //!
  //!  user defined connect status callback
  //!
  connect_callback_t m_connect_callback;

  //!
  //!   callbacks thread safety
  //!
  mutex_t m_callbacks_mutex;

  //!
  //!  cond var for callbacks updates
  //!
  condition_variable_t m_sync_cond_var;

  //!
  //!  number of callbacks currently being running
  //!
  atomic_uint m_callbacks_running;
}; // class client

using client_t = client;

using client_ptr_t = unique_ptr<client_t>;

} // namespace cpp_redis

#include <cpp_redis/impl/client.ipp>

#endif
