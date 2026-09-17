// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// update_check.h — does a newer build exist?
//
// One HTTPS request to the GitHub releases API, once per OBS run, off the UI
// thread. It is a notification and nothing more: nothing is downloaded, nothing
// replaces anything, and the request carries no identifier beyond a
// User-Agent naming the plugin. A machine with no route to github.com simply
// gets no answer, which is not an error worth showing.
//
// Whether this happens at all is a machine-wide setting (update.json), not a
// per-role one: the answer cannot sensibly differ between the two docks.
//
#include <string>

namespace multisite_obs {

enum class UpdateState {
    Disabled,    // the operator turned the check off
    Checking,
    UpToDate,
    Newer,       // update_check_latest() names the published tag
    Failed,      // offline, rate-limited, or no usable answer — say nothing
};

// Starts the check, off the UI thread. Safe from any thread, and safe to call
// more than once: only the first call does anything.
void update_check_start(const std::string& current_version);

// What the docks read, several times a second.
UpdateState update_check_state();
std::string update_check_latest();

bool update_check_enabled();
void update_check_set_enabled(bool on);

} // namespace multisite_obs
