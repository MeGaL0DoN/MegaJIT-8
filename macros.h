#pragma once

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#define UNREACHABLE() __assume(false);
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#define UNREACHABLE() __builtin_unreachable();
#else
#define FORCE_INLINE inline
#define UNREACHABLE()
#endif