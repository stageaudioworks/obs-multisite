// SPDX-License-Identifier: GPL-3.0-or-later
// resume_state.h — what a player restores after a restart, and when it is too
// old to be worth restoring.
//
// Kept pure (no Player, no clock) so the decision is testable: the caller
// passes "now". Anything unreadable or stale means "follow live as configured".
#pragma once

#include <string>

namespace multisite_player {

struct ResumeState {
    std::string event_id;
    bool        pinned = false;
    bool        paused = false;
    long long   media_ms = 0;      // position from the event's start
    long long   saved_wall_ms = 0; // when it was written, Unix ms
};

constexpr long long kResumeMaxAgeMs = 30LL * 60 * 1000;

// Sibling "resume.json" of the config file.
std::string resume_state_path(const std::string& config_path);

// Temp file then rename, as Config::save does, so a crash mid-write never
// leaves a half file under the real name.
bool save_resume_state(const std::string& path, const ResumeState& s,
                       std::string& error);

// false: missing or corrupt.
bool load_resume_state(const std::string& path, ResumeState& out);

// true when `s` should be restored at `now_wall_ms`. A timestamp in the future
// is a wrong clock, not a fresh save, so it is refused too.
bool resume_is_fresh(const ResumeState& s, long long now_wall_ms,
                     long long max_age_ms = kResumeMaxAgeMs);

// true when there is anything to restore beyond what the config already does.
bool resume_worth_restoring(const ResumeState& s);

} // namespace multisite_player
