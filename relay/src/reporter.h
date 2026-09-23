// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// reporter.h — the relay's side of Multisite Cloud: pairing, the monitoring
// heartbeat, and the credential lifecycle that storage reads through.
//
// The relay is the THIRD CloudIdentity host, after the OBS plugin and the
// campus player, and it is its own role (ADR-0001): a relay on a machine that
// also runs an encoder pairs separately and shares nothing with it. It reports
// as kind `relay`, which has been a reserved word on the wire since the
// heartbeat was written — "accepted strings with no host yet, so a future host
// adds wiring rather than a protocol change". This is that host.
//
// The POST mechanics mirror src/appliance/reporter.cpp rather than sharing it,
// for the reason that file gives for not sharing with the OBS one: each is
// written against its own host's threads, config and logging, and the part
// that can actually be wrong — what goes on the wire and when — is already
// shared, in src/core/heartbeat_reporter.h.
//
// ONE DIFFERENCE FROM EVERY OTHER HOST. The relay requires read-only
// credentials (ADR-0002). It is the one component deliberately exposed to the
// internet and it never writes, so a read-write set is refused rather than
// used, and the relay says so instead of streaming.
//
#include <memory>
#include <string>

namespace multisite { class CloudIdentity; }

namespace multisite_relay {

class Service;

// Pairing state for the page: phase 0 idle, 1 waiting (a code is on screen),
// 2 done (credentials saved), 3 expired, 4 failed. Words, not the core enum,
// so the page never includes a core header for this.
struct PairView {
    int phase = 0;
    std::string user_code;
    std::string verification_url;
    std::string error;
    std::string note;        // host-side notice, preferred over error when set
    // True once the claim is persisted. The page reloads settings only then:
    // showing Done first and filling the fields from a save still in flight is
    // what wipes a fresh claim on the next save.
    bool saved = false;
};

class Reporter {
public:
    Reporter();
    ~Reporter();

    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

    // Safe to call more than once; only the first call starts the thread.
    void start(Service& service);
    void stop();

    // The last POST outcome as stable words for the page: "accepted (200)",
    // "rejected: 401 — token unknown", "not reached (dropped)", "not paired",
    // or "" when nothing has been evaluated yet. Stable on purpose — the
    // worker logs only when this changes, so a dead collector cannot fill
    // `docker logs` at heartbeat cadence.
    std::string last_result() const;

    // Device-code pairing. begin() returns false when no collector URL is set.
    bool pair_begin(const std::string& collector_url);
    void pair_cancel();
    PairView pair_view() const;

    // The identity the relay's storage reads through. Owned here because this
    // thread already owns the collector connection, the clock and the network
    // — the same reason the player rides its credential lifecycle on its
    // reporter thread.
    multisite::CloudIdentity& cloud_identity();

private:
    void loop();
    void serve_pairing();
    void serve_credentials();
    void serve_heartbeat();

    struct Worker;
    std::unique_ptr<Worker> m_worker;
};

} // namespace multisite_relay
