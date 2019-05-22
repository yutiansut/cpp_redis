#include <utility>

#ifndef CPP_REDIS_IMPL_REPLY_IPP_
#define CPP_REDIS_IMPL_REPLY_IPP_

#include <functional>
#include <iostream>

#include <cpp_redis/core/reply.hpp>

namespace cpp_redis {

template <typename T> class reply_payload_iface {
public:
  explicit reply_payload_iface(reply_t repl) : m_reply(std::move(repl)) {}
  virtual T get_payload() = 0;

  bool is_error() const { return m_reply.is_error(); }

  bool is_null() const { return m_reply.is_null(); }

  reply_t get_reply() { return m_reply; }

protected:
  reply_t m_reply;
};

} // namespace cpp_redis

#endif