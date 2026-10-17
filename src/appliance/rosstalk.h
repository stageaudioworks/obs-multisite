// SPDX-License-Identifier: GPL-3.0-or-later
// rosstalk.h — ProPresenter's RossTalk cues, pure parts.
//
// parse_rosstalk_line() and find_cue_marker_ms() turn a line off the wire into
// a command and a programme position, and next_cue_step() orders a cue. Nothing
// here touches the Player or a socket, so it is testable.
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

enum class CueStep { Start, Seek, Wait, Hold, Done };

struct CueProgress {               // owned by the caller, starts zeroed
    bool start_sent = false, seek_sent = false, target_cleared = false, hold_sent = false;
    unsigned long long frames_at_clear = 0;
    long long started_wall_ms = 0; // the caller stamps it when the cue arrives
    std::string outcome;           // "done" | "no such marker" | "timed out"
};

constexpr long long kCueGiveUpMs = 10 * 1000;
// How near the marker the first picture must be for the cue to count as landed.
constexpr long long kCueLandToleranceMs = 2000;

// Called once per tick until it returns Done. target_ms is find_cue_marker_ms's
// answer (-1 = none). It records Start, Seek and the landing in `p`; the caller
// performs Seek = seek_to_media(seek_ms_for(target_ms)), Hold = pause() and sets
// `hold_sent` when it does.
//
// Start is resume() if the Player is paused, else play(). A paused Player must be
// resumed, not just seeked: seek_to_media() clears m_paused but never resumes the
// DecoderSession, which serves only while Playing, so a seek from a hold never
// delivers a frame.
//
// The Hold waits for a presented video frame near the marker, not just for
// seek_target_ms to clear: that clears on the first item delivered, audio
// included, and a failed seek leaves it at 0, so a stale pre-seek frame could
// also clear it. Outside the tolerance the cue runs to the give-up instead.
CueStep next_cue_step(long long target_ms, const Status& now, CueProgress& p,
                      long long now_wall_ms);

// target 0 -> 1: 0 is seek_to_media's failure value (BUGS #9).
long long seek_ms_for(long long target_ms);

} // namespace multisite_player
