// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// cloud_identity.h — one owner of "who this device is to the collector", and of
// the storage credentials that follow from being paired.
//
// WHY THIS EXISTS. A device could heartbeat to the collector under its appliance
// identity while reading storage with a key an operator pasted by hand — two
// identities, one machine. Splitting the two is how that stays true, so the
// storage credentials and the heartbeat both come from this one place. See
// docs/scope/spec-cloud-identity.md.
//
// WHAT IT OWNS. The pairing state (reused from heartbeat_reporter.h, not
// rewritten), and the credential lifecycle: GET {collector}/v1/credentials with
// the appliance bearer, on boot and again before expires_at, refreshing at
// roughly half the TTL.
//
// WHAT IT DOES NOT. The Transport that uses the credentials, the heartbeat
// cadence and payload, and the choice of bucket — the collector's reply is the
// answer and is never guessed.
//
// Portable: no OBS, no Qt, no curl, no clock. Hosts own the thread, the HTTP and
// the time; the arithmetic and the state transitions are covered by
// tests/test_cloud_identity.cpp.

#include <string>

#include "heartbeat_reporter.h"

namespace multisite {

// Whether credentials are usable, and whether the set behind them is fresh.
// `from_last_good` is the distinction that matters: a consumer must never see a
// bucket without also seeing whether the credentials behind it are live.
struct Credentials {
    std::string bucket;
    std::string endpoint;
    // Temporary credentials: an access key ID and secret, plus the session
    // token that goes with them. The collector returns ALL THREE (measured
    // 2026-09-22), not a token alone — so a consumer needs the pair as well as
    // the token, or the signature is made with nothing.
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;   // sent onward as X-Amz-Security-Token
    long long   expires_at_ms = 0;
    bool        read_write = false;
    bool        from_last_good = false;   // a fetch failed; this is the old set

    bool live(long long now_ms) const {
        return !from_last_good && !bucket.empty() && now_ms < expires_at_ms;
    }
    bool present() const { return !bucket.empty(); }
};

// The credential fetch's shape, host-owned HTTP. Parsers only; never throw.
// Separate from the pairing parsers because this is a different endpoint and a
// different reply.
struct CredentialsReply {
    bool        ok = false;
    Credentials creds;
    // 403 is its own outcome, not a generic failure: the device was unpaired
    // and must STOP rather than retry.
    bool        unpaired = false;
};
CredentialsReply cloud_parse_credentials(const std::string& body, int http_code);

// ── The lifecycle ───────────────────────────────────────────────────────────

// The collector's `expires_at` is an ISO-8601 UTC instant with milliseconds —
// "2026-09-22T08:18:03.586Z" — MEASURED from the live service 2026-09-22, not
// assumed. (The spec first guessed an integer of milliseconds; the probe showed
// a string, and reading it as an integer yielded 0, i.e. every credential set
// treated as already expired.) Returns epoch milliseconds, or 0 when the string
// is absent or unparseable — 0 is "no expiry known", which no live() call
// accepts, so a malformed timestamp fails safe.
long long cloud_parse_iso8601_ms(const std::string& iso);

// When to fetch again: half the remaining TTL, so a refresh lands well before
// expiry and one failed attempt still leaves room for the next. Never returns 0
// for a live credential set, and never schedules past the expiry it is
// refreshing against.
long long cloud_next_refresh_ms(const Credentials& c, long long now_ms);

// What the state machine wants the host to do next. One decision per tick, so
// the host never has to re-derive it.
enum class CloudAction {
    Idle,          // nothing to do
    Fetch,         // GET /v1/credentials with the appliance bearer
};

class CloudIdentity {
public:
    // The pairing this device completed, or an earlier one loaded from
    // settings. Empty id/token means unpaired.
    void set_enrolment(const std::string& collector_url,
                       const std::string& appliance_id,
                       const std::string& appliance_token);
    bool paired() const { return !m_id.empty() && !m_token.empty() &&
                                 !m_url.empty(); }
    const std::string& collector_url() const { return m_url; }
    const std::string& appliance_id() const { return m_id; }
    const std::string& appliance_token() const { return m_token; }

    // What to do at `now_ms`. Fetch on boot, then again at the refresh time.
    CloudAction tick(long long now_ms) const;

    // The outcome of a fetch, decided as a whole (standards §2: one function
    // reads both the reply and the clock).
    //
    //  - ok: the new set replaces the old and is not marked last-good.
    //  - 403: the device was unpaired. Stop — no further fetch is scheduled,
    //    and a running event is NOT torn down (last-good covers it to expiry).
    //  - anything else: the last-good set is KEPT and marked stale. Nothing is
    //    cleared, so a collector outage cannot stop a live event.
    void on_credentials(const CredentialsReply& r, long long now_ms);

    // The current set. from_last_good tells the caller whether it is fresh.
    Credentials credentials() const { return m_creds; }

    // True once a 403 has been seen: fetching has stopped and the device needs
    // an operator to pair again.
    bool unpaired() const { return m_unpaired; }
    const std::string& error() const { return m_error; }

    // Clear everything — Disconnect. Typed keys are not this class's concern.
    void reset();

private:
    std::string m_url, m_id, m_token;
    Credentials m_creds;
    bool        m_unpaired = false;
    // 0 means "not fetched yet": the first tick must fetch. Set from the reply
    // so a boot with last-good credentials still refreshes on schedule.
    long long   m_next_fetch_at_ms = 0;
    std::string m_error;
};

} // namespace multisite
