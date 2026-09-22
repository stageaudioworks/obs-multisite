// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// reporter.h — the monitoring heartbeat's host side in the OBS plugin.
//
// One worker thread, started once at module load, serving both roles. Every
// tick it snapshots each role's settings (so Apply takes effect without a
// restart), and when a role is enabled AND configured it filters that role's
// own status document through the core allowlists and POSTs it. Disabled or
// unconfigured sends nothing: zero sockets, zero bytes.
//
// Deliberately free of Qt: a build without docks still reports, because the
// machine nobody is sitting at is the one that most needs reporting.
//
#include <memory>
#include <string>

#include "../core/cloud_identity.h"

namespace multisite_obs {

// Starts the worker. Safe from any thread; later calls are no-ops.
void reporter_start();
// Stops the worker and joins it. Called from obs_module_unload.
void reporter_stop();

// The plugin's ONE cloud identity (Phase 12). The worker owns it, because it
// already owns the collector connection, the clock and the network — and the
// encoder output and decoder source read it, so a machine's storage and its
// monitoring use one identity rather than two. Exactly the seam the Pi
// appliance uses (Player::cloud_identity), so the two hosts cannot diverge.
//
// Shared and never null once the worker has started; callers tolerate a null
// before that. The identity's own state is written only on the worker thread.
std::shared_ptr<multisite::CloudIdentity> reporter_cloud_identity();

// Adopt this role's saved collector url/id/token into the identity, so an
// already-paired install works with no operator action. Called by the worker
// each tick, cheap and idempotent.
void reporter_adopt_saved_pairing();

// The last POST outcome for a role ("obs-encoder" / "obs-decoder"), as stable
// words for the docks: "accepted (200)", "rejected: 401 — token unknown",
// "not reached (dropped)", "disabled", "not configured", or "" when nothing
// has been evaluated yet. Stable on purpose — the worker logs only when this
// changes, so a timestamp here would log every tick.
std::string reporter_last_result(const std::string& kind);

// ── Pairing (device-code flow, TELEMETRY.md §4) ─────────────────────────────
// The worker owns the state machine and the polling; the docks only show the
// code and cancel. Phase words for the docks: 0 idle, 1 waiting (a code is on
// screen), 2 done (credentials saved), 3 expired, 4 failed.

// Begin pairing for a role. False when no collector URL is configured —
// pairing needs somewhere to ask, and the URL field is the manual fallback
// that names it. True means the worker has it and the code is coming; the
// dock should Apply first so the typed URL is what gets used.
bool reporter_pair_begin(const std::string& kind);
struct PairView {
    int phase = 0;
    std::string user_code;
    std::string verification_url;
    std::string error;
    std::string note;   // host-side notice, preferred over error when set
    // True once the claimed credentials are persisted, not merely parsed.
    // The docks acknowledge Done only when this is set: filling the fields
    // from a save still in flight is what used to wipe a fresh claim on the
    // next Apply.
    bool saved = false;
};
PairView reporter_pair_view(const std::string& kind);
void reporter_pair_cancel(const std::string& kind);

} // namespace multisite_obs
