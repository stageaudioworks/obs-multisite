// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// output_heads.h — one player, several screens (obs-multisite#29).
//
// A composited feed (a 2x1 tile layout, 3840x1080) goes out as two screens
// from one download and one decode: each connector is a "head" of the one
// DRM output, with its own display controller (CRTC), mode and buffers, and
// shows one tile of the same decoded frame. The DRM parts live in
// drm_output.cpp; what is here has no DRM in it, so it is tested without a
// display.
//
#include "../core/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace multisite_player {

// A display controller for each head, each used once. `possible[i]` is the
// bitmask of CRTC indices head i's connector can be driven by (its encoders'
// possible_crtcs, OR'd); `current[i]` the CRTC index it is driven by now, or
// -1. A head keeps the CRTC it has where it can, so a box that has been on
// one screen does not swap controllers under it; otherwise the lowest free
// one. -1 for a head no free controller can drive.
std::vector<int> assign_crtcs(const std::vector<uint32_t>& possible,
                              const std::vector<int>& current, int crtc_count);

// The line on each screen's identity screen saying which one it is, so a
// swapped cable is obvious at install: "OUTPUT 1 OF 2 - LEFT HALF". Empty for
// a single screen, which has nothing to be told apart from.
std::string output_label(int head, int heads, int tile, const multisite::TileLayout& layout);

} // namespace multisite_player
