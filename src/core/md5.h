// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// md5.h — MD5 (RFC 1321) and base64, for the Content-MD5 header only.
//
// Why Content-MD5 at all, when every PUT is already signed over its SHA-256:
// a store that verifies the signed payload hash refuses damaged bytes inside
// the request, but not every S3-compatible store is known to. Content-MD5 is
// the oldest integrity header in the S3 API and the one every compatible store
// implements, so sending it makes the check happen in-request on those too.
// This is the hardening docs/adr/0003 §1 names; a refusal is an ordinary failed
// upload (put_failure.h: TransitDamage) and nothing is ever tested after a 2xx.
//
// Portable rather than per crypto backend: this is an integrity check, not a
// security primitive, and three backends (one of them Windows-only) would be
// three things to keep in step for a 100-line function.
//
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace multisite {

inline std::vector<uint8_t> md5(const uint8_t* data, size_t len) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const uint8_t S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;

    // Padded message: data, 0x80, zeros to 56 mod 64, then the bit length LE.
    const uint64_t bit_len = (uint64_t)len * 8;
    size_t padded = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded, 0);
    if (len) std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i) msg[padded - 8 + i] = (uint8_t)(bit_len >> (8 * i));

    auto rotl = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &msg[off + i * 4];
            M[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; ++i) {
            uint32_t f; int g;
            if (i < 16)      { f = (b & c) | (~b & d);  g = i; }
            else if (i < 32) { f = (d & b) | (~d & c);  g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;           g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);        g = (7 * i) % 16; }
            const uint32_t tmp = d;
            d = c; c = b;
            b = b + rotl(a + f + K[i] + M[g], S[i]);
            a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    std::vector<uint8_t> out(16);
    const uint32_t hs[4] = {h0, h1, h2, h3};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(hs[i] >> (8 * j));
    return out;
}

inline std::string base64_encode(const std::vector<uint8_t>& in) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = in[i] << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += '=';
    }
    return out;
}

// The Content-MD5 header value: base64 of the raw 16-byte digest.
inline std::string content_md5(const std::vector<uint8_t>& body) {
    return base64_encode(md5(body.data(), body.size()));
}

} // namespace multisite
