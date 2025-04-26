#pragma once
#include <cassert>

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#define UNREACHABLE() assert(false); __assume(false);
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#define UNREACHABLE() assert(false); __builtin_unreachable();
#else
#define FORCE_INLINE inline
#define UNREACHABLE()
#endif