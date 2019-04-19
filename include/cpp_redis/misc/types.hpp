#ifndef CPP_REDIS_MISC_TYPES_HPP_
#define CPP_REDIS_MISC_TYPES_HPP_

#include <cstdint>

namespace cpp_redis {
#if INTPTR_MAX == INT32_MAX
    using int_t = int32_t;
    using uint_t = uint32_t;
#else
    using int_t = int64_t;
    using uint_t = uint64_t;
#endif

using ms = int_t;

using string_t = std::string;
} // namespace cpp_redis

#endif