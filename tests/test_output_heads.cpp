// SPDX-License-Identifier: GPL-3.0-or-later
// test_output_heads.cpp — one player, several screens (obs-multisite#29).
//
// What decides which display controller drives which screen, what each
// screen's identity screen calls it, and the `outputs` setting's round trip
// through the config file. The DRM parts need a board; these do not.
#include "output_heads.h"
#include "config.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

int main() {
    std::printf("== A display controller for each screen, each used once ==\n");
    // The RK3588 as the 5B reports it: HDMI-A-1 on its current controller 0,
    // HDMI-A-2 able to use 0, 1 or 2 and on none yet.
    {
        const auto got = assign_crtcs({0b0111, 0b0111}, {0, -1}, 4);
        CHECK(got.size() == 2 && got[0] == 0 && got[1] == 1,
              "the first keeps the controller it has; the second gets the next free one");
    }
    {
        const auto got = assign_crtcs({0b0010, 0b0011}, {-1, -1}, 4);
        CHECK(got[0] == 1 && got[1] == 0, "a screen with one choice gets it; the other takes what is left");
    }
    {
        const auto got = assign_crtcs({0b0001, 0b0001}, {-1, -1}, 4);
        CHECK(got[0] == 0 && got[1] == -1, "two screens that need the same controller: the second has none");
    }
    {
        const auto got = assign_crtcs({0b0110, 0b0110}, {2, 2}, 4);
        CHECK(got[0] == 2 && got[1] == 1, "two claiming the same current one: the first keeps it");
    }
    {
        const auto got = assign_crtcs({0b0001}, {5}, 4);
        CHECK(got[0] == 0, "a current controller it may not use is ignored");
    }

    std::printf("\n== Each screen says which it is ==\n");
    multisite::TileLayout two;
    two.cols = 2;
    CHECK(output_label(0, 2, 0, two) == "OUTPUT 1 OF 2 - LEFT HALF", "2x1, tile 0: the left half");
    CHECK(output_label(1, 2, 1, two) == "OUTPUT 2 OF 2 - RIGHT HALF", "2x1, tile 1: the right half");
    multisite::TileLayout quad;
    quad.cols = 2;
    quad.rows = 2;
    CHECK(output_label(3, 4, 3, quad) == "OUTPUT 4 OF 4 - BOTTOM RIGHT", "2x2, tile 3: the bottom right");
    multisite::TileLayout one;
    CHECK(output_label(0, 2, 0, one) == "OUTPUT 1 OF 2 - WHOLE PICTURE",
          "a one-picture feed on two screens: each shows the whole");
    CHECK(output_label(0, 2, -1, two) == "OUTPUT 1 OF 2 - WHOLE PICTURE", "a screen given no tile");
    CHECK(output_label(0, 1, 0, two).empty(), "a single screen has nothing to be told apart from");

    std::printf("\n== The outputs setting, through the config file ==\n");
    char path[] = "/tmp/test_output_heads_XXXXXX";
    const int fd = mkstemp(path);
    close(fd);
    {
        std::ofstream f(path);
        f << R"({"connector": "HDMI-A-1", "outputs": [{"connector": "HDMI-A-1", "tile": 0},
                 {"connector": "HDMI-A-2", "tile": 1}, {"tile": 3}, "not an object"]})";
    }
    Config c;
    std::string err;
    CHECK(c.load(path, err), "a config with outputs loads");
    CHECK(c.outputs.size() == 2 && c.outputs[0].connector == "HDMI-A-1" && c.outputs[0].tile == 0 &&
          c.outputs[1].connector == "HDMI-A-2" && c.outputs[1].tile == 1,
          "both screens, in order; an entry with no connector, or not an object, is skipped");
    CHECK(c.save(path, err), "it saves");
    Config back;
    back.load(path, err);
    CHECK(back.outputs == c.outputs, "and loads back the same");
    Config plain;
    CHECK(plain.outputs.empty(), "no outputs setting: the one connector, as ever");
    unlink(path);

    std::printf("\n%s\n", g_fail == 0 ? "ALL OUTPUT HEAD TESTS PASSED" : "SOME OUTPUT HEAD TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
