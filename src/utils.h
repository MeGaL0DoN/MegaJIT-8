#pragma once
#include <cstdint>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#define UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#define UNREACHABLE() __assume(false)
#else
#define FORCE_INLINE inline
#define UNREACHABLE() *(volatile char*)nullptr
#endif

template <typename T>
inline uint64_t addr(T ptr)
{
	uint64_t x;
	std::memcpy(&x, &ptr, sizeof(x));
	return x;
}

template <typename T, typename V>
inline int32_t offset(T from, V to) { return static_cast<int32_t>(addr(to) - addr(from)); }