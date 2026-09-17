// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// update_check.h — does a newer build exist?
//
// One HTTPS request to the GitHub releases API, once per run, off the player's
// own threads. Notification only: nothing is downloaded and nothing installs
// itself. A box with no route to github.com gets no answer, which is not an
// error worth showing to anyone.
//
// Deliberately not shared with the plugin's copy: that one is written against
// OBS's thread and logging conventions and this one against the player's, and
// they have nothing else in common. The part that can be wrong — is one tag
// newer than another — IS shared, in src/core/version.cpp.
//
#include <string>

namespace multisite_player {

struct UpdateInfo {
    bool        checked = false;   // an answer came back
    bool        newer   = false;   // …and it names a newer build
    std::string latest;            // the published tag, when there is one
};

// Starts the check in the background. Safe to call more than once; only the
// first call does anything. `enabled` is the operator's setting — false leaves
// it alone and makes no request at all.
void update_check_start(const std::string& current, bool enabled);

UpdateInfo update_check_info();

} // namespace multisite_player
