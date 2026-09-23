// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// alsa_stall.h — what to do when the sound card stops taking samples.
//
// A playback device can simply stop consuming: on a Rockchip board the HDMI
// audio follows the display connector, and a hotplug event while the player is
// playing left the card with no room for ever. The write that was waiting for
// room held the output's lock, so every status request, the keep-alive thread
// and the picture queued behind it and the whole player froze — found on a
// ROCK 5B from a thread dump on 2026-09-23.
//
// So the wait for room is bounded, and this decides what the bound means: keep
// waiting, restart the stream once, and after that give up on this frame and
// let the audio thread move on. The picture must never wait for a sound card.
// No ALSA here, so it is tested without one (tests/test_alsa_stall.cpp).

namespace multisite_player {

// Each wait for room in the card is at most this long, so a stall is noticed
// in steps rather than all at once.
inline constexpr int kAlsaWaitSliceMs = 100;

// No room for this long, with the device not reporting an error, is a stall.
// Several times any sane buffer, so a card that is merely full is never
// mistaken for one that has stopped.
inline constexpr int kAlsaStallMs = 2000;

enum class StallAction { Wait, Restart, GiveUp };

// `waited_ms` is how long the current write has gone without any room;
// `restarts` is how many times this write has already restarted the stream.
inline StallAction alsa_stall_action(int waited_ms, int restarts) {
    if (waited_ms < kAlsaStallMs) return StallAction::Wait;
    return restarts < 1 ? StallAction::Restart : StallAction::GiveUp;
}

} // namespace multisite_player
