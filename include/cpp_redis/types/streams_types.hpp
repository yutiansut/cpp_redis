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

#ifndef CPP_REDIS_TYPES_STREAMS_HPP_
#define CPP_REDIS_TYPES_STREAMS_HPP_

#include <chrono>
#include <cpp_redis/core/reply.hpp>
#include <cpp_redis/impl/serializer.ipp>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cpp_redis {

//!
//!  @brief first array is the session name, second is ids
//!
//!
using streams_t = std::pair<std::vector<std::string>, std::vector<std::string>>;

//!
//!  @brief Options
//!
typedef struct xread_options {
  streams_t Streams;
  int_t Count;
  int_t Block;
} xread_options_t;

typedef struct xreadgroup_options {
  string_t Group;
  string_t Consumer;
  streams_t Streams;
  int_t Count;
  int_t Block;
  bool NoAck;
} xreadgroup_options_t;

typedef struct range_options {
  string_t Start;
  string_t Stop;
  int_t Count;
} range_options_t;

typedef struct xclaim_options {
  int_t Idle;
  std::time_t *Time;
  int_t RetryCount;
  bool Force;
  bool JustId;
} xclaim_options_t;

typedef struct xpending_options {
  range_options_t Range;
  string_t Consumer;
} xpending_options_t;

//!
//!  @brief Replies
//!

class xmessage : public virtual message_type {
public:
  xmessage();
  virtual ~xmessage() {};

  explicit xmessage(const reply_t &data);

  friend std::ostream &operator<<(std::ostream &os, const xmessage &xm);
};

using xmessage_t = xmessage;

class xstream {
public:
  explicit xstream(const reply_t &data);

  friend std::ostream &operator<<(std::ostream &os, const xstream &xs);

  string_t Stream;
  std::vector<xmessage_t> Messages;
};

using xstream_t = xstream;

class xinfo_reply {
public:
  explicit xinfo_reply(const cpp_redis::reply_t &data);

  int_t Length;
  int_t RadixTreeKeys;
  int_t RadixTreeNodes;
  int_t Groups;
  string_t LastGeneratedId;
  xmessage_t FirstEntry;
  xmessage_t LastEntry;
};

class xstream_reply : public std::vector<xstream_t> {
public:
  explicit xstream_reply(const reply_t &data);

  friend std::ostream &operator<<(std::ostream &os, const xstream_reply &xs);

  bool is_null() const {
    if (empty())
      return true;
    for (auto &v : *this) {
      if (v.Messages.empty())
        return true;
    }
    return false;
  }
};

using xstream_reply_t = xstream_reply;

//!
//!  @brief Callbacks
//!

//!
//!  acknowledgment callback called whenever a subscribe completes
//!  takes as parameter the int returned by the redis server (usually the number
//!  of channels you are subscribed to)
//!
//!
using acknowledgement_callback_t = std::function<void(const int_t &)>;

//!
//!  high availability (re)connection states
//!   * dropped: connection has dropped
//!   * start: attempt of connection has started
//!   * sleeping: sleep between two attempts
//!   * ok: connected
//!   * failed: failed to connect
//!   * lookup failed: failed to retrieve master sentinel
//!   * stopped: stop to try to reconnect
//!
//!
enum class connect_state {
  dropped,
  start,
  sleeping,
  ok,
  failed,
  lookup_failed,
  stopped
};

using connect_state_t = connect_state;

//!
//!  connect handler, called whenever a new connection even occurred
//!
//!
typedef std::function<void(const string_t &host, std::size_t port,
                           connect_state status)>
    connect_callback_t;

using message_callback_t = std::function<void(const message_type_t &)>;
} // namespace cpp_redis

#endif // CPP_REDIS_TYPES_STREAMS_HPP_
