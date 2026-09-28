// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// screen.h — who owns the display on a given tick.
//
// The poll thread asks this once a round: leave the screen alone, or put an
// idle screen up — and if so, which one. Everything about the answer is
// visible above the box's own screen, so it is worth being exact about.
//
// It is a free function rather than part of Player because the rule is the one
// thing here that can be got wrong without anything to test it against: the
// failure it guards against (an operator pressing Hold and the picture being
// replaced by the box's own address) needs a live event, a decoder and a
// display to reproduce by hand, and none of those may be in a test. Player
// supplies the state; this decides.
//
#include "config.h"

namespace multisite_player {

enum class ScreenAction {
    Leave,    // touch nothing: a picture is arriving, or one is being held
    Blank,    // idle, in black
    Still,    // idle, showing the holding slide
    Splash,   // idle, showing this box's details
};

// `frames_arriving` — a frame reached the display within the last couple of
//   seconds, so there is an event on and the idle screen has nothing to say.
// `holding_picture` — the operator pressed Hold while playback was running.
// `has_frame` — any frame has reached the display since the box started.
// `identity_until_video` — until that first frame, the identity screen,
//   whatever `mode` says (Config::identity_until_video).
ScreenAction screen_action(IdleMode mode, bool frames_arriving,
                           bool holding_picture, bool has_frame,
                           bool identity_until_video = false);

} // namespace multisite_player
