#ifndef CPP_REDIS_TYPES_COMMON_TYPES_HPP_
#define CPP_REDIS_TYPES_COMMON_TYPES_HPP_

#include <cstdint>

#include <mutex>
#include <string>
#include <queue>
#include <atomic>
#include <map>
#include <future>
#include <sstream>

namespace cpp_redis {

#if INTPTR_MAX == INT32_MAX
using int_t = int32_t;
using uint_t = uint32_t;
#else
using int_t = int64_t;
using uint_t = uint64_t;
#endif

using std::size_t;

using std::vector;

using std::atomic_bool;

using std::queue;

using std::atomic_uint;

using std::function;

using std::unique_ptr;

using std::multimap;
using std::map;

using std::tuple;

using std::pair;

using std::chrono::duration;

using std::future;
using std::shared_ptr;
using std::unique_lock;

using std::lock_guard;
using std::ostream;

using condition_variable_t = std::condition_variable;

using milliseconds_t = int_t;

using string_t = std::string;

using mutex_t = std::mutex;

using std::stringstream;

using mutex_lock_t = std::unique_lock<mutex_t>;
} // namespace cpp_redis

#endif // CPP_REDIS_TYPES_COMMON_TYPES_HPP_