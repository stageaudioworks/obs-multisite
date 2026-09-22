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

#include "../core/cloud_storage.h"   // CloudRole
#include <string>

#include "../core/cloud_identity.h"

namespace multisite_obs {

// Starts the worker. Safe from any thread; later calls are no-ops.
void reporter_start();
// Stops the worker and joins it. Called from obs_module_unload.
void reporter_stop();

// A ROLE's cloud identity (Phase 12) — one for the encoder, one for the
// decoder, because each role pairs on its own and is its own appliance to the
// collector. A role's storage and that role's heartbeat come from its identity
// and from no other, which is the rule the subsystem exists for. The worker
// owns them: it already owns the collector connection, the clock and the
// network. The Pi appliance has one role and so one identity
// (Player::cloud_identity).
//
// Deliberately no role-less overload. There was one, and it quietly gave the
// decoder the encoder's pairing — read-write, and a different appliance from
// the one the decoder heartbeats as. Making every caller name its role is what
// stops that coming back.
//
// Shared and never null once the worker has started; callers tolerate a null
// before that. The identity's own state is written only on the worker thread.
std::shared_ptr<multisite::CloudIdentity>
reporter_cloud_identity(multisite::CloudRole role);

// Adopt each role's saved collector url/id/token into that role's identity, so
// an already-paired install works with no operator action. Called by the
// worker each tick, cheap and idempotent.
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
