#pragma once

#include <cstdint>

namespace shogi {

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
        return __builtin_popcountll(lo) + __builtin_popcountll(hi);
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

    int pop_lsb() {
        if (lo != 0) {
            const int bit = __builtin_ctzll(lo);
            lo &= lo - 1;
            return bit;
        }
        const int bit = __builtin_ctzll(hi);
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
