// SPDX-License-Identifier: GPL-3.0-or-later
// rosstalk.h — ProPresenter's RossTalk cues, pure parts.
//
// parse_rosstalk_line() and find_cue_marker_ms() turn a line off the wire into
// a command and a programme position. Nothing here touches the Player or a
// socket, so it is testable.
#pragma once

#include "player.h"

#include <string>
#include <vector>

namespace multisite_player {

enum class RossCmd { Cue, Play, Hold, Live, Unknown };
struct RossCommand { RossCmd kind = RossCmd::Unknown; std::string arg; std::string text; };

// One line, without its newline. Strips \r, an optional leading "CC " (any case),
// surrounding spaces.
RossCommand parse_rosstalk_line(const std::string& line);

// Programme position (ms from event start) of the best match, or -1. Matches label
// trimmed and case-insensitively, any author; among matches, the largest position.
// A marker's position is at_media_ms if >= 0, else at_ms - started_ms when
// at_ms > 0, else it is skipped.
long long find_cue_marker_ms(const std::vector<Status::MarkerEntry>& markers,
                             const std::string& name, long long started_ms);

} // namespace multisite_player
