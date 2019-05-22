// The MIT License (MIT)
//
// Copyright (c) 11/27/18 nick. <nbatkins@gmail.com>
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
// SOFTWARE.#ifndef CPP_REDIS_CONSUMER_HPP

#ifndef CPP_REDIS_CONSUMER_HPP
#define CPP_REDIS_CONSUMER_HPP

#include <cpp_redis/core/client.hpp>
#include <cpp_redis/misc/dispatch_queue.hpp>
#include <string>

namespace cpp_redis {

using defer = shared_ptr<void>;

#define READ_NEW ">"
#define CPP_REDIS_WILD_CARD "*"

//!
//!  reply callback called whenever a reply is received
//!  takes as parameter the received reply
//!
using consumer_callback_t = dispatch_callback_t;

struct consumer_callback_container {
  consumer_callback_t consumer_callback;
  acknowledgement_callback_t acknowledgement_callback;
};

using consumer_callback_container_t = consumer_callback_container;

struct consumer_reply {
  string_t group_id;
  xstream_reply_t reply;
};

using consumer_reply_t = consumer_reply;

//!
//!  background processing using redis streams
//!
class consumer {
public:
  explicit consumer(
      string_t stream, string_t consumer,
      size_t max_concurrency = std::thread::hardware_concurrency());

  consumer &subscribe(
      const string_t &group, const consumer_callback_t &consumer_callback,
      const acknowledgement_callback_t &acknowledgement_callback = nullptr);

  //!
  //!  @brief Connect to redis server
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
               int timeout_ms = 0, int max_reconnects = 0,
               int reconnect_interval_ms = 0);

  void auth(const string_t &password,
            const reply_callback_t &reply_callback = nullptr);

  //!
  //!  commit pipelined transaction
  //!  that is, send to the network all commands pipelined by calling send() /
  //!  subscribe() / ...
  //!
  //!  @return current instance
  //!
  consumer &commit();
  //!
  //!  check if it is necessary to read
  //!  entries from the backlog
  //!
  void check_for_pending() {
    if (m_should_read_pending.load()) {
      m_should_read_pending.store(false);
      m_read_id = READ_NEW;
      // Set to block infinitely
      m_block_sec = 0;
      // Set to read 1
      m_read_count = 1;
    }
  }

  //!
  //!  fires upon a change in the dispatcher.
  //!  changes occur when a task item is finished
  //!  or the queue is full.
  //!
  void dispatch_changed_handler(size_t size);

private:
  //!
  //!  polls the stream for work items
  //!
  void poll();

  void dispatch(const xmessage_t &message,
                const pair<string_t, consumer_callback_container_t> &cb);

private:
  class client_container {
  public:
    client_container();

    client_t ack_client;
    client_t poll_client;
  };

public:
  //!
  //!  internal typedef for mapping callbacks
  //!
  using client_container_t = client_container;
  //!
  //!  internal typedef for mapping callbacks
  //!
  using client_container_ptr_t = unique_ptr<client_container_t>;
  //!
  //!  internal typedef for mapping callbacks
  //!
  using consumer_callbacks_t =
      multimap<string_t, consumer_callback_container_t>;

private:
  //!
  //!  the redis client
  //!
  client_container_ptr_t m_client;
  //!
  //!  callback container
  //!
  consumer_callbacks_t m_callbacks;
  //!
  //!  mutex for the callback container
  //!
  mutex_t m_callbacks_mutex;
  //!
  //!  dispatch queue:
  //!   fire and forget background processing
  //!
  dispatch_queue_ptr_t m_dispatch_queue;
  //!
  //!  whether to add additional work items
  //!
  atomic_bool dispatch_queue_full{false};
  //!
  //!  signals from the dispatcher
  //!
  condition_variable_t dispatch_queue_changed;
  //!
  //!  lock for the condi variable
  //!
  mutex_t dispatch_queue_changed_mutex;
  //!
  //!  the name of the stream
  //!
  bool is_ready = false;
  //!
  //!  whether or not the client should read
  //!  from new or stale
  //!
  atomic_bool m_should_read_pending{true};

private:
  //!
  //!  the name of the stream
  //!
  string_t m_stream;

  //!
  //!  the name of this consumer group
  //!
  string_t m_name;

  //!
  //!  the topic ID
  //!
  string_t m_read_id;

  //!
  //!  number of milliseconds to block when polling
  //!
  int m_block_sec;

  //!
  //!  maximum number of worker threads
  //!
  size_t m_max_concurrency;

  //!
  //!  number of messages read
  //!
  int m_read_count;
};

//!
//!  exported typedef
//!
using consumer_t = consumer;

} // namespace cpp_redis

#endif // CPP_REDIS_CONSUMER_HPP
