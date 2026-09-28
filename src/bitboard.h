#pragma once

#include <cstdint>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace shogi {

namespace bitboard_detail {
inline int popcount(std::uint64_t bits) {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt64(bits));
#else
    return __builtin_popcountll(bits);
#endif
}

inline int trailing_zeros(std::uint64_t bits) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, bits);
    return static_cast<int>(index);
#else
    return __builtin_ctzll(bits);
#endif
}

inline int highest_bit(std::uint64_t bits) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse64(&index, bits);
    return static_cast<int>(index);
#else
    return 63 - __builtin_clzll(bits);
#endif
}
}  // namespace bitboard_detail

struct Bitboard {
    static constexpr std::uint64_t kHiMask = (1ULL << 17) - 1;

    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    constexpr Bitboard() = default;
    constexpr Bitboard(std::uint64_t lo_bits, std::uint64_t hi_bits)
        : lo(lo_bits), hi(hi_bits & kHiMask) {}

    bool any() const {
        return lo != 0 || hi != 0;
    }

    bool none() const {
        return !any();
    }

    explicit operator bool() const {
        return any();
    }

    int count() const {
        return bitboard_detail::popcount(lo) + bitboard_detail::popcount(hi);
    }

    bool test(int square) const {
        if (square < 64) {
            return ((lo >> square) & 1ULL) != 0;
        }
        return ((hi >> (square - 64)) & 1ULL) != 0;
    }

    void set(int square) {
        if (square < 64) {
            lo |= 1ULL << square;
        } else {
            hi |= 1ULL << (square - 64);
            hi &= kHiMask;
        }
    }

    void reset(int square) {
        if (square < 64) {
            lo &= ~(1ULL << square);
        } else {
            hi &= ~(1ULL << (square - 64));
        }
    }

    // 最下位ビットの升（空でないこと）
    int lsb() const {
        return lo != 0 ? bitboard_detail::trailing_zeros(lo) : 64 + bitboard_detail::trailing_zeros(hi);
    }

    // 最上位ビットの升（空でないこと）
    int msb() const {
        return hi != 0 ? 64 + bitboard_detail::highest_bit(hi) : bitboard_detail::highest_bit(lo);
    }

    // 2 つ以上のビットが立っているか
    bool more_than_one() const {
        return (lo & (lo - 1)) != 0 || (hi & (hi - 1)) != 0 || (lo != 0 && hi != 0);
    }

    int pop_lsb() {
        if (lo != 0) {
            const int bit = bitboard_detail::trailing_zeros(lo);
            lo &= lo - 1;
            return bit;
        }
        const int bit = bitboard_detail::trailing_zeros(hi);
        hi &= hi - 1;
        return 64 + bit;
    }

    Bitboard& operator|=(const Bitboard& other) {
        lo |= other.lo;
        hi = (hi | other.hi) & kHiMask;
        return *this;
    }

    Bitboard& operator&=(const Bitboard& other) {
        lo &= other.lo;
        hi &= other.hi;
        return *this;
    }

    Bitboard& operator^=(const Bitboard& other) {
        lo ^= other.lo;
        hi = (hi ^ other.hi) & kHiMask;
        return *this;
    }
};

inline Bitboard operator|(Bitboard lhs, const Bitboard& rhs) {
    lhs |= rhs;
    return lhs;
}

inline Bitboard operator&(Bitboard lhs, const Bitboard& rhs) {
    lhs &= rhs;
    return lhs;
}

inline Bitboard operator^(Bitboard lhs, const Bitboard& rhs) {
    lhs ^= rhs;
    return lhs;
}

inline Bitboard operator~(const Bitboard& value) {
    return Bitboard(~value.lo, ~value.hi);
}

inline bool operator==(const Bitboard& lhs, const Bitboard& rhs) {
    return lhs.lo == rhs.lo && lhs.hi == rhs.hi;
}

inline bool operator!=(const Bitboard& lhs, const Bitboard& rhs) {
    return !(lhs == rhs);
}

}  // namespace shogi
