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
// SOFTWARE.#include "consumer.hpp"

#include <cpp_redis/core/consumer.hpp>

#include <functional>

using std::bind;
using namespace std::placeholders;

namespace cpp_redis {

consumer::client_container::client_container() : ack_client(), poll_client() {}

consumer::consumer(string_t stream, string_t consumer,
                   size_t max_concurrency)
    : m_callbacks(), m_stream(std::move(stream)), m_name(std::move(consumer)),
      m_read_id("0"), m_block_sec(-1), m_max_concurrency(max_concurrency),
      m_read_count(static_cast<int>(max_concurrency)) {
  // Supply the dispatch queue a callback to notify the queue when it is at max
  // capacity
  m_dispatch_queue = dispatch_queue_ptr_t(new dispatch_queue(
      stream, [&](size_t size) { dispatch_changed_handler(size); },
      max_concurrency));
  m_client = consumer::client_container_ptr_t(new consumer::client_container());
}

consumer_t &consumer::subscribe(
    const string_t &group, const consumer_callback_t &consumer_callback,
    const acknowledgement_callback_t &acknowledgement_callback) {
  m_callbacks.insert({group, {consumer_callback, acknowledgement_callback}});
  return *this;
}

void consumer::dispatch_changed_handler(size_t size) {
  if (size >= m_max_concurrency) {
    dispatch_queue_full.store(true);
    dispatch_queue_changed.notify_all();
  }
}

void consumer::connect(const string_t &host, size_t port,
                       const connect_callback_t &connect_callback,
                       int timeout_ms, int max_reconnects,
                       int reconnect_interval_ms) {
  m_client->ack_client.connect(host, port, connect_callback, timeout_ms,
                               max_reconnects, reconnect_interval_ms);
  m_client->poll_client.connect(host, port, connect_callback, timeout_ms,
                                max_reconnects, reconnect_interval_ms);
}

void consumer::auth(const string_t &password,
                    const reply_callback_t &reply_callback) {
  m_client->ack_client.auth(password, reply_callback);
  m_client->poll_client.auth(password, reply_callback);
}

consumer_t &consumer::commit() {
  while (!is_ready) {
    if (!is_ready) {
      mutex_lock_t dispatch_lock(dispatch_queue_changed_mutex);
      dispatch_queue_changed.wait(
          dispatch_lock, [&]() { return !dispatch_queue_full.load(); });
      m_read_count =
          static_cast<int>(m_max_concurrency - m_dispatch_queue->size());
      poll();
    }
  }
  return *this;
}

void consumer::dispatch(
    const xmessage_t &message,
    const pair<string_t, consumer_callback_container_t> &cb) {
  m_dispatch_queue->dispatch(message, [&](const message_type &message) {
    auto response = cb.second.consumer_callback(message);

    // add results to result stream
    m_client->ack_client.xadd(m_stream + ":results", CPP_REDIS_WILD_CARD,
                              response);

    // acknowledge task completion
    m_client->ack_client
        .xack(m_stream, cb.first, {message.get_id()},
              [&](const reply_t &r) {
                if (r.is_integer()) {
                  auto ret_int = r.as_integer();
                  cb.second.acknowledgement_callback(ret_int);
                }
              })
        .sync_commit();
    return response;
  });
}

void consumer::poll() {
  for (auto &cb : m_callbacks) {
    m_client->poll_client
        .xreadgroup(
            {cb.first,
             m_name,
             {{m_stream}, {m_read_id}},
             m_read_count,
             m_block_sec,
             false},
            [&](reply_t &reply) {
              // The reply is an array if valid
              xstream_reply s_reply(reply);
              if (!s_reply.is_null()) {
                __CPP_REDIS_LOG(2, "Stream " << s_reply)
                for (const auto &stream : s_reply) {
                  for (auto &m : stream.Messages) {
                    if (m_should_read_pending.load())
                      m_read_id = m.get_id();
                    try {
                      dispatch(m, cb);
                    } catch (std::exception &exc) {
                      __CPP_REDIS_LOG(
                          1, "Processing failed for message id: " + m.get_id() +
                                 "\nDetails: " + exc.what());
                      throw exc;
                    }
                  }
                }
              } else {
                check_for_pending();
              }
              return;
            })
        .sync_commit();
  }
}
} // namespace cpp_redis
