// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// transport.h — the object-store PUT abstraction the uploader drains through.
//
// The real implementation wraps libcurl + the SigV4 signer against R2/S3. A
// fault-injecting mock implements the same interface so retry/backoff and
// store-and-forward behaviour can be tested without a network.
//
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace multisite {

struct PutResult {
    bool        success = false;
    long        http_status = 0;
    bool        retryable = true;   // false for permanent errors (e.g. 403)
    std::string error;
};

struct GetResult {
    bool        success = false;
    long        http_status = 0;
    bool        retryable = true;
    std::string error;
    std::vector<uint8_t> body;
};

// One object returned by a listing. `size` is -1 when the store omitted it.
struct ListEntry {
    std::string key;
    int64_t     size = -1;
    std::string last_modified;   // ISO-8601, as the store reported it
};

struct ListResult {
    bool        success = false;
    long        http_status = 0;
    bool        retryable = true;
    std::string error;
    // Directory-style groupings when a delimiter was supplied, each still
    // carrying the delimiter (e.g. "events/01J8ZK.../"). This is how event IDs
    // are discovered without listing every segment beneath them.
    std::vector<std::string> common_prefixes;
    std::vector<ListEntry>   keys;
    // A page is a page: a store may cap results well below max_keys, so callers
    // must follow the token rather than assume one request sees everything.
    bool        truncated = false;
    std::string next_continuation_token;
};

// Result of deleting one object. A 404 is still success: the caller wanted the
// object gone, and it already is.
struct DeleteResult {
    bool        success = false;
    long        http_status = 0;
    bool        retryable = true;
    std::string error;
};

// Abstract object-store transport.
class Transport {
public:
    virtual ~Transport() = default;
    // PUT an object. `tags` become x-amz-tagging (e.g. MultisiteExpiry=7d).
    virtual PutResult put(const std::string& key,
                          const std::vector<uint8_t>& body,
                          const std::string& content_type,
                          const std::map<std::string, std::string>& tags) = 0;

    // Optional: report the stored size of an object (HEAD). Returns -1 if the
    // transport can't check or the object is absent. Used to VERIFY that a PUT
    // reported as successful actually persisted the bytes.
    virtual int64_t object_size(const std::string& /*key*/) { return -1; }

    // Fetch an object. Decoders need this; upload-only transports may leave it
    // unimplemented.
    virtual GetResult get(const std::string& /*key*/) {
        GetResult r; r.error = "get() not implemented by this transport";
        r.retryable = false;
        return r;
    }

    // List objects under `prefix`. With a `delimiter` ("/"), keys sharing a
    // path component collapse into common_prefixes instead of being returned
    // individually — the cheap way to enumerate events without walking their
    // segments. Pass a previous result's next_continuation_token to page.
    //
    // Note this needs the s3:ListBucket permission, which object-scoped or
    // read-only credentials often lack; implementations should say so plainly
    // rather than returning an empty list that looks like "no events".
    virtual ListResult list(const std::string& /*prefix*/,
                            const std::string& /*delimiter*/ = "",
                            const std::string& /*continuation_token*/ = "",
                            int /*max_keys*/ = 1000) {
        ListResult r; r.error = "list() not implemented by this transport";
        r.retryable = false;
        return r;
    }

    // Delete one object. Encoder-side storage management. A 404 is success
    // (already gone), so callers can treat this as idempotent.
    virtual DeleteResult remove(const std::string& /*key*/) {
        DeleteResult r;
        r.error = "remove() not implemented by this transport";
        r.retryable = false;
        return r;
    }

    // Abandon whatever request is currently in flight (and refuse to start a
    // new one) as soon as the transport next checks in, rather than letting it
    // run to its full timeout. Call this before joining a thread that might be
    // blocked inside put()/get()/list()/object_size() — S3Transport's own
    // comment tells the story: tearing a decoder source down while a request
    // was in flight once froze OBS's UI thread for as long as that request had
    // left, long enough that an operator force-quit it, which OBS then reports
    // as a crash. The encoder's RetryUploader has the exact same hazard on
    // its own upload thread (see BUGS.md).
    //
    // No-op by default: a mock transport used in tests never blocks, so it
    // has nothing to cancel.
    virtual void cancel_pending() {}
};

} // namespace multisite
