#pragma once
#include <mcp/detail/Base64Url.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace mcp { namespace detail { namespace p256 {

struct U256 {
    std::array<std::uint32_t, 8> w{};
};

inline constexpr U256 kZero{};
inline constexpr U256 kOne = {{1, 0, 0, 0, 0, 0, 0, 0}};
inline constexpr U256 kTwo = {{2, 0, 0, 0, 0, 0, 0, 0}};
inline constexpr U256 kP = {{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu}};
inline constexpr U256 kN = {{0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu}};
inline constexpr U256 kGx = {{0xD898C296u, 0xF4A13945u, 0x2DEB33A0u, 0x77037D81u,
    0x63A440F2u, 0xF8BCE6E5u, 0xE12C4247u, 0x6B17D1F2u}};
inline constexpr U256 kGy = {{0x37BF51F5u, 0xCBB64068u, 0x6B315ECEu, 0x2BCE3357u,
    0x7C0F9E16u, 0x8EE7EB4Au, 0xFE1A7F9Bu, 0x4FE342E2u}};

inline int Compare(const U256& lhs, const U256& rhs) {
    for (int i = 7; i >= 0; --i) {
        if (lhs.w[i] != rhs.w[i]) return lhs.w[i] < rhs.w[i] ? -1 : 1;
    }
    return 0;
}

inline bool IsZero(const U256& value) {
    for (std::uint32_t limb : value.w) {
        if (limb != 0) return false;
    }
    return true;
}

inline bool Bit(const U256& value, int index) {
    return ((value.w[static_cast<size_t>(index >> 5)] >> (index & 31)) & 1u) != 0;
}

inline U256 AddRaw(const U256& lhs, const U256& rhs) {
    U256 result;
    std::uint64_t carry = 0;
    for (size_t i = 0; i < 8; ++i) {
        std::uint64_t sum = static_cast<std::uint64_t>(lhs.w[i]) +
            rhs.w[i] + carry;
        result.w[i] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
    return result;
}

inline U256 SubRaw(const U256& lhs, const U256& rhs) {
    U256 result;
    std::uint64_t borrow = 0;
    for (size_t i = 0; i < 8; ++i) {
        std::uint64_t diff = static_cast<std::uint64_t>(lhs.w[i]) -
            rhs.w[i] - borrow;
        result.w[i] = static_cast<std::uint32_t>(diff);
        borrow = (diff >> 63) & 1u;
    }
    return result;
}

inline U256 AddMod(const U256& lhs, const U256& rhs, const U256& mod) {
    U256 threshold = SubRaw(mod, rhs);
    if (Compare(lhs, threshold) >= 0) return SubRaw(lhs, threshold);
    return AddRaw(lhs, rhs);
}

inline U256 SubMod(const U256& lhs, const U256& rhs, const U256& mod) {
    if (Compare(lhs, rhs) >= 0) return SubRaw(lhs, rhs);
    return SubRaw(mod, SubRaw(rhs, lhs));
}

inline U256 MulMod(const U256& lhs, const U256& rhs, const U256& mod) {
    U256 result;
    for (int i = 255; i >= 0; --i) {
        result = AddMod(result, result, mod);
        if (Bit(rhs, i)) result = AddMod(result, lhs, mod);
    }
    return result;
}

inline U256 InvMod(const U256& value, const U256& mod) {
    U256 exponent = SubRaw(mod, kTwo);
    U256 result = kOne;
    U256 base = value;
    for (int i = 255; i >= 0; --i) {
        result = MulMod(result, result, mod);
        if (Bit(exponent, i)) result = MulMod(result, base, mod);
    }
    return result;
}

struct Point {
    U256 x{};
    U256 y{};
    U256 z{};
    bool infinity{true};
};

inline Point DoublePoint(const Point& point) {
    if (point.infinity || IsZero(point.y) || IsZero(point.z)) return {};
    U256 yy = MulMod(point.y, point.y, kP);
    U256 s = MulMod(point.x, yy, kP);
    s = AddMod(s, s, kP);
    s = AddMod(s, s, kP);
    U256 x2 = MulMod(point.x, point.x, kP);
    U256 z2 = MulMod(point.z, point.z, kP);
    U256 z4 = MulMod(z2, z2, kP);
    U256 m = SubMod(x2, z4, kP);
    m = AddMod(m, m, kP);
    m = AddMod(AddMod(m, m, kP), m, kP);
    U256 x3 = SubMod(MulMod(m, m, kP), AddMod(s, s, kP), kP);
    U256 yy2 = MulMod(yy, yy, kP);
    U256 eight_yy2 = yy2;
    for (int i = 0; i < 3; ++i) eight_yy2 = AddMod(eight_yy2, eight_yy2, kP);
    U256 y3 = MulMod(m, SubMod(s, x3, kP), kP);
    y3 = SubMod(y3, eight_yy2, kP);
    U256 z3 = MulMod(point.y, point.z, kP);
    z3 = AddMod(z3, z3, kP);
    return Point{x3, y3, z3, false};
}

inline Point AddPoint(const Point& lhs, const Point& rhs) {
    if (lhs.infinity) return rhs;
    if (rhs.infinity) return lhs;
    U256 z1z1 = MulMod(lhs.z, lhs.z, kP);
    U256 z2z2 = MulMod(rhs.z, rhs.z, kP);
    U256 u1 = MulMod(lhs.x, z2z2, kP);
    U256 u2 = MulMod(rhs.x, z1z1, kP);
    U256 s1 = MulMod(MulMod(lhs.y, rhs.z, kP), z2z2, kP);
    U256 s2 = MulMod(MulMod(rhs.y, lhs.z, kP), z1z1, kP);
    if (Compare(u1, u2) == 0) {
        if (Compare(s1, s2) == 0) return DoublePoint(lhs);
        return {};
    }
    U256 h = SubMod(u2, u1, kP);
    U256 doubled_h = AddMod(h, h, kP);
    U256 i = MulMod(doubled_h, doubled_h, kP);
    U256 j = MulMod(h, i, kP);
    U256 r = AddMod(SubMod(s2, s1, kP), SubMod(s2, s1, kP), kP);
    U256 v = MulMod(u1, i, kP);
    U256 x3 = SubMod(SubMod(MulMod(r, r, kP), j, kP), AddMod(v, v, kP), kP);
    U256 s1j = MulMod(s1, j, kP);
    s1j = AddMod(s1j, s1j, kP);
    U256 y3 = SubMod(MulMod(r, SubMod(v, x3, kP), kP), s1j, kP);
    U256 z3 = MulMod(MulMod(lhs.z, rhs.z, kP), h, kP);
    z3 = AddMod(z3, z3, kP);
    return Point{x3, y3, z3, false};
}

inline Point ScalarMultiply(const U256& scalar) {
    Point result;
    Point generator{kGx, kGy, kOne, false};
    for (int i = 255; i >= 0; --i) {
        result = DoublePoint(result);
        if (Bit(scalar, i)) result = AddPoint(result, generator);
    }
    return result;
}

inline bool ToAffine(const Point& point, U256& x, U256& y) {
    if (point.infinity || IsZero(point.z)) return false;
    U256 z_inv = InvMod(point.z, kP);
    U256 z_inv2 = MulMod(z_inv, z_inv, kP);
    x = MulMod(point.x, z_inv2, kP);
    y = MulMod(MulMod(point.y, z_inv2, kP), z_inv, kP);
    return true;
}

inline bool DecodeCoordinate(std::string_view encoded, U256& out) {
    auto bytes = Base64UrlDecode(encoded);
    if (!bytes || bytes->size() != 32) return false;
    for (size_t i = 0; i < 8; ++i) {
        out.w[i] = static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[31 - 4 * i]))
            | (static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[30 - 4 * i])) << 8)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[29 - 4 * i])) << 16)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[28 - 4 * i])) << 24);
    }
    return true;
}

inline bool IsValidKey(std::string_view x, std::string_view y, std::string_view d) {
    U256 x_value;
    U256 y_value;
    U256 d_value;
    if (!DecodeCoordinate(x, x_value) || !DecodeCoordinate(y, y_value) ||
        !DecodeCoordinate(d, d_value)) {
        return false;
    }
    if (Compare(d_value, kZero) <= 0 || Compare(d_value, kN) >= 0) return false;
    if (Compare(x_value, kP) >= 0 || Compare(y_value, kP) >= 0) return false;
    U256 derived_x;
    U256 derived_y;
    if (!ToAffine(ScalarMultiply(d_value), derived_x, derived_y)) return false;
    return Compare(derived_x, x_value) == 0 && Compare(derived_y, y_value) == 0;
}

} } } // namespace mcp::detail::p256
