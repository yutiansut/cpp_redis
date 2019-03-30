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
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
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

#include <cpp_redis/core/sentinel.hpp>
#include <cpp_redis/core/types.hpp>
#include <cpp_redis/helpers/variadic_template.hpp>
#include <cpp_redis/misc/logger.hpp>
#include <cpp_redis/network/redis_connection.hpp>
#include <cpp_redis/network/tcp_client_iface.hpp>

namespace cpp_redis {

//!
//!  cpp_redis::client is the class providing communication with a Redis server.
//!  It is meant to be used for sending commands to the remote server and
//!  receiving its replies. The client support asynchronous requests, as well as
//!  synchronous ones. Moreover, commands pipelining is supported.
//!
//!
class client {
public:
  //!
  //!  client type
  //!  used for client kill
  //!
  //!
  enum class client_type { normal, master, pubsub, slave, cluster };

public:
#ifndef __CPP_REDIS_USE_CUSTOM_TCP_CLIENT

  //!
  //!  ctor
  //!
  //!
  client();

#endif //!  __CPP_REDIS_USE_CUSTOM_TCP_CLIENT //!

  //!
  //!  custom ctor to specify custom tcp_client
  //!
  //!  @param tcp_client tcp client to be used for network communications
  //!
  //!
  explicit client(const std::shared_ptr<network::tcp_client_iface> &tcp_client);

  //!
  //!  dtor
  //!
  //!
  ~client();

  //!
  //!  copy ctor
  //!
  //!
  client(const client_t &) = delete;

  //!
  //!  assignment operator
  //!
  //!
  client_t &
  operator=(const client_t &) = delete;

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
  //!
  void
  connect(const std::string &host = "127.0.0.1",
	  std::size_t port = 6379,
	  const connect_callback_t &connect_callback = nullptr,
	  std::uint32_t timeout_ms = 0,
	  std::int32_t max_reconnects = 0,
	  std::uint32_t reconnect_interval_ms = 0);

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
  //!
  void
  connect(const std::string &name,
	  const connect_callback_t &connect_callback = nullptr,
	  std::uint32_t timeout_ms = 0,
	  std::int32_t max_reconnects = 0,
	  std::uint32_t reconnect_interval_ms = 0);

  //!
  //!  @return whether we are connected to the redis server
  //!
  //!
  bool
  is_connected() const;

  //!
  //!  disconnect from redis server
  //!
  //!  @param wait_for_removal when sets to true, disconnect blocks until the
  //!  underlying TCP client has been effectively removed from the io_service
  //!  and that all the underlying callbacks have completed.
  //!
  //!
  void
  disconnect(bool wait_for_removal = false);

  //!
  //!  @return whether an attempt to reconnect is in progress
  //!
  //!
  bool
  is_reconnecting() const;

  //!
  //!  stop any reconnect in progress
  //!
  //!
  void
  cancel_reconnect();

public:
  //!
  //!  reply callback called whenever a reply is received
  //!  takes as parameter the received reply
  //!
  //!
  using reply_callback_t = std::function<void(reply &)>;

  //!
  //!  send the given command
  //!  the command is actually pipelined and only buffered, so nothing is sent
  //!  to the network please call commit() / sync_commit() to flush the buffer
  //!
  //!  @param redis_cmd command to be sent
  //!  @param callback callback to be called on received reply
  //!  @return current instance
  //!
  //!
  client_t &
  send(const std::vector<std::string> &redis_cmd,
       const reply_callback_t &callback);

  //!
  //!  same as the other send method
  //!  but future based: does not take any callback and return an std:;future to
  //!  handle the reply
  //!
  //!  @param redis_cmd command to be sent
  //!  @return std::future to handler redis reply
  //!
  //!
  std::future<reply_t>
  send(const std::vector<std::string> &redis_cmd);

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
  //!
  client_t &
  commit();

  //!
  //!  same as commit(), but synchronous
  //!  will block until all pending commands have been sent and that a reply has
  //!  been received for each of them and all underlying callbacks completed
  //!
  //!  @return current instance
  //!
  //!
  client_t &
  sync_commit();

  //!
  //!  same as sync_commit, but with a timeout
  //!  will simply block until it completes or timeout expires
  //!
  //!  @return current instance
  //!
  //!
  template <class Rep, class Period>
  client_t &
  sync_commit(const std::chrono::duration<Rep, Period> &timeout) {
    //!
    //!  no need to call commit in case of reconnection
    //!  the reconnection flow will do it for us
    //!
    //!
    if (!is_reconnecting()) {
      try_commit();
    }

    std::unique_lock<std::mutex> lock_callback(m_callbacks_mutex);
    __CPP_REDIS_LOG(debug,
		    "cpp_redis::client waiting for callbacks to complete");
    if (!m_sync_condvar.wait_for(lock_callback, timeout, [=] {
	  return m_callbacks_running == 0 && m_commands.empty();
	})) {
      __CPP_REDIS_LOG(debug, "cpp_redis::client finished waiting for callback");
    } else {
      __CPP_REDIS_LOG(debug,
		      "cpp_redis::client timed out waiting for callback");
    }

    return *this;
  }

private:
  //!
  //!  @return whether a reconnection attempt should be performed
  //!
  //!
  bool
  should_reconnect() const;

  //!
  //!  resend all pending commands that failed to be sent due to disconnection
  //!
  //!
  void
  resend_failed_commands();

  //!
  //!  sleep between two reconnect attempts if necessary
  //!
  //!
  void
  sleep_before_next_reconnect_attempt();

  //!
  //!  reconnect to the previously connected host
  //!  automatically re authenticate and resubscribe to subscribed channel in
  //!  case of success
  //!
  //!
  void
  reconnect();

  //!
  //!  re authenticate to redis server based on previously used password
  //!
  //!
  void
  re_auth();

  //!
  //!  re select db to redis server based on previously selected db
  //!
  //!
  void
  re_select();

private:
  //!
  //!  unprotected send
  //!  same as send, but without any mutex lock
  //!
  //!  @param redis_cmd cmd to be sent
  //!  @param callback callback to be called whenever a reply is received
  //!
  //!
  void
  unprotected_send(const std::vector<std::string> &redis_cmd,
		   const reply_callback_t &callback);

  //!
  //!  unprotected auth
  //!  same as auth, but without any mutex lock
  //!
  //!  @param password password to be used for authentication
  //!  @param reply_callback callback to be called whenever a reply is received
  //!
  //!
  void
  unprotected_auth(const std::string &password,
		   const reply_callback_t &reply_callback);

  //!
  //!  unprotected select
  //!  same as select, but without any mutex lock
  //!
  //!  @param index index to be used for db select
  //!  @param reply_callback callback to be called whenever a reply is received
  //!
  //!
  void
  unprotected_select(int index, const reply_callback_t &reply_callback);

public:
  //!
  //!  add a sentinel definition. Required for connect() or
  //!  get_master_addr_by_name() when autoconnect is enabled.
  //!
  //!  @param host sentinel host
  //!  @param port sentinel port
  //!  @param timeout_ms maximum time to connect
  //!
  //!
  void
  add_sentinel(const std::string &host,
	       std::size_t port,
	       std::uint32_t timeout_ms = 0);

  //!
  //!  retrieve sentinel for current client
  //!
  //!  @return sentinel associated to current client
  //!
  //!
  const sentinel &
  get_sentinel() const;

  //!
  //!  retrieve sentinel for current client
  //!  non-const version
  //!
  //!  @return sentinel associated to current client
  //!
  //!
  sentinel &
  get_sentinel();

  //!
  //!  clear all existing sentinels.
  //!
  //!
  void
  clear_sentinels();

public:
  //!
  //!  aggregate method to be used for some commands (like zunionstore)
  //!  these match the aggregate methods supported by redis
  //!  use server_default if you are not willing to specify this parameter and
  //!  let the server defaults
  //!
  //!
  enum class aggregate_method { sum, min, max, server_default };

  //!
  //!  convert an aggregate_method enum to its equivalent redis-server string
  //!
  //!  @param method aggregate_method to convert
  //!  @return conversion
  //!
  //!
  std::string
  aggregate_method_to_string(aggregate_method method) const;

public:
  //!
  //!  geographic unit to be used for some commands (like georadius)
  //!  these match the geo units supported by redis-server
  //!
  //!
  enum class geo_unit { m, km, ft, mi };

  //!
  //!  convert a geo unit to its equivalent redis-server string
  //!
  //!  @param unit geo_unit to convert
  //!  @return conversion
  //!
  //!
  std::string
  geo_unit_to_string(geo_unit unit) const;

public:
  //!
  //!  overflow type to be used for some commands (like bitfield)
  //!  these match the overflow types supported by redis-server
  //!  use server_default if you are not willing to specify this parameter and
  //!  let the server defaults
  //!
  //!
  enum class overflow_type { wrap, sat, fail, server_default };

  //!
  //!  convert an overflow type to its equivalent redis-server string
  //!
  //!  @param type overflow type to convert
  //!  @return conversion
  //!
  //!
  std::string
  overflow_type_to_string(overflow_type type) const;

public:
  //!
  //!  bitfield operation type to be used for some commands (like bitfield)
  //!  these match the bitfield operation types supported by redis-server
  //!
  //!
  enum class bitfield_operation_type { get, set, incrby };

  //!
  //!  convert a bitfield operation type to its equivalent redis-server string
  //!
  //!  @param operation operation type to convert
  //!  @return conversion
  //!
  //!
  std::string
  bitfield_operation_type_to_string(bitfield_operation_type operation) const;

public:
  //!
  //!  used to store a get, set or incrby bitfield operation (for bitfield
  //!  command)
  //!
  //!
  struct bitfield_operation {
    //!
    //!  operation type (get, set, incrby)
    //!
    //!
    bitfield_operation_type operation_type;

    //!
    //!  redis type parameter for get, set or incrby operations
    //!
    //!
    std::string type;

    //!
    //!  redis offset parameter for get, set or incrby operations
    //!
    //!
    int offset;

    //!
    //!  redis value parameter for set operation, or increment parameter for
    //!  incrby operation
    //!
    //!
    int value;

    //!
    //!  overflow optional specification
    //!
    //!
    overflow_type overflow;

    //!
    //!  build a bitfield_operation for a bitfield get operation
    //!
    //!  @param type type param of a get operation
    //!  @param offset offset param of a get operation
    //!  @param overflow overflow specification (leave to server_default if you
    //!  do not want to specify it)
    //!  @return corresponding get bitfield_operation
    //!
    //!
    static bitfield_operation
    get(const std::string &type,
	int offset,
	overflow_type overflow = overflow_type::server_default);

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
    //!
    static bitfield_operation
    set(const std::string &type,
	int offset,
	int value,
	overflow_type overflow = overflow_type::server_default);

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
    //!
    static bitfield_operation
    incrby(const std::string &type,
	   int offset,
	   int increment,
	   overflow_type overflow = overflow_type::server_default);
  };

public:
  client_t &
  append(const std::string &key,
	 const std::string &value,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  append(const std::string &key, const std::string &value);

  client_t &
  auth(const std::string &password, const reply_callback_t &reply_callback);

  std::future<reply_t>
  auth(const std::string &password);

  client_t &
  bgrewriteaof(const reply_callback_t &reply_callback);

  std::future<reply_t>
  bgrewriteaof();

  client_t &
  bgsave(const reply_callback_t &reply_callback);

  std::future<reply_t>
  bgsave();

  client_t &
  bitcount(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  bitcount(const std::string &key);

  client_t &
  bitcount(const std::string &key,
	   int start,
	   int end,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  bitcount(const std::string &key, int start, int end);

  client_t &
  bitfield(const std::string &key,
	   const std::vector<bitfield_operation> &operations,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  bitfield(const std::string &key,
	   const std::vector<bitfield_operation> &operations);

  client_t &
  bitop(const std::string &operation,
	const std::string &destkey,
	const std::vector<std::string> &keys,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  bitop(const std::string &operation,
	const std::string &destkey,
	const std::vector<std::string> &keys);

  client_t &
  bitpos(const std::string &key,
	 int bit,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  bitpos(const std::string &key, int bit);

  client_t &
  bitpos(const std::string &key,
	 int bit,
	 int start,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  bitpos(const std::string &key, int bit, int start);

  client_t &
  bitpos(const std::string &key,
	 int bit,
	 int start,
	 int end,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  bitpos(const std::string &key, int bit, int start, int end);

  client_t &
  blpop(const std::vector<std::string> &keys,
	int timeout,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  blpop(const std::vector<std::string> &keys, int timeout);

  client_t &
  brpop(const std::vector<std::string> &keys,
	int timeout,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  brpop(const std::vector<std::string> &keys, int timeout);

  client_t &
  brpoplpush(const std::string &src,
	     const std::string &dst,
	     int timeout,
	     const reply_callback_t &reply_callback);

  std::future<reply_t>
  brpoplpush(const std::string &src, const std::string &dst, int timeout);

  client_t &
  bzpopmin(const std::vector<std::string> &keys,
	   int timeout,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  bzpopmin(const std::vector<std::string> &keys, int timeout);

  client_t &
  bzpopmax(const std::vector<std::string> &keys,
	   int timeout,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  bzpopmax(const std::vector<std::string> &keys, int timeout);

  client_t &
  client_id(const reply_callback_t &reply_callback);

  std::future<reply_t>
  client_id();

  //<editor-fold desc="client">
  template <typename T, typename... Ts>
  client_t &
  client_kill(const std::string &host,
	      int port,
	      const T &arg,
	      const Ts &... args);

  client_t &
  client_kill(const std::string &host, int port);

  template <typename... Ts>
  client_t &
  client_kill(const char *host, int port, const Ts &... args);

  template <typename T, typename... Ts>
  client_t &
  client_kill(const T &, const Ts &...);

  template <typename T, typename... Ts>
  std::future<reply_t>
  client_kill_future(T, const Ts...);

  client_t &
  client_list(const reply_callback_t &reply_callback);

  std::future<reply_t>
  client_list();

  client_t &
  client_getname(const reply_callback_t &reply_callback);

  std::future<reply_t>
  client_getname();

  client_t &
  client_pause(int timeout, const reply_callback_t &reply_callback);

  std::future<reply_t>
  client_pause(int timeout);

  client_t &
  client_reply(const std::string &mode, const reply_callback_t &reply_callback);

  std::future<reply_t>
  client_reply(const std::string &mode);

  client_t &
  client_setname(const std::string &name,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  client_setname(const std::string &name);
  //</editor-fold>

  client_t &
  client_unblock(int id, const reply_callback_t &reply_callback);

  client_t &
  client_unblock(int id,
		 bool witherror,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  client_unblock(int id, bool witherror = false);

  client_t &
  cluster_addslots(const std::vector<std::string> &p_slots,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_addslots(const std::vector<std::string> &p_slots);

  client_t &
  cluster_count_failure_reports(const std::string &node_id,
				const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_count_failure_reports(const std::string &node_id);

  client_t &
  cluster_countkeysinslot(const std::string &slot,
			  const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_countkeysinslot(const std::string &slot);

  client_t &
  cluster_delslots(const std::vector<std::string> &p_slots,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_delslots(const std::vector<std::string> &p_slots);

  client_t &
  cluster_failover(const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_failover();

  client_t &
  cluster_failover(const std::string &mode,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_failover(const std::string &mode);

  client_t &
  cluster_forget(const std::string &node_id,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_forget(const std::string &node_id);

  client_t &
  cluster_getkeysinslot(const std::string &slot,
			int count,
			const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_getkeysinslot(const std::string &slot, int count);

  client_t &
  cluster_info(const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_info();

  client_t &
  cluster_keyslot(const std::string &key,
		  const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_keyslot(const std::string &key);

  client_t &
  cluster_meet(const std::string &ip,
	       int port,
	       const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_meet(const std::string &ip, int port);

  client_t &
  cluster_nodes(const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_nodes();

  client_t &
  cluster_replicate(const std::string &node_id,
		    const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_replicate(const std::string &node_id);

  client_t &
  cluster_reset(const reply_callback_t &reply_callback);

  client_t &
  cluster_reset(const std::string &mode,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_reset(const std::string &mode = "soft");

  client_t &
  cluster_saveconfig(const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_saveconfig();

  client_t &
  cluster_set_config_epoch(const std::string &epoch,
			   const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_set_config_epoch(const std::string &epoch);

  client_t &
  cluster_setslot(const std::string &slot,
		  const std::string &mode,
		  const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_setslot(const std::string &slot, const std::string &mode);

  client_t &
  cluster_setslot(const std::string &slot,
		  const std::string &mode,
		  const std::string &node_id,
		  const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_setslot(const std::string &slot,
		  const std::string &mode,
		  const std::string &node_id);

  client_t &
  cluster_slaves(const std::string &node_id,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_slaves(const std::string &node_id);

  client_t &
  cluster_slots(const reply_callback_t &reply_callback);

  std::future<reply_t>
  cluster_slots();

  client_t &
  command(const reply_callback_t &reply_callback);

  std::future<reply_t>
  command();

  client_t &
  command_count(const reply_callback_t &reply_callback);

  std::future<reply_t>
  command_count();

  client_t &
  command_getkeys(const reply_callback_t &reply_callback);

  std::future<reply_t>
  command_getkeys();

  client_t &
  command_info(const std::vector<std::string> &command_name,
	       const reply_callback_t &reply_callback);

  std::future<reply_t>
  command_info(const std::vector<std::string> &command_name);

  client_t &
  config_get(const std::string &param, const reply_callback_t &reply_callback);

  std::future<reply_t>
  config_get(const std::string &param);

  client_t &
  config_rewrite(const reply_callback_t &reply_callback);

  std::future<reply_t>
  config_rewrite();

  client_t &
  config_set(const std::string &param,
	     const std::string &val,
	     const reply_callback_t &reply_callback);

  std::future<reply_t>
  config_set(const std::string &param, const std::string &val);

  client_t &
  config_resetstat(const reply_callback_t &reply_callback);

  std::future<reply_t>
  config_resetstat();

  client_t &
  dbsize(const reply_callback_t &reply_callback);

  std::future<reply_t>
  dbsize();

  client_t &
  debug_object(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  debug_object(const std::string &key);

  client_t &
  debug_segfault(const reply_callback_t &reply_callback);

  std::future<reply_t>
  debug_segfault();

  client_t &
  decr(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  decr(const std::string &key);

  client_t &
  decrby(const std::string &key,
	 int val,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  decrby(const std::string &key, int val);

  client_t &
  del(const std::vector<std::string> &key,
      const reply_callback_t &reply_callback);

  std::future<reply_t>
  del(const std::vector<std::string> &key);

  client_t &
  discard(const reply_callback_t &reply_callback);

  std::future<reply_t>
  discard();

  client_t &
  dump(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  dump(const std::string &key);

  client_t &
  echo(const std::string &msg, const reply_callback_t &reply_callback);

  std::future<reply_t>
  echo(const std::string &msg);

  client_t &
  eval(const std::string &script,
       int numkeys,
       const std::vector<std::string> &keys,
       const std::vector<std::string> &args,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  eval(const std::string &script,
       int numkeys,
       const std::vector<std::string> &keys,
       const std::vector<std::string> &args);

  client_t &
  evalsha(const std::string &sha1,
	  int numkeys,
	  const std::vector<std::string> &keys,
	  const std::vector<std::string> &args,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  evalsha(const std::string &sha1,
	  int numkeys,
	  const std::vector<std::string> &keys,
	  const std::vector<std::string> &args);

  client_t &
  exec(const reply_callback_t &reply_callback);

  std::future<reply_t>
  exec();

  client_t &
  exists(const std::vector<std::string> &keys,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  exists(const std::vector<std::string> &keys);

  client_t &
  expire(const std::string &key,
	 int seconds,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  expire(const std::string &key, int seconds);

  client_t &
  expireat(const std::string &key,
	   int timestamp,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  expireat(const std::string &key, int timestamp);

  client_t &
  flushall(const reply_callback_t &reply_callback);

  std::future<reply_t>
  flushall();

  client_t &
  flushdb(const reply_callback_t &reply_callback);

  std::future<reply_t>
  flushdb();

  client_t &
  geoadd(const std::string &key,
	 const std::vector<std::tuple<std::string, std::string, std::string>>
	     &long_lat_memb,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  geoadd(const std::string &key,
	 const std::vector<std::tuple<std::string, std::string, std::string>>
	     &long_lat_memb);

  client_t &
  geohash(const std::string &key,
	  const std::vector<std::string> &members,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  geohash(const std::string &key, const std::vector<std::string> &members);

  client_t &
  geopos(const std::string &key,
	 const std::vector<std::string> &members,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  geopos(const std::string &key, const std::vector<std::string> &members);

  client_t &
  geodist(const std::string &key,
	  const std::string &member_1,
	  const std::string &member_2,
	  const reply_callback_t &reply_callback);

  client_t &
  geodist(const std::string &key,
	  const std::string &member_1,
	  const std::string &member_2,
	  const std::string &unit,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  geodist(const std::string &key,
	  const std::string &member_1,
	  const std::string &member_2,
	  const std::string &unit = "m_cv_mutex");

  client_t &
  georadius(const std::string &key,
	    double longitude,
	    double latitude,
	    double radius,
	    geo_unit unit,
	    bool with_coord,
	    bool with_dist,
	    bool with_hash,
	    bool asc_order,
	    const reply_callback_t &reply_callback);

  client_t &
  georadius(const std::string &key,
	    double longitude,
	    double latitude,
	    double radius,
	    geo_unit unit,
	    bool with_coord,
	    bool with_dist,
	    bool with_hash,
	    bool asc_order,
	    std::size_t count,
	    const reply_callback_t &reply_callback);

  client_t &
  georadius(const std::string &key,
	    double longitude,
	    double latitude,
	    double radius,
	    geo_unit unit,
	    bool with_coord,
	    bool with_dist,
	    bool with_hash,
	    bool asc_order,
	    const std::string &store_key,
	    const reply_callback_t &reply_callback);

  client_t &
  georadius(const std::string &key,
	    double longitude,
	    double latitude,
	    double radius,
	    geo_unit unit,
	    bool with_coord,
	    bool with_dist,
	    bool with_hash,
	    bool asc_order,
	    const std::string &store_key,
	    const std::string &storedist_key,
	    const reply_callback_t &reply_callback);

  client_t &
  georadius(const std::string &key,
	    double longitude,
	    double latitude,
	    double radius,
	    geo_unit unit,
	    bool with_coord,
	    bool with_dist,
	    bool with_hash,
	    bool asc_order,
	    std::size_t count,
	    const std::string &store_key,
	    const reply_callback_t &reply_callback);

  client_t &
  georadius(const std::string &key,
	    double longitude,
	    double latitude,
	    double radius,
	    geo_unit unit,
	    bool with_coord,
	    bool with_dist,
	    bool with_hash,
	    bool asc_order,
	    std::size_t count,
	    const std::string &store_key,
	    const std::string &storedist_key,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  georadius(const std::string &key,
	    double longitude,
	    double latitude,
	    double radius,
	    geo_unit unit,
	    bool with_coord = false,
	    bool with_dist = false,
	    bool with_hash = false,
	    bool asc_order = false,
	    std::size_t count = 0,
	    const std::string &store_key = "",
	    const std::string &storedist_key = "");

  client_t &
  georadiusbymember(const std::string &key,
		    const std::string &member,
		    double radius,
		    geo_unit unit,
		    bool with_coord,
		    bool with_dist,
		    bool with_hash,
		    bool asc_order,
		    const reply_callback_t &reply_callback);

  client_t &
  georadiusbymember(const std::string &key,
		    const std::string &member,
		    double radius,
		    geo_unit unit,
		    bool with_coord,
		    bool with_dist,
		    bool with_hash,
		    bool asc_order,
		    std::size_t count,
		    const reply_callback_t &reply_callback);

  client_t &
  georadiusbymember(const std::string &key,
		    const std::string &member,
		    double radius,
		    geo_unit unit,
		    bool with_coord,
		    bool with_dist,
		    bool with_hash,
		    bool asc_order,
		    const std::string &store_key,
		    const reply_callback_t &reply_callback);

  client_t &
  georadiusbymember(const std::string &key,
		    const std::string &member,
		    double radius,
		    geo_unit unit,
		    bool with_coord,
		    bool with_dist,
		    bool with_hash,
		    bool asc_order,
		    const std::string &store_key,
		    const std::string &storedist_key,
		    const reply_callback_t &reply_callback);

  client_t &
  georadiusbymember(const std::string &key,
		    const std::string &member,
		    double radius,
		    geo_unit unit,
		    bool with_coord,
		    bool with_dist,
		    bool with_hash,
		    bool asc_order,
		    std::size_t count,
		    const std::string &store_key,
		    const reply_callback_t &reply_callback);

  client_t &
  georadiusbymember(const std::string &key,
		    const std::string &member,
		    double radius,
		    geo_unit unit,
		    bool with_coord,
		    bool with_dist,
		    bool with_hash,
		    bool asc_order,
		    std::size_t count,
		    const std::string &store_key,
		    const std::string &storedist_key,
		    const reply_callback_t &reply_callback);

  std::future<reply_t>
  georadiusbymember(const std::string &key,
		    const std::string &member,
		    double radius,
		    geo_unit unit,
		    bool with_coord = false,
		    bool with_dist = false,
		    bool with_hash = false,
		    bool asc_order = false,
		    std::size_t count = 0,
		    const std::string &store_key = "",
		    const std::string &storedist_key = "");

  client_t &
  get(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  get(const std::string &key);

  client_t &
  getbit(const std::string &key,
	 int offset,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  getbit(const std::string &key, int offset);

  client_t &
  getrange(const std::string &key,
	   int start,
	   int end,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  getrange(const std::string &key, int start, int end);

  client_t &
  getset(const std::string &key,
	 const std::string &val,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  getset(const std::string &key, const std::string &val);

  client_t &
  hdel(const std::string &key,
       const std::vector<std::string> &fields,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  hdel(const std::string &key, const std::vector<std::string> &fields);

  client_t &
  hexists(const std::string &key,
	  const std::string &field,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  hexists(const std::string &key, const std::string &field);

  client_t &
  hget(const std::string &key,
       const std::string &field,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  hget(const std::string &key, const std::string &field);

  client_t &
  hgetall(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  hgetall(const std::string &key);

  client_t &
  hincrby(const std::string &key,
	  const std::string &field,
	  int incr,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  hincrby(const std::string &key, const std::string &field, int incr);

  client_t &
  hincrbyfloat(const std::string &key,
	       const std::string &field,
	       float incr,
	       const reply_callback_t &reply_callback);

  std::future<reply_t>
  hincrbyfloat(const std::string &key, const std::string &field, float incr);

  client_t &
  hkeys(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  hkeys(const std::string &key);

  client_t &
  hlen(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  hlen(const std::string &key);

  client_t &
  hmget(const std::string &key,
	const std::vector<std::string> &fields,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  hmget(const std::string &key, const std::vector<std::string> &fields);

  client_t &
  hmset(const std::string &key,
	const std::vector<std::pair<std::string, std::string>> &field_val,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  hmset(const std::string &key,
	const std::vector<std::pair<std::string, std::string>> &field_val);

  client_t &
  hscan(const std::string &key,
	std::size_t cursor,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  hscan(const std::string &key, std::size_t cursor);

  client_t &
  hscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  hscan(const std::string &key, std::size_t cursor, const std::string &pattern);

  client_t &
  hscan(const std::string &key,
	std::size_t cursor,
	std::size_t count,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  hscan(const std::string &key, std::size_t cursor, std::size_t count);

  client_t &
  hscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	std::size_t count,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  hscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	std::size_t count);

  client_t &
  hset(const std::string &key,
       const std::string &field,
       const std::string &value,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  hset(const std::string &key,
       const std::string &field,
       const std::string &value);

  client_t &
  hsetnx(const std::string &key,
	 const std::string &field,
	 const std::string &value,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  hsetnx(const std::string &key,
	 const std::string &field,
	 const std::string &value);

  client_t &
  hstrlen(const std::string &key,
	  const std::string &field,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  hstrlen(const std::string &key, const std::string &field);

  client_t &
  hvals(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  hvals(const std::string &key);

  client_t &
  incr(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  incr(const std::string &key);

  client_t &
  incrby(const std::string &key,
	 int incr,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  incrby(const std::string &key, int incr);

  client_t &
  incrbyfloat(const std::string &key,
	      float incr,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  incrbyfloat(const std::string &key, float incr);

  client_t &
  info(const reply_callback_t &reply_callback);

  client_t &
  info(const std::string &section, const reply_callback_t &reply_callback);

  std::future<reply_t>
  info(const std::string &section = "default");

  client_t &
  keys(const std::string &pattern, const reply_callback_t &reply_callback);

  std::future<reply_t>
  keys(const std::string &pattern);

  client_t &
  lastsave(const reply_callback_t &reply_callback);

  std::future<reply_t>
  lastsave();

  client_t &
  lindex(const std::string &key,
	 int index,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  lindex(const std::string &key, int index);

  client_t &
  linsert(const std::string &key,
	  const std::string &before_after,
	  const std::string &pivot,
	  const std::string &value,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  linsert(const std::string &key,
	  const std::string &before_after,
	  const std::string &pivot,
	  const std::string &value);

  client_t &
  llen(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  llen(const std::string &key);

  client_t &
  lpop(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  lpop(const std::string &key);

  client_t &
  lpush(const std::string &key,
	const std::vector<std::string> &values,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  lpush(const std::string &key, const std::vector<std::string> &values);

  client_t &
  lpushx(const std::string &key,
	 const std::string &value,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  lpushx(const std::string &key, const std::string &value);

  client_t &
  lrange(const std::string &key,
	 int start,
	 int stop,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  lrange(const std::string &key, int start, int stop);

  client_t &
  lrem(const std::string &key,
       int count,
       const std::string &value,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  lrem(const std::string &key, int count, const std::string &value);

  client_t &
  lset(const std::string &key,
       int index,
       const std::string &value,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  lset(const std::string &key, int index, const std::string &value);

  client_t &
  ltrim(const std::string &key,
	int start,
	int stop,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  ltrim(const std::string &key, int start, int stop);

  client_t &
  mget(const std::vector<std::string> &keys,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  mget(const std::vector<std::string> &keys);

  client_t &
  migrate(const std::string &host,
	  int port,
	  const std::string &key,
	  const std::string &dest_db,
	  int timeout,
	  const reply_callback_t &reply_callback);

  client_t &
  migrate(const std::string &host,
	  int port,
	  const std::string &key,
	  const std::string &dest_db,
	  int timeout,
	  bool copy,
	  bool replace,
	  const std::vector<std::string> &keys,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  migrate(const std::string &host,
	  int port,
	  const std::string &key,
	  const std::string &dest_db,
	  int timeout,
	  bool copy = false,
	  bool replace = false,
	  const std::vector<std::string> &keys = {});

  client_t &
  monitor(const reply_callback_t &reply_callback);

  std::future<reply_t>
  monitor();

  client_t &
  move(const std::string &key,
       const std::string &db,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  move(const std::string &key, const std::string &db);

  client_t &
  mset(const std::vector<std::pair<std::string, std::string>> &key_vals,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  mset(const std::vector<std::pair<std::string, std::string>> &key_vals);

  client_t &
  msetnx(const std::vector<std::pair<std::string, std::string>> &key_vals,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  msetnx(const std::vector<std::pair<std::string, std::string>> &key_vals);

  client_t &
  multi(const reply_callback_t &reply_callback);

  std::future<reply_t>
  multi();

  client_t &
  object(const std::string &subcommand,
	 const std::vector<std::string> &args,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  object(const std::string &subcommand, const std::vector<std::string> &args);

  client_t &
  persist(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  persist(const std::string &key);

  client_t &
  pexpire(const std::string &key,
	  int ms,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  pexpire(const std::string &key, int ms);

  client_t &
  pexpireat(const std::string &key,
	    int ms_timestamp,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  pexpireat(const std::string &key, int ms_timestamp);

  client_t &
  pfadd(const std::string &key,
	const std::vector<std::string> &elements,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  pfadd(const std::string &key, const std::vector<std::string> &elements);

  client_t &
  pfcount(const std::vector<std::string> &keys,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  pfcount(const std::vector<std::string> &keys);

  client_t &
  pfmerge(const std::string &destkey,
	  const std::vector<std::string> &sourcekeys,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  pfmerge(const std::string &destkey,
	  const std::vector<std::string> &sourcekeys);

  client_t &
  ping(const reply_callback_t &reply_callback);

  std::future<reply_t>
  ping();

  client_t &
  ping(const std::string &message, const reply_callback_t &reply_callback);

  std::future<reply_t>
  ping(const std::string &message);

  client_t &
  psetex(const std::string &key,
	 int64_t ms,
	 const std::string &val,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  psetex(const std::string &key, int64_t ms, const std::string &val);

  client_t &
  publish(const std::string &channel,
	  const std::string &message,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  publish(const std::string &channel, const std::string &message);

  client_t &
  pubsub(const std::string &subcommand,
	 const std::vector<std::string> &args,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  pubsub(const std::string &subcommand, const std::vector<std::string> &args);

  client_t &
  pttl(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  pttl(const std::string &key);

  client_t &
  quit(const reply_callback_t &reply_callback);

  std::future<reply_t>
  quit();

  client_t &
  randomkey(const reply_callback_t &reply_callback);

  std::future<reply_t>
  randomkey();

  client_t &
  readonly(const reply_callback_t &reply_callback);

  std::future<reply_t>
  readonly();

  client_t &
  readwrite(const reply_callback_t &reply_callback);

  std::future<reply_t>
  readwrite();

  client_t &
  rename(const std::string &key,
	 const std::string &newkey,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  rename(const std::string &key, const std::string &newkey);

  client_t &
  renamenx(const std::string &key,
	   const std::string &newkey,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  renamenx(const std::string &key, const std::string &newkey);

  client_t &
  restore(const std::string &key,
	  int ttl,
	  const std::string &serialized_value,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  restore(const std::string &key, int ttl, const std::string &serialized_value);

  client_t &
  restore(const std::string &key,
	  int ttl,
	  const std::string &serialized_value,
	  const std::string &replace,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  restore(const std::string &key,
	  int ttl,
	  const std::string &serialized_value,
	  const std::string &replace);

  client_t &
  role(const reply_callback_t &reply_callback);

  std::future<reply_t>
  role();

  client_t &
  rpop(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  rpop(const std::string &key);

  client_t &
  rpoplpush(const std::string &source,
	    const std::string &destination,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  rpoplpush(const std::string &src, const std::string &dst);

  client_t &
  rpush(const std::string &key,
	const std::vector<std::string> &values,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  rpush(const std::string &key, const std::vector<std::string> &values);

  client_t &
  rpushx(const std::string &key,
	 const std::string &value,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  rpushx(const std::string &key, const std::string &value);

  client_t &
  sadd(const std::string &key,
       const std::vector<std::string> &members,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sadd(const std::string &key, const std::vector<std::string> &members);

  client_t &
  save(const reply_callback_t &reply_callback);

  std::future<reply_t>
  save();

  client_t &
  scan(std::size_t cursor, const reply_callback_t &reply_callback);

  std::future<reply_t>
  scan(std::size_t cursor);

  client_t &
  scan(std::size_t cursor,
       const std::string &pattern,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  scan(std::size_t cursor, const std::string &pattern);

  client_t &
  scan(std::size_t cursor,
       std::size_t count,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  scan(std::size_t cursor, std::size_t count);

  client_t &
  scan(std::size_t cursor,
       const std::string &pattern,
       std::size_t count,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  scan(std::size_t cursor, const std::string &pattern, std::size_t count);

  client_t &
  scard(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  scard(const std::string &key);

  client_t &
  script_debug(const std::string &mode, const reply_callback_t &reply_callback);

  std::future<reply_t>
  script_debug(const std::string &mode);

  client_t &
  script_exists(const std::vector<std::string> &scripts,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  script_exists(const std::vector<std::string> &scripts);

  client_t &
  script_flush(const reply_callback_t &reply_callback);

  std::future<reply_t>
  script_flush();

  client_t &
  script_kill(const reply_callback_t &reply_callback);

  std::future<reply_t>
  script_kill();

  client_t &
  script_load(const std::string &script,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  script_load(const std::string &script);

  client_t &
  sdiff(const std::vector<std::string> &keys,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  sdiff(const std::vector<std::string> &keys);

  client_t &
  sdiffstore(const std::string &destination,
	     const std::vector<std::string> &keys,
	     const reply_callback_t &reply_callback);

  std::future<reply_t>
  sdiffstore(const std::string &dst, const std::vector<std::string> &keys);

  client_t &
  select(int index, const reply_callback_t &reply_callback);

  std::future<reply_t>
  select(int index);

  client_t &
  set(const std::string &key,
      const std::string &value,
      const reply_callback_t &reply_callback);

  std::future<reply_t>
  set(const std::string &key, const std::string &value);

  client_t &
  set_advanced(const std::string &key,
	       const std::string &value,
	       const reply_callback_t &reply_callback);

  client_t &
  set_advanced(const std::string &key,
	       const std::string &value,
	       bool ex,
	       int ex_sec,
	       bool px,
	       int px_milli,
	       bool nx,
	       bool xx,
	       const reply_callback_t &reply_callback);

  std::future<reply_t>
  set_advanced(const std::string &key,
	       const std::string &value,
	       bool ex = false,
	       int ex_sec = 0,
	       bool px = false,
	       int px_milli = 0,
	       bool nx = false,
	       bool xx = false);

  client_t &
  setbit_(const std::string &key,
	  int offset,
	  const std::string &value,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  setbit_(const std::string &key, int offset, const std::string &value);

  client_t &
  setex(const std::string &key,
	int64_t seconds,
	const std::string &value,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  setex(const std::string &key, int64_t seconds, const std::string &value);

  client_t &
  setnx(const std::string &key,
	const std::string &value,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  setnx(const std::string &key, const std::string &value);

  client_t &
  setrange(const std::string &key,
	   int offset,
	   const std::string &value,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  setrange(const std::string &key, int offset, const std::string &value);

  client_t &
  shutdown(const reply_callback_t &reply_callback);

  std::future<reply_t>
  shutdown();

  client_t &
  shutdown(const std::string &save, const reply_callback_t &reply_callback);

  std::future<reply_t>
  shutdown(const std::string &save);

  client_t &
  sinter(const std::vector<std::string> &keys,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  sinter(const std::vector<std::string> &keys);

  client_t &
  sinterstore(const std::string &destination,
	      const std::vector<std::string> &keys,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  sinterstore(const std::string &dst, const std::vector<std::string> &keys);

  client_t &
  sismember(const std::string &key,
	    const std::string &member,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  sismember(const std::string &key, const std::string &member);

  client_t &
  slaveof(const std::string &host,
	  int port,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  slaveof(const std::string &host, int port);

  client_t &
  slowlog(std::string subcommand, const reply_callback_t &reply_callback);

  std::future<reply_t>
  slowlog(const std::string &subcommand);

  client_t &
  slowlog(std::string subcommand,
	  const std::string &argument,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  slowlog(const std::string &subcommand, const std::string &argument);

  client_t &
  smembers(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  smembers(const std::string &key);

  client_t &
  smove(const std::string &source,
	const std::string &destination,
	const std::string &member,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  smove(const std::string &src,
	const std::string &dst,
	const std::string &member);

  client_t &
  sort(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key);

  client_t &
  sort(const std::string &key,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha);

  client_t &
  sort(const std::string &key,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha);

  client_t &
  sort(const std::string &key,
       const std::string &by_pattern,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       const std::string &by_pattern,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha);

  client_t &
  sort(const std::string &key,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest);

  client_t &
  sort(const std::string &key,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest);

  client_t &
  sort(const std::string &key,
       const std::string &by_pattern,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       const std::string &by_pattern,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest);

  client_t &
  sort(const std::string &key,
       const std::string &by_pattern,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       const std::string &by_pattern,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha);

  client_t &
  sort(const std::string &key,
       const std::string &by_pattern,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  sort(const std::string &key,
       const std::string &by_pattern,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest);

  client_t &
  spop(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  spop(const std::string &key);

  client_t &
  spop(const std::string &key,
       int count,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  spop(const std::string &key, int count);

  client_t &
  srandmember(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  srandmember(const std::string &key);

  client_t &
  srandmember(const std::string &key,
	      int count,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  srandmember(const std::string &key, int count);

  client_t &
  srem(const std::string &key,
       const std::vector<std::string> &members,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  srem(const std::string &key, const std::vector<std::string> &members);

  client_t &
  sscan(const std::string &key,
	std::size_t cursor,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  sscan(const std::string &key, std::size_t cursor);

  client_t &
  sscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  sscan(const std::string &key, std::size_t cursor, const std::string &pattern);

  client_t &
  sscan(const std::string &key,
	std::size_t cursor,
	std::size_t count,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  sscan(const std::string &key, std::size_t cursor, std::size_t count);

  client_t &
  sscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	std::size_t count,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  sscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	std::size_t count);

  client_t &
  strlen(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  strlen(const std::string &key);

  client_t &
  sunion(const std::vector<std::string> &keys,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  sunion(const std::vector<std::string> &keys);

  client_t &
  sunionstore(const std::string &destination,
	      const std::vector<std::string> &keys,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  sunionstore(const std::string &dst, const std::vector<std::string> &keys);

  client_t &
  sync(const reply_callback_t &reply_callback);

  std::future<reply_t>
  sync();

  client_t &
  time(const reply_callback_t &reply_callback);

  std::future<reply_t>
  time();

  client_t &
  ttl(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  ttl(const std::string &key);

  client_t &
  type(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  type(const std::string &key);

  client_t &
  unwatch(const reply_callback_t &reply_callback);

  std::future<reply_t>
  unwatch();

  client_t &
  wait(int numslaves, int timeout, const reply_callback_t &reply_callback);

  std::future<reply_t>
  wait(int numslaves, int timeout);

  client_t &
  watch(const std::vector<std::string> &keys,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  watch(const std::vector<std::string> &keys);

  //!
  //!  @brief
  //!  @param stream
  //!  @param group
  //!  @param message_ids
  //!  @param reply_callback
  //!  @return
  //!
  client_t &
  xack(const std::string &stream,
       const std::string &group,
       const std::vector<std::string> &message_ids,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  xack(const std::string &key,
       const std::string &group,
       const std::vector<std::string> &id_members);

  client_t &
  xadd(const std::string &key,
       const std::string &id,
       const std::multimap<std::string, std::string> &field_members,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  xadd(const std::string &key,
       const std::string &id,
       const std::multimap<std::string, std::string> &field_members);

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
  //!
  client_t &
  xclaim(const std::string &stream,
	 const std::string &group,
	 const std::string &consumer,
	 int min_idle_time,
	 const std::vector<std::string> &message_ids,
	 const xclaim_options_t &options,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  xclaim(const std::string &key,
	 const std::string &group,
	 const std::string &consumer,
	 const int &min_idle_time,
	 const std::vector<std::string> &id_members,
	 const xclaim_options_t &options);

  client_t &
  xdel(const std::string &key,
       const std::vector<std::string> &id_members,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  xdel(const std::string &key, const std::vector<std::string> &id_members);

  client_t &
  xgroup_create(const std::string &key,
		const std::string &group_name,
		const reply_callback_t &reply_callback);

  client_t &
  xgroup_create(const std::string &key,
		const std::string &group_name,
		const std::string &id,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  xgroup_create(const std::string &key,
		const std::string &group_name,
		const std::string &id = "$");

  client_t &
  xgroup_set_id(const std::string &key,
		const std::string &group_name,
		const reply_callback_t &reply_callback);

  client_t &
  xgroup_set_id(const std::string &key,
		const std::string &group_name,
		const std::string &id,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  xgroup_set_id(const std::string &key,
		const std::string &group_name,
		const std::string &id = "$");

  client_t &
  xgroup_destroy(const std::string &key,
		 const std::string &group_name,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  xgroup_destroy(const std::string &key, const std::string &group_name);

  client_t &
  xgroup_del_consumer(const std::string &key,
		      const std::string &group_name,
		      const std::string &consumer_name,
		      const reply_callback_t &reply_callback);

  std::future<reply_t>
  xgroup_del_consumer(const std::string &key,
		      const std::string &group_name,
		      const std::string &consumer_name);

  //!
  //!  @brief introspection command used in order to retrieve different
  //!  information about the consumer groups
  //!  @param key stream id
  //!  @param group_name stream consumer group name
  //!  @return
  //!
  client_t &
  xinfo_consumers(const std::string &key,
		  const std::string &group_name,
		  const reply_callback_t &reply_callback);

  //!
  //!  @brief \copybrief client::xinfo_consumers(key, group_name,
  //!  reply_callback)
  //!  @param key stream id
  //!  @param group_name
  //!  @return
  //!
  std::future<reply_t>
  xinfo_consumers(const std::string &key, const std::string &group_name);

  //!
  //!  @brief \copybrief client::xinfo_consumers(key, group_name,
  //!  reply_callback)
  //!  @param key stream id
  //!  @param reply_callback
  //!  @return
  //!
  client_t &
  xinfo_groups(const std::string &key, const reply_callback_t &reply_callback);

  //!
  //!  @brief \copybrief client::xinfo_consumers(key, group_name,
  //!  reply_callback)
  //!  @param stream stream id
  //!  @return
  //!
  std::future<reply_t>
  xinfo_groups(const std::string &stream);

  client_t &
  xinfo_stream(const std::string &stream,
	       const reply_callback_t &reply_callback);

  std::future<reply_t>
  xinfo_stream(const std::string &stream);

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
  client_t &
  xlen(const std::string &stream, const reply_callback_t &reply_callback);

  //!
  //!  @copydoc client::xlen(key, reply_callback)
  //!  @param key
  //!  @return
  //!
  std::future<reply_t>
  xlen(const std::string &key);

  //!
  //!  @brief inspects the list of pending messages for the stream & group
  //!  @param stream
  //!  @param group
  //!  @param options
  //!  @param reply_callback
  //!  @return
  //!
  client_t &
  xpending(const std::string &stream,
	   const std::string &group,
	   const xpending_options_t &options,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  xpending(const std::string &stream,
	   const std::string &group,
	   const xpending_options_t &options);
  // endregion

  //!
  //!  @brief
  //!  @param stream
  //!  @param options
  //!  @param reply_callback
  //!  @return
  //!
  client_t &
  xrange(const std::string &stream,
	 const range_options_t &options,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  xrange(const std::string &stream, const range_options_t &range_args);

  //!
  //!  @brief
  //!  @param a streams_t Streams std::int32_t Count std::int32_t Block;
  //!  @param reply_callback
  //!  @return
  //!
  client_t &
  xread(const xread_options_t &a, const reply_callback_t &reply_callback);

  std::future<reply_t>
  xread(const xread_options_t &a);

  client_t &
  xreadgroup(const xreadgroup_options_t &a,
	     const reply_callback_t &reply_callback);

  std::future<reply_t>
  xreadgroup(const xreadgroup_options_t &a);

  client_t &
  xrevrange(const std::string &key,
	    const range_options_t &range_args,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  xrevrange(const std::string &key, const range_options_t &range_args);

  //!
  //!  @brief trims the stream to a given number of items, evicting older items
  //!  (items with lower IDs) if needed
  //!  @param stream
  //!  @param max_len
  //!  @param reply_callback
  //!  @return
  //!
  client_t &
  xtrim(const std::string &stream,
	int max_len,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  xtrim(const std::string &key, int max_len);

  //!
  //!  optimizes the xtrim command
  //!
  //!
  client_t &
  xtrim_approx(const std::string &key,
	       int max_len,
	       const reply_callback_t &reply_callback);

  std::future<reply_t>
  xtrim_approx(const std::string &key, int max_len);

  client_t &
  zadd(const std::string &key,
       const std::vector<std::string> &options,
       const std::multimap<std::string, std::string> &score_members,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  zadd(const std::string &key,
       const std::vector<std::string> &options,
       const std::multimap<std::string, std::string> &score_members);

  client_t &
  zcard(const std::string &key, const reply_callback_t &reply_callback);

  std::future<reply_t>
  zcard(const std::string &key);

  client_t &
  zcount(const std::string &key,
	 int min,
	 int max,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zcount(const std::string &key, int min, int max);

  client_t &
  zcount(const std::string &key,
	 double min,
	 double max,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zcount(const std::string &key, double min, double max);

  client_t &
  zcount(const std::string &key,
	 const std::string &min,
	 const std::string &max,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zcount(const std::string &key,
	 const std::string &min,
	 const std::string &max);

  client_t &
  zincrby(const std::string &key,
	  int incr,
	  const std::string &member,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zincrby(const std::string &key, int incr, const std::string &member);

  client_t &
  zincrby(const std::string &key,
	  double incr,
	  const std::string &member,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zincrby(const std::string &key, double incr, const std::string &member);

  client_t &
  zincrby(const std::string &key,
	  const std::string &incr,
	  const std::string &member,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zincrby(const std::string &key,
	  const std::string &incr,
	  const std::string &member);

  client_t &
  zinterstore(const std::string &destination,
	      std::size_t numkeys,
	      const std::vector<std::string> &keys,
	      std::vector<std::size_t> weights,
	      aggregate_method method,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zinterstore(const std::string &destination,
	      std::size_t numkeys,
	      const std::vector<std::string> &keys,
	      std::vector<std::size_t> weights,
	      aggregate_method method);

  client_t &
  zlexcount(const std::string &key,
	    int min,
	    int max,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  zlexcount(const std::string &key, int min, int max);

  client_t &
  zlexcount(const std::string &key,
	    double min,
	    double max,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  zlexcount(const std::string &key, double min, double max);

  client_t &
  zlexcount(const std::string &key,
	    const std::string &min,
	    const std::string &max,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  zlexcount(const std::string &key,
	    const std::string &min,
	    const std::string &max);

  client_t &
  zpopmin(const std::string &key,
	  int count,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zpopmin(const std::string &key, int count);

  client_t &
  zpopmax(const std::string &key,
	  int count,
	  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zpopmax(const std::string &key, int count);

  client_t &
  zrange(const std::string &key,
	 int start,
	 int stop,
	 const reply_callback_t &reply_callback);

  client_t &
  zrange(const std::string &key,
	 int start,
	 int stop,
	 bool withscores,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrange(const std::string &key, int start, int stop, bool withscores = false);

  client_t &
  zrange(const std::string &key,
	 double start,
	 double stop,
	 const reply_callback_t &reply_callback);

  client_t &
  zrange(const std::string &key,
	 double start,
	 double stop,
	 bool withscores,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrange(const std::string &key,
	 double start,
	 double stop,
	 bool withscores = false);

  client_t &
  zrange(const std::string &key,
	 const std::string &start,
	 const std::string &stop,
	 const reply_callback_t &reply_callback);

  client_t &
  zrange(const std::string &key,
	 const std::string &start,
	 const std::string &stop,
	 bool withscores,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrange(const std::string &key,
	 const std::string &start,
	 const std::string &stop,
	 bool withscores = false);

  client_t &
  zrangebylex(const std::string &key,
	      int min,
	      int max,
	      const reply_callback_t &reply_callback);

  client_t &
  zrangebylex(const std::string &key,
	      int min,
	      int max,
	      bool withscores,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebylex(const std::string &key,
	      int min,
	      int max,
	      bool withscores = false);

  client_t &
  zrangebylex(const std::string &key,
	      double min,
	      double max,
	      const reply_callback_t &reply_callback);

  client_t &
  zrangebylex(const std::string &key,
	      double min,
	      double max,
	      bool withscores,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebylex(const std::string &key,
	      double min,
	      double max,
	      bool withscores = false);

  client_t &
  zrangebylex(const std::string &key,
	      const std::string &min,
	      const std::string &max,
	      const reply_callback_t &reply_callback);

  client_t &
  zrangebylex(const std::string &key,
	      const std::string &min,
	      const std::string &max,
	      bool withscores,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebylex(const std::string &key,
	      const std::string &min,
	      const std::string &max,
	      bool withscores = false);

  client_t &
  zrangebylex(const std::string &key,
	      int min,
	      int max,
	      std::size_t offset,
	      std::size_t count,
	      const reply_callback_t &reply_callback);

  client_t &
  zrangebylex(const std::string &key,
	      int min,
	      int max,
	      std::size_t offset,
	      std::size_t count,
	      bool withscores,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebylex(const std::string &key,
	      int min,
	      int max,
	      std::size_t offset,
	      std::size_t count,
	      bool withscores = false);

  client_t &
  zrangebylex(const std::string &key,
	      double min,
	      double max,
	      std::size_t offset,
	      std::size_t count,
	      const reply_callback_t &reply_callback);

  client_t &
  zrangebylex(const std::string &key,
	      double min,
	      double max,
	      std::size_t offset,
	      std::size_t count,
	      bool withscores,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebylex(const std::string &key,
	      double min,
	      double max,
	      std::size_t offset,
	      std::size_t count,
	      bool withscores = false);

  client_t &
  zrangebylex(const std::string &key,
	      const std::string &min,
	      const std::string &max,
	      std::size_t offset,
	      std::size_t count,
	      const reply_callback_t &reply_callback);

  client_t &
  zrangebylex(const std::string &key,
	      const std::string &min,
	      const std::string &max,
	      std::size_t offset,
	      std::size_t count,
	      bool withscores,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebylex(const std::string &key,
	      const std::string &min,
	      const std::string &max,
	      std::size_t offset,
	      std::size_t count,
	      bool withscores = false);

  client_t &
  zrangebyscore(const std::string &key,
		int min,
		int max,
		const reply_callback_t &reply_callback);

  client_t &
  zrangebyscore(const std::string &key,
		int min,
		int max,
		bool withscores,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebyscore(const std::string &key,
		int min,
		int max,
		bool withscores = false);

  client_t &
  zrangebyscore(const std::string &key,
		double min,
		double max,
		const reply_callback_t &reply_callback);

  client_t &
  zrangebyscore(const std::string &key,
		double min,
		double max,
		bool withscores,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebyscore(const std::string &key,
		double min,
		double max,
		bool withscores = false);

  client_t &
  zrangebyscore(const std::string &key,
		const std::string &min,
		const std::string &max,
		const reply_callback_t &reply_callback);

  client_t &
  zrangebyscore(const std::string &key,
		const std::string &min,
		const std::string &max,
		bool withscores,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebyscore(const std::string &key,
		const std::string &min,
		const std::string &max,
		bool withscores = false);

  client_t &
  zrangebyscore(const std::string &key,
		int min,
		int max,
		std::size_t offset,
		std::size_t count,
		const reply_callback_t &reply_callback);

  client_t &
  zrangebyscore(const std::string &key,
		int min,
		int max,
		std::size_t offset,
		std::size_t count,
		bool withscores,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebyscore(const std::string &key,
		int min,
		int max,
		std::size_t offset,
		std::size_t count,
		bool withscores = false);

  client_t &
  zrangebyscore(const std::string &key,
		double min,
		double max,
		std::size_t offset,
		std::size_t count,
		const reply_callback_t &reply_callback);

  client_t &
  zrangebyscore(const std::string &key,
		double min,
		double max,
		std::size_t offset,
		std::size_t count,
		bool withscores,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebyscore(const std::string &key,
		double min,
		double max,
		std::size_t offset,
		std::size_t count,
		bool withscores = false);

  client_t &
  zrangebyscore(const std::string &key,
		const std::string &min,
		const std::string &max,
		std::size_t offset,
		std::size_t count,
		const reply_callback_t &reply_callback);

  client_t &
  zrangebyscore(const std::string &key,
		const std::string &min,
		const std::string &max,
		std::size_t offset,
		std::size_t count,
		bool withscores,
		const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrangebyscore(const std::string &key,
		const std::string &min,
		const std::string &max,
		std::size_t offset,
		std::size_t count,
		bool withscores = false);

  client_t &
  zrank(const std::string &key,
	const std::string &member,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrank(const std::string &key, const std::string &member);

  client_t &
  zrem(const std::string &key,
       const std::vector<std::string> &members,
       const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrem(const std::string &key, const std::vector<std::string> &members);

  client_t &
  zremrangebylex(const std::string &key,
		 int min,
		 int max,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebylex(const std::string &key, int min, int max);

  client_t &
  zremrangebylex(const std::string &key,
		 double min,
		 double max,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebylex(const std::string &key, double min, double max);

  client_t &
  zremrangebylex(const std::string &key,
		 const std::string &min,
		 const std::string &max,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebylex(const std::string &key,
		 const std::string &min,
		 const std::string &max);

  client_t &
  zremrangebyrank(const std::string &key,
		  int start,
		  int stop,
		  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebyrank(const std::string &key, int start, int stop);

  client_t &
  zremrangebyrank(const std::string &key,
		  double start,
		  double stop,
		  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebyrank(const std::string &key, double start, double stop);

  client_t &
  zremrangebyrank(const std::string &key,
		  const std::string &start,
		  const std::string &stop,
		  const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebyrank(const std::string &key,
		  const std::string &start,
		  const std::string &stop);

  client_t &
  zremrangebyscore(const std::string &key,
		   int min,
		   int max,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebyscore(const std::string &key, int min, int max);

  client_t &
  zremrangebyscore(const std::string &key,
		   double min,
		   double max,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebyscore(const std::string &key, double min, double max);

  client_t &
  zremrangebyscore(const std::string &key,
		   const std::string &min,
		   const std::string &max,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zremrangebyscore(const std::string &key,
		   const std::string &min,
		   const std::string &max);

  client_t &
  zrevrange(const std::string &key,
	    int start,
	    int stop,
	    const reply_callback_t &reply_callback);

  client_t &
  zrevrange(const std::string &key,
	    int start,
	    int stop,
	    bool withscores,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrange(const std::string &key,
	    int start,
	    int stop,
	    bool withscores = false);

  client_t &
  zrevrange(const std::string &key,
	    double start,
	    double stop,
	    const reply_callback_t &reply_callback);

  client_t &
  zrevrange(const std::string &key,
	    double start,
	    double stop,
	    bool withscores,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrange(const std::string &key,
	    double start,
	    double stop,
	    bool withscores = false);

  client_t &
  zrevrange(const std::string &key,
	    const std::string &start,
	    const std::string &stop,
	    const reply_callback_t &reply_callback);

  client_t &
  zrevrange(const std::string &key,
	    const std::string &start,
	    const std::string &stop,
	    bool withscores,
	    const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrange(const std::string &key,
	    const std::string &start,
	    const std::string &stop,
	    bool withscores = false);

  client_t &
  zrevrangebylex(const std::string &key,
		 int max,
		 int min,
		 const reply_callback_t &reply_callback);

  client_t &
  zrevrangebylex(const std::string &key,
		 int max,
		 int min,
		 bool withscores,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebylex(const std::string &key,
		 int max,
		 int min,
		 bool withscores = false);

  client_t &
  zrevrangebylex(const std::string &key,
		 double max,
		 double min,
		 const reply_callback_t &reply_callback);

  client_t &
  zrevrangebylex(const std::string &key,
		 double max,
		 double min,
		 bool withscores,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebylex(const std::string &key,
		 double max,
		 double min,
		 bool withscores = false);

  client_t &
  zrevrangebylex(const std::string &key,
		 const std::string &max,
		 const std::string &min,
		 const reply_callback_t &reply_callback);

  client_t &
  zrevrangebylex(const std::string &key,
		 const std::string &max,
		 const std::string &min,
		 bool withscores,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebylex(const std::string &key,
		 const std::string &max,
		 const std::string &min,
		 bool withscores = false);

  client_t &
  zrevrangebylex(const std::string &key,
		 int max,
		 int min,
		 std::size_t offset,
		 std::size_t count,
		 const reply_callback_t &reply_callback);

  client_t &
  zrevrangebylex(const std::string &key,
		 int max,
		 int min,
		 std::size_t offset,
		 std::size_t count,
		 bool withscores,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebylex(const std::string &key,
		 int max,
		 int min,
		 std::size_t offset,
		 std::size_t count,
		 bool withscores = false);

  client_t &
  zrevrangebylex(const std::string &key,
		 double max,
		 double min,
		 std::size_t offset,
		 std::size_t count,
		 const reply_callback_t &reply_callback);

  client_t &
  zrevrangebylex(const std::string &key,
		 double max,
		 double min,
		 std::size_t offset,
		 std::size_t count,
		 bool withscores,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebylex(const std::string &key,
		 double max,
		 double min,
		 std::size_t offset,
		 std::size_t count,
		 bool withscores = false);

  client_t &
  zrevrangebylex(const std::string &key,
		 const std::string &max,
		 const std::string &min,
		 std::size_t offset,
		 std::size_t count,
		 const reply_callback_t &reply_callback);

  client_t &
  zrevrangebylex(const std::string &key,
		 const std::string &max,
		 const std::string &min,
		 std::size_t offset,
		 std::size_t count,
		 bool withscores,
		 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebylex(const std::string &key,
		 const std::string &max,
		 const std::string &min,
		 std::size_t offset,
		 std::size_t count,
		 bool withscores = false);

  client_t &
  zrevrangebyscore(const std::string &key,
		   int max,
		   int min,
		   const reply_callback_t &reply_callback);

  client_t &
  zrevrangebyscore(const std::string &key,
		   int max,
		   int min,
		   bool withscores,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebyscore(const std::string &key,
		   int max,
		   int min,
		   bool withscores = false);

  client_t &
  zrevrangebyscore(const std::string &key,
		   double max,
		   double min,
		   const reply_callback_t &reply_callback);

  client_t &
  zrevrangebyscore(const std::string &key,
		   double max,
		   double min,
		   bool withscores,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebyscore(const std::string &key,
		   double max,
		   double min,
		   bool withscores = false);

  client_t &
  zrevrangebyscore(const std::string &key,
		   const std::string &max,
		   const std::string &min,
		   const reply_callback_t &reply_callback);

  client_t &
  zrevrangebyscore(const std::string &key,
		   const std::string &max,
		   const std::string &min,
		   bool withscores,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebyscore(const std::string &key,
		   const std::string &max,
		   const std::string &min,
		   bool withscores = false);

  client_t &
  zrevrangebyscore(const std::string &key,
		   int max,
		   int min,
		   std::size_t offset,
		   std::size_t count,
		   const reply_callback_t &reply_callback);

  client_t &
  zrevrangebyscore(const std::string &key,
		   int max,
		   int min,
		   std::size_t offset,
		   std::size_t count,
		   bool withscores,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebyscore(const std::string &key,
		   int max,
		   int min,
		   std::size_t offset,
		   std::size_t count,
		   bool withscores = false);

  client_t &
  zrevrangebyscore(const std::string &key,
		   double max,
		   double min,
		   std::size_t offset,
		   std::size_t count,
		   const reply_callback_t &reply_callback);

  client_t &
  zrevrangebyscore(const std::string &key,
		   double max,
		   double min,
		   std::size_t offset,
		   std::size_t count,
		   bool withscores,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebyscore(const std::string &key,
		   double max,
		   double min,
		   std::size_t offset,
		   std::size_t count,
		   bool withscores = false);

  client_t &
  zrevrangebyscore(const std::string &key,
		   const std::string &max,
		   const std::string &min,
		   std::size_t offset,
		   std::size_t count,
		   const reply_callback_t &reply_callback);

  client_t &
  zrevrangebyscore(const std::string &key,
		   const std::string &max,
		   const std::string &min,
		   std::size_t offset,
		   std::size_t count,
		   bool withscores,
		   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrangebyscore(const std::string &key,
		   const std::string &max,
		   const std::string &min,
		   std::size_t offset,
		   std::size_t count,
		   bool withscores = false);

  client_t &
  zrevrank(const std::string &key,
	   const std::string &member,
	   const reply_callback_t &reply_callback);

  std::future<reply_t>
  zrevrank(const std::string &key, const std::string &member);

  client_t &
  zscan(const std::string &key,
	std::size_t cursor,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  zscan(const std::string &key, std::size_t cursor);

  client_t &
  zscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  zscan(const std::string &key, std::size_t cursor, const std::string &pattern);

  client_t &
  zscan(const std::string &key,
	std::size_t cursor,
	std::size_t count,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  zscan(const std::string &key, std::size_t cursor, std::size_t count);

  client_t &
  zscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	std::size_t count,
	const reply_callback_t &reply_callback);

  std::future<reply_t>
  zscan(const std::string &key,
	std::size_t cursor,
	const std::string &pattern,
	std::size_t count);

  client_t &
  zscore(const std::string &key,
	 const std::string &member,
	 const reply_callback_t &reply_callback);

  std::future<reply_t>
  zscore(const std::string &key, const std::string &member);

  client_t &
  zunionstore(const std::string &destination,
	      std::size_t numkeys,
	      const std::vector<std::string> &keys,
	      std::vector<std::size_t> weights,
	      aggregate_method method,
	      const reply_callback_t &reply_callback);

  std::future<reply_t>
  zunionstore(const std::string &destination,
	      std::size_t numkeys,
	      const std::vector<std::string> &keys,
	      std::vector<std::size_t> weights,
	      aggregate_method method);

private:
  //!
  //!  client kill impl
  //!
  //!
  template <typename T>
  typename std::enable_if<std::is_same<T, client_type>::value>::type
  client_kill_unpack_arg(std::vector<std::string> &redis_cmd,
			 reply_callback_t &,
			 client_type type);

  template <typename T>
  typename std::enable_if<std::is_same<T, bool>::value>::type
  client_kill_unpack_arg(std::vector<std::string> &redis_cmd,
			 reply_callback_t &,
			 bool skip);

  template <typename T>
  typename std::enable_if<std::is_integral<T>::value>::type
  client_kill_unpack_arg(std::vector<std::string> &redis_cmd,
			 reply_callback_t &,
			 uint64_t id);

  template <typename T>
  typename std::enable_if<std::is_class<T>::value>::type
  client_kill_unpack_arg(std::vector<std::string> &,
			 reply_callback_t &reply_callback,
			 const T &cb);

  template <typename T, typename... Ts>
  void
  client_kill_impl(std::vector<std::string> &redis_cmd,
		   reply_callback_t &reply,
		   const T &arg,
		   const Ts &... args);

  template <typename T>
  void
  client_kill_impl(std::vector<std::string> &redis_cmd,
		   reply_callback_t &reply,
		   const T &arg);

private:
  //!
  //!  sort impl
  //!
  //!
  client_t &
  sort(const std::string &key,
       const std::string &by_pattern,
       bool limit,
       std::size_t offset,
       std::size_t count,
       const std::vector<std::string> &get_patterns,
       bool asc_order,
       bool alpha,
       const std::string &store_dest,
       const reply_callback_t &reply_callback);

  //!
  //!  zrevrangebyscore impl
  //!
  //!
  client_t &
  zrevrangebyscore(const std::string &key,
		   const std::string &max,
		   const std::string &min,
		   bool limit,
		   std::size_t offset,
		   std::size_t count,
		   bool withscores,
		   const reply_callback_t &reply_callback);

  //!
  //!  zrangebyscore impl
  //!
  //!
  client_t &
  zrangebyscore(const std::string &key,
		const std::string &min,
		const std::string &max,
		bool limit,
		std::size_t offset,
		std::size_t count,
		bool withscores,
		const reply_callback_t &reply_callback);

  //!
  //!  zrevrangebylex impl
  //!
  //!
  client_t &
  zrevrangebylex(const std::string &key,
		 const std::string &max,
		 const std::string &min,
		 bool limit,
		 std::size_t offset,
		 std::size_t count,
		 bool withscores,
		 const reply_callback_t &reply_callback);

  //!
  //!  zrangebylex impl
  //!
  //!
  client_t &
  zrangebylex(const std::string &key,
	      const std::string &min,
	      const std::string &max,
	      bool limit,
	      std::size_t offset,
	      std::size_t count,
	      bool withscores,
	      const reply_callback_t &reply_callback);

private:
  //!
  //!  redis connection receive handler, triggered whenever a reply has been
  //!  read by the redis connection
  //!
  //!  @param connection redis_connection instance
  //!  @param reply parsed reply
  //!
  //!
  void
  connection_receive_handler(network::redis_connection &connection,
			     reply &reply);

  //!
  //!  redis_connection disconnection handler, triggered whenever a
  //!  disconnection occurred
  //!
  //!  @param connection redis_connection instance
  //!
  //!
  void
  connection_disconnection_handler(network::redis_connection &connection);

  //!
  //!  reset the queue of pending callbacks
  //!
  //!
  void
  clear_callbacks();

  //!
  //!  try to commit the pending pipelined
  //!  if client is disconnected, will throw an exception and clear all pending
  //!  callbacks (call clear_callbacks())
  //!
  //!
  void
  try_commit();

  //!
  //!  Execute a command on the client and tie the callback to a future
  //!
  //!
  std::future<reply_t>
  exec_cmd(const std::function<client_t &(const reply_callback_t &)> &f);

private:
  //!
  //!  struct to store commands information (command to be sent and callback to
  //!  be called)
  //!
  //!
  struct command_request {
    std::vector<std::string> command;
    reply_callback_t callback;
  };

private:
  //!
  //!  server we are connected to
  //!
  //!
  std::string m_redis_server;
  //!
  //!  port we are connected to
  //!
  //!
  std::size_t m_redis_port = 0;
  //!
  //!  master name (if we are using sentinel) we are connected to
  //!
  //!
  std::string m_master_name;
  //!
  //!  password used to authenticate
  //!
  //!
  std::string m_password;
  //!
  //!  selected redis db
  //!
  //!
  int m_database_index = 0;

  //!
  //!  tcp client for redis connection
  //!
  //!
  network::redis_connection m_client;

  //!
  //!  redis sentinel
  //!
  //!
  cpp_redis::sentinel m_sentinel;

  //!
  //!  max time to connect
  //!
  //!
  std::uint32_t m_connect_timeout_ms = 0;
  //!
  //!  max number of reconnection attempts
  //!
  //!
  std::int32_t m_max_reconnects = 0;
  //!
  //!  current number of attempts to reconnect
  //!
  //!
  std::int32_t m_current_reconnect_attempts = 0;
  //!
  //!  time between two reconnection attempts
  //!
  //!
  std::uint32_t m_reconnect_interval_ms = 0;

  //!
  //!  reconnection status
  //!
  //!
  std::atomic_bool m_reconnecting;
  //!
  //!  to force cancel reconnection
  //!
  //!
  std::atomic_bool m_cancel;

  //!
  //!  sent commands waiting to be executed
  //!
  //!
  std::queue<command_request> m_commands;

  //!
  //!  user defined connect status callback
  //!
  //!
  connect_callback_t m_connect_callback;

  //!
  //!   callbacks thread safety
  //!
  //!
  std::mutex m_callbacks_mutex;

  //!
  //!  condvar for callbacks updates
  //!
  //!
  std::condition_variable m_sync_condvar;

  //!
  //!  number of callbacks currently being running
  //!
  //!
  std::atomic<unsigned int> m_callbacks_running;
}; // class client

using client_t = client;

} // namespace cpp_redis

#include <cpp_redis/impl/client.ipp>

#endif
