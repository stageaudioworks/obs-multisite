// SPDX-License-Identifier: GPL-3.0-or-later
// test_idle_screen.cpp — Hold must win over the idle screen.
//
// An operator pressing Hold is asking for the frame in front of them to stay.
// The idle screen is for having nothing to show — waiting for the main site, or
// stopped. Those two collided: holding the picture was recognised only when the
// box's own idle mode was "hold", so on a box left on the default identity
// screen, Hold put the box's address and state up in place of the picture the
// operator had just asked to freeze. On screen that reads as the player having
// died, which is the worst thing to show somebody mid-event.
//
// The rule is a pure function, so the whole matrix is checked here with no
// display, no decoder and no network. That is the point: the collision needed a
// live event, a running decoder and an HDMI socket to reproduce by hand, which
// is exactly how it reached a congregation in the first place.
#include "screen.h"

#include <cstdio>
#include <cstring>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

static const char* name(IdleMode m) {
    switch (m) {
    case IdleMode::Black:     return "black";
    case IdleMode::HoldFrame: return "hold";
    case IdleMode::Splash:    return "splash";
    case IdleMode::Image:     return "image";
    }
    return "?";
}

static const char* name(ScreenAction a) {
    switch (a) {
    case ScreenAction::Leave:  return "leave the screen alone";
    case ScreenAction::Blank:  return "blank";
    case ScreenAction::Still:  return "holding slide";
    case ScreenAction::Splash: return "splash";
    }
    return "?";
}

// The state is printed with every case, so a failure names the combination that
// is wrong rather than just the expectation that did not hold.
static void expect(IdleMode mode, bool arriving, bool holding, bool has_frame,
                   ScreenAction want, const char* what) {
    const ScreenAction got = screen_action(mode, arriving, holding, has_frame);
    if (got == want) {
        std::printf("  [ok]   %s\n", what);
        return;
    }
    std::printf("  [FAIL] %s\n"
                "         idle=%s arriving=%d holding=%d has_frame=%d\n"
                "         got %s, wanted %s\n",
                what, name(mode), (int)arriving, (int)holding, (int)has_frame,
                name(got), name(want));
    ++g_fail;
}

static const IdleMode kModes[] = { IdleMode::Black, IdleMode::HoldFrame,
                                   IdleMode::Splash, IdleMode::Image };

int main() {
    std::printf("== A picture arriving owns the screen, whatever the idle mode ==\n");
    for (const IdleMode m : kModes) {
        char what[96];
        std::snprintf(what, sizeof what,
                      "frames arriving, idle=%s: the screen is not touched",
                      name(m));
        expect(m, /*arriving*/ true, /*holding*/ false, /*has_frame*/ true,
               ScreenAction::Leave, what);
    }

    std::printf("\n== Hold beats the idle screen, whatever it is set to ==\n");
    for (const IdleMode m : kModes) {
        char what[128];
        std::snprintf(what, sizeof what,
                      "holding a picture, idle=%s: the held frame stays", name(m));
        expect(m, /*arriving*/ false, /*holding*/ true, /*has_frame*/ true,
               ScreenAction::Leave, what);
    }

    std::printf("\n== The failure this guards against, spelled out ==\n");
    // The default idle mode (config.h) and the one the installer writes. This
    // single call is the bug: pressing Hold used to bring the identity screen
    // up over the picture being held.
    expect(IdleMode::Splash, false, true, true, ScreenAction::Leave,
           "default idle screen (splash) + Hold: the held picture stays");

    std::printf("\n== Waiting for the main site: nothing to hold, so say where the box is ==\n");
    expect(IdleMode::Splash, false, false, false, ScreenAction::Splash,
           "splash idle, nothing decoded yet: the identity screen");
    expect(IdleMode::Black, false, false, false, ScreenAction::Blank,
           "black idle, nothing decoded yet: black");
    expect(IdleMode::Image, false, false, false, ScreenAction::Still,
           "slide idle, nothing decoded yet: the holding slide");
    // "Hold the last picture" with no last picture: the identity screen at least
    // says which box has come up with nothing.
    expect(IdleMode::HoldFrame, false, false, false, ScreenAction::Splash,
           "hold idle, nothing decoded yet: the identity screen, not a blank");

    std::printf("\n== Stopped, and frames have simply stopped arriving ==\n");
    // Stop is a deliberate act, so the screen is allowed to say so — this is the
    // behaviour the Hold fix must not disturb.
    expect(IdleMode::Splash, false, false, true, ScreenAction::Splash,
           "stopped on the splash idle: the identity screen");
    expect(IdleMode::Black, false, false, true, ScreenAction::Blank,
           "stopped on black: black");
    expect(IdleMode::Image, false, false, true, ScreenAction::Still,
           "stopped on a slide: the holding slide");
    expect(IdleMode::HoldFrame, false, false, true, ScreenAction::Leave,
           "stopped on 'hold the last picture': the last frame stays up");

    std::printf("\n== Nothing was ever decoded, so Hold has nothing to hold ==\n");
    expect(IdleMode::Splash, false, true, false, ScreenAction::Splash,
           "Hold pressed before any frame: the identity screen is still allowed");
    expect(IdleMode::Black, false, true, false, ScreenAction::Blank,
           "Hold pressed before any frame, black idle: black, not a held nothing");

    std::printf("\n== Releasing Hold hands the screen back to the idle mode ==\n");
    expect(IdleMode::Splash, false, false, true, ScreenAction::Splash,
           "Hold released and frames stopped: the identity screen returns");
    expect(IdleMode::Black, false, false, true, ScreenAction::Blank,
           "Hold released and frames stopped, black idle: black returns");

    std::printf("\n== Until the first picture, the identity screen (identity_until_video) ==\n");
    for (const IdleMode m : kModes) {
        char what[128];
        std::snprintf(what, sizeof what, "idle=%s, nothing shown yet: the identity screen", name(m));
        CHECK(screen_action(m, false, false, false, true) == ScreenAction::Splash, what);
        std::snprintf(what, sizeof what, "idle=%s, after a picture: the idle mode as ever", name(m));
        CHECK(screen_action(m, false, false, true, true) == screen_action(m, false, false, true, false), what);
    }
    CHECK(screen_action(IdleMode::Black, true, false, true, true) == ScreenAction::Leave,
          "a picture arriving still owns the screen");
    CHECK(screen_action(IdleMode::Black, false, false, false, false) == ScreenAction::Blank,
          "off (the default), a box told to idle in black stays black from the start");

    std::printf("\n== Every combination is decided, and none falls through ==\n");
    // A mode added later without a case in the switch would land on the splash
    // (the fallback), which is the safest of the four but not obviously so, and
    // a combination that answers "leave the screen alone" with nothing on it
    // would leave the box on whatever was there before. So the whole input space
    // is swept rather than only the cases somebody thought to write down.
    int swept = 0, sweep_bad = 0;
    for (const IdleMode m : kModes)
        for (int arriving = 0; arriving < 2; ++arriving)
            for (int holding = 0; holding < 2; ++holding)
                for (int has_frame = 0; has_frame < 2; ++has_frame) {
                    const ScreenAction got = screen_action(
                        m, arriving != 0, holding != 0, has_frame != 0);
                    ++swept;
                    if (got == ScreenAction::Leave && !arriving &&
                        !holding && !has_frame) {
                        std::printf("  [FAIL] idle=%s: nothing decoded and "
                                    "nothing held, yet the screen was left "
                                    "alone — the idle screen would never come up\n",
                                    name(m));
                        ++sweep_bad;
                    }
                }
    CHECK(sweep_bad == 0, "all combinations return a usable action");
    std::printf("     (combinations swept: %d)\n", swept);

    std::printf("\n%s\n", g_fail == 0 ? "ALL IDLE SCREEN TESTS PASSED"
                                      : "SOME IDLE SCREEN TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
