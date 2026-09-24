// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// collector_client.h — how a host speaks to the collector: the endpoint paths,
// the HTTP call, and the small result type both hosts were each writing for
// themselves.
//
// WHY THIS EXISTS. Three hosts (the OBS plugin, the Pi player, and any future
// one) each built their own `post_json`, their own `join_url`/`join_path`, and
// hard-coded the endpoint strings at the call sites. Two copies of a URL builder
// is one authority too few — the project's most expensive bug class is one
// quantity derived in more than one place (standards §2), and the four endpoint
// paths drifting apart across hosts is exactly that shape.
//
// The core owns the PATHS and the call shape; the host owns the thread, the
// schedule and the clock, exactly as it does for heartbeat_reporter.
//
// This is the network seam, so it is the one part of the cloud modules that
// needs libcurl. It is deliberately its own target (MULTISITE_COLLECTOR_CURL)
// so a build without curl — a plugin that links OBS's own copy, a test runner —
// can exclude it and still use everything above it.

#include <string>

namespace multisite {

// The endpoint paths, in one place. Each is appended to the collector base,
// which is the operator's field and may carry a trailing slash.
inline constexpr const char* kHeartbeatPath   = "/v1/heartbeat";
inline constexpr const char* kPairStartPath   = "/v1/pair/start";
inline constexpr const char* kPairPollPath    = "/v1/pair/poll";
inline constexpr const char* kCredentialsPath = "/v1/credentials";
// A paired decoder's second credential, for its own cue file in one event
// (multisite-cloud TELEMETRY.md §4, "Cue credentials"; cue_credentials.h).
inline constexpr const char* kCueCredentialsPath = "/v1/credentials/cue";

// Build a collector URL from a base and a path. Trims trailing slashes from the
// base so `https://host/` and `https://host` give the same answer. Pure, so it
// is tested without a socket.
std::string collector_url(const std::string& base, const char* path);

struct HttpResult {
    bool        reached = false;   // a response arrived (any status)
    long        code = 0;
    std::string body;
    int         retry_after_s = 0;  // from a Retry-After header, else 0
};

// POST `payload` as JSON to `url`, optionally bearing `token`.
//
// A monitor must not hold anything hostage: short connect, bounded total, and
// the body is capped rather than buffered without limit. `token` empty sends no
// Authorization header at all — pairing's first request has no token yet, and
// an empty Bearer header is worse than none because some servers read its
// presence as a bad credential.
HttpResult http_post_json(const std::string& url, const std::string& token,
                          const std::string& payload);

// GET `url`, optionally bearing `token`, with the same bounds. Used for the
// credential fetch.
HttpResult http_get_json(const std::string& url, const std::string& token);

// One-time curl initialisation, safe to call from any thread and idempotent.
// Hosts call it before their first request; it is a no-op on later calls.
void collector_http_init();

} // namespace multisite
