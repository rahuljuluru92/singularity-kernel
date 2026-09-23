#include <cstddef>

// GCC's codegen assumes these exist (standard C ABI names/signatures)
// even in freestanding mode — e.g. zero-initializing a large array can
// get lowered into a memset call rather than a per-element loop, even
// unoptimized. No libc here, so this project provides its own, exactly
// like include/cstdint and friends.

extern "C" void* memset(void* dest, int value, std::size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    for (std::size_t i = 0; i < count; i++) {
        d[i] = static_cast<unsigned char>(value);
    }
    return dest;
}

extern "C" void* memcpy(void* dest, const void* src, std::size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    for (std::size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

extern "C" void* memmove(void* dest, const void* src, std::size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    if (d < s) {
        for (std::size_t i = 0; i < count; i++) {
            d[i] = s[i];
        }
    } else {
        for (std::size_t i = count; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

extern "C" int memcmp(const void* a, const void* b, std::size_t count) {
    const auto* pa = static_cast<const unsigned char*>(a);
    const auto* pb = static_cast<const unsigned char*>(b);
    for (std::size_t i = 0; i < count; i++) {
        if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    return 0;
}
