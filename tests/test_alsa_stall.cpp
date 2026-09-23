// SPDX-License-Identifier: GPL-3.0-or-later
//
// test_alsa_stall.cpp — the bound on waiting for a sound card that has stopped.
#include "alsa_stall.h"

#include <cstdio>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else std::printf("  [ok]   %s\n", m); } while (0)

int main() {
    std::printf("A card with no room: wait, restart once, then give up\n");
    CHECK(alsa_stall_action(0, 0) == StallAction::Wait, "no wait yet: keep waiting");
    CHECK(alsa_stall_action(kAlsaStallMs - kAlsaWaitSliceMs, 0) == StallAction::Wait,
          "just under the bound: still waiting");
    CHECK(alsa_stall_action(kAlsaStallMs, 0) == StallAction::Restart,
          "at the bound, first time: restart the stream");
    CHECK(alsa_stall_action(kAlsaStallMs, 1) == StallAction::GiveUp,
          "at the bound again after a restart: give up on this frame");
    CHECK(alsa_stall_action(kAlsaStallMs * 10, 1) == StallAction::GiveUp,
          "never waits past the second bound");
    CHECK(kAlsaStallMs >= 1000, "the bound is far longer than any sane buffer");
    CHECK(kAlsaWaitSliceMs * 5 <= kAlsaStallMs, "a stall is noticed in steps");

    std::printf(g_fail ? "alsa_stall: %d failed\n" : "alsa_stall: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
