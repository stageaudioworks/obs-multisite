// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// cue_credentials.h — the second credential a paired decoder holds, for the
// one object it may write: its own cue file in the event it is playing.
//
// WHY. A decoder paired to Multisite Cloud reads with a read-only credential,
// but any site may drop a cue, and a cue is an object each author writes for
// itself (events/{event}/cues/{token}.json, PROJECT-SCOPE §7). The provider
// gives a temporary credential a single permission, so the collector mints a
// SECOND one per event — object read-write on exactly one key, which the
// collector chooses (multisite-cloud TELEMETRY.md §4, "Cue credentials"):
//
//   POST /v1/credentials/cue   Bearer {appliance_token}   {"event_id": "…"}
//   200 { …the /v1/credentials fields…, "object_key": "events/…/cues/….json" }
//
// It is fetched when the device JOINS an event, not when a cue is dropped, and
// refreshed like the main credential, so a collector outage mid-service does
// not stop cues. It is used for that one put and nothing else: every read,
// including the merged cue listing and the read half of add_cue's
// read-modify-write, stays on the read-only credential.
//
// Which devices: only one whose STORAGE is Multisite Cloud. A box paired for
// monitoring that reads a bucket it has typed keys for keeps writing cues as it
// always has — a cue credential for the organisation's Cloud bucket would put
// its cue somewhere nobody at that site reads.
//
// Pure logic, no network, like cloud_identity.h: the host owns the thread and
// the request (serve_cue_credentials in cloud_storage.h does it for both).

#include "cloud_identity.h"
#include "transport.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace multisite {

// What DecoderSession::add_cue is handed to write a cue with, when this device
// holds a cue credential: a transport that can put exactly `object_key`, or,
// when it cannot, why not. Defined here rather than beside the S3 writer that
// builds it, so the session stays free of S3 (see CueWriter, cloud_storage.h).
struct CueTarget {
    std::shared_ptr<Transport> tx;          // null = nothing to write with
    std::string                object_key;
    std::string                why;         // set when tx is null
};

// What one POST /v1/credentials/cue came back as.
struct CueCredentialsReply {
    enum class Outcome {
        Ok,           // creds + object_key
        Unreachable,  // no response at all
        Unpaired,     // 403: stop, as for the main credential (401 is Failed,
                      // and retried, as it is there)
        NotADecoder,  // 409: an encoder already writes with its own credential
        BadEvent,     // 400: the event id is wrong — a bug on this side
        Failed,       // anything else, or a body that does not parse
    };
    Outcome     outcome = Outcome::Failed;
    long        http_code = 0;
    Credentials creds;
    std::string object_key;
};

// Parse a reply for `event_id`. An `object_key` outside
// events/{event_id}/cues/*.json is refused as Failed: this device would
// otherwise write wherever a reply told it to, and the collector's own rule is
// that the key sits in that event's cues/.
CueCredentialsReply cloud_parse_cue_credentials(const std::string& body,
                                                long http_code,
                                                const std::string& event_id);

// The request body, {"event_id": "…"}, built by the JSON library rather than
// by hand so an id can never break out of its string.
std::string cue_credentials_request(const std::string& event_id);

// The cue credential for the event being played, per event, with the main
// credential's lifecycle: fetch on joining, refresh at half the remaining
// lifetime, keep the last good set through an outage, stop for good on 403.
//
// Thread-safe. Three threads touch it: the host's poll loop says which event is
// playing (want_event), the host's reporter thread fetches (tick / on_reply),
// and whichever thread drops a cue asks for the set to write with (held).
class CueCredentials {
public:
    // The event this device is playing now; "" for none. Only the wanted event
    // is ever fetched, and entries for events no longer wanted are dropped past
    // a small cap, so a box that has watched a hundred recordings is not
    // refreshing a hundred credentials.
    void want_event(const std::string& event_id);
    std::string wanted_event() const;

    // Whether the reporter thread should fetch now, and for which event.
    struct Due {
        bool        fetch = false;
        std::string event_id;
    };
    Due tick(long long now_ms) const;

    // The outcome of a fetch made for `event_id`.
    void on_reply(const std::string& event_id, const CueCredentialsReply& r,
                  long long now_ms);

    // What a cue dropped now for `event_id` would be written with. `present` is
    // false with `why` set when there is nothing usable — never fetched, the
    // collector refused, or the set has expired — and the caller must not fall
    // back to the read-only credential: that write is refused, and reporting
    // the refusal as a bucket fault is the mistake this avoids.
    struct Held {
        bool        present = false;
        Credentials creds;
        std::string object_key;
        std::string why;
    };
    Held held(const std::string& event_id, long long now_ms) const;

    // The pairing was cleared or replaced: everything held belonged to it.
    void reset();

private:
    struct Entry {
        Credentials creds;
        std::string object_key;
        long long   next_fetch_at_ms = 0;   // 0 = fetch on the next tick
        bool        stopped = false;        // 400 / 409: never retried
        std::string why;                    // the last failure, for held()
    };
    mutable std::mutex           m_mtx;
    std::string                  m_wanted;
    std::map<std::string, Entry> m_entries;
    bool                         m_unpaired = false;
};

} // namespace multisite
