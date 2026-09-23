// SPDX-License-Identifier: GPL-3.0-or-later
// test_mpp_convert.cpp — NV12 into I420, the one part of the MPP decode path
// that can be proved without a board.
//
// What it guards: the decoder's frames are I420 with plane pointers into their
// own buffer (video_output.h relies on that), and MPP hands back NV12 in a
// stride- and height-padded buffer. Getting the stride wrong drags padding into
// the picture as coloured stripes; getting the height wrong shifts the chroma
// off the luma. Neither is visible until somebody looks at a screen.
#include "../src/core/mpp_decoder.h"

#include <cstdio>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

int main() {
    // A 4x4 picture: Y counts up, every chroma sample is U=200, V=100.
    const int w = 4, h = 4;
    std::vector<uint8_t> y(4 * 4), uv(2 * 2 * 2);
    for (int i = 0; i < 16; ++i) y[(size_t)i] = (uint8_t)i;
    for (int i = 0; i < 4; ++i) { uv[(size_t)i * 2] = 200; uv[(size_t)i * 2 + 1] = 100; }

    std::vector<uint8_t> out;
    uint8_t* plane[3] = { nullptr, nullptr, nullptr };
    int stride[3] = { 0, 0, 0 };
    nv12_to_i420(y.data(), w, uv.data(), w, w, h, out, plane, stride);

    CHECK(out.size() == (size_t)(16 + 2 * 4), "size is Y + U + V for 4x4");
    CHECK(stride[0] == 4 && stride[1] == 2 && stride[2] == 2,
          "strides are width and half-width");
    CHECK(plane[0] == out.data(), "Y is the buffer start");
    CHECK(plane[1] == out.data() + 16, "U follows Y");
    CHECK(plane[2] == out.data() + 20, "V follows U");
    CHECK(plane[0][5] == 5, "luma copied");
    CHECK(plane[1][0] == 200 && plane[2][0] == 100, "chroma de-interleaved");

    // MPP's real shape: a row wider than the picture (1920 into a padded
    // stride). Only `width` bytes of each row may reach the output.
    std::vector<uint8_t> y2(2 * 8, 0xFF), uv2(2 * 8, 0xFF);
    y2[0] = 10; y2[1] = 20;               // row 0, the two real pixels
    y2[8] = 30; y2[9] = 40;               // row 1
    uv2[0] = 50; uv2[1] = 60;             // one chroma pair
    nv12_to_i420(y2.data(), 8, uv2.data(), 8, 2, 2, out, plane, stride);
    CHECK(plane[0][0] == 10 && plane[0][1] == 20, "row 0 uses width, not stride");
    CHECK(plane[0][2] == 30 && plane[0][3] == 40, "row 1 is at the output width");
    CHECK(plane[1][0] == 50 && plane[2][0] == 60, "chroma pair de-interleaved");

    std::printf("\n%s\n", g_fail ? "FAILURES" : "ALL PASS");
    return g_fail ? 1 : 0;
}
