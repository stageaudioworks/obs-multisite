// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// storage_probe.h — "does this bucket actually work?", asked before an event
// rather than during one. See PROJECT-SCOPE.md §10 Phase 9 for the second
// bucket's version of the same idea.
//
// The settings can be filled in perfectly and still not work: a key scoped to
// the wrong bucket, an endpoint copied from the provider's console instead of
// its S3 API, a bucket that was never created, a firewall that drops the port.
// Every one of those is cheap to find now and expensive to find at Go Live, and
// every one of them reads differently in the answer.
//
#include <cstddef>
#include <string>

namespace multisite { struct S3Config; }

namespace multisite_obs {

struct ProbeResult {
    bool        ok = false;         // connected, and able to do what was asked
    bool        answered = false;   // a request reached a server at all
    long        http_status = 0;
    std::string detail;             // as the provider put it, trimmed
    std::string url;                // the endpoint actually used, for spotting typos
    bool        wrote = false;      // a write probe wrote and removed its file
    bool        found_live = false; // a read probe found this room live
};

// `write` decides what is asked of the bucket: a small file written and removed
// (what the main site needs), or the room's live.json read (exactly the request
// a campus will make). Blocking — call it off the UI thread.
ProbeResult probe_bucket(const multisite::S3Config& cfg, bool write,
                         const std::string& room_id);

// A measured burst into a bucket — the only way to learn spare uplink capacity,
// since the live stream only ever produces at its own bitrate and so can never
// show what is left over (PROJECT-SCOPE.md §10 Phase 9).
//
// It lives here, beside the other question asked of a bucket before an event,
// and it takes the bucket as an argument. It used to be `secondary_uplink_test`
// with the second bucket wired in, which filed a measurement of THE LINK under
// one of the things using the link: an operator with no second bucket could not
// measure their uplink at all, and the button was hidden inside a collapsed
// block. Capacity is a property of the connection, not of a destination.
//
// Puts a payload of `bytes`, times it, then removes it. Blocking, and it is
// real traffic: call it OFF the UI thread and never automatically.
struct UplinkTestResult {
    bool        ok = false;
    double      mbps = 0.0;
    std::string error;
};
UplinkTestResult uplink_test(const multisite::S3Config& cfg,
                             size_t bytes = 8u << 20);

// The one place an operator's typed fields become an S3Config. Shared so the
// two docks, the check and the probe cannot normalise a provider differently.
void fill_s3_config(multisite::S3Config& out, const std::string& provider_key,
                    const std::string& account_id, const std::string& endpoint,
                    const std::string& region_field, const std::string& bucket,
                    const std::string& key_id, const std::string& secret);

} // namespace multisite_obs
