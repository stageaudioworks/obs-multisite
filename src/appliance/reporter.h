// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// reporter.h — the monitoring heartbeat's host side on the campus player.
//
// Owned by Player and run on its own thread: every tick it snapshots the
// config (so Save takes effect without a restart), and when reporting is
// enabled AND configured it filters the player's own status document through
// the core allowlists and POSTs it. Disabled or unconfigured sends nothing:
// zero sockets, zero bytes.
//
// The POST mechanics deliberately mirror src/obs/reporter.cpp rather than
// sharing it — the same reason the two update checks are separate (see
// update_check.h): one is written against OBS's threads and logging, this one
// against the player's, and they have nothing else in common. The part that
// can be wrong — what goes on the wire and when — IS shared, in
// src/core/heartbeat_reporter.h.
//
#include <memory>
#include <string>

namespace multisite_player {

class Player;

// Pairing state for the page: phase 0 idle, 1 waiting (a code is on screen),
// 2 done (credentials saved), 3 expired, 4 failed. Words, not the core enum,
// so the page never includes a core header for this.
struct PairView {
    int phase = 0;
    std::string user_code;
    std::string verification_url;
    std::string error;
    std::string note;   // host-side notice, preferred over error when set
};

class Reporter {
public:
    Reporter();
    ~Reporter();

    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    // Safe to call more than once; only the first call starts the thread.
    // Safe to call again after stop().
    void start(Player& player);
    void stop();

    // The last POST outcome, as stable words for the page: "accepted (200)",
    // "rejected: 401 — token unknown", "not reached (dropped)", "disabled",
    // "not configured", or "" when nothing has been evaluated yet. Stable on
    // purpose — the worker logs only when this changes.
    std::string last_result() const;

    // Device-code pairing. False from begin when no collector URL is
    // configured. The worker polls and persists the claim; the page shows
    // the code from view() and stops it with cancel().
    bool pair_begin();
    void pair_cancel();
    PairView pair_view() const;

private:
    void loop();
    void serve_once();
    void serve_pairing();

    struct Worker;
    std::unique_ptr<Worker> m_worker;
};

} // namespace multisite_player
