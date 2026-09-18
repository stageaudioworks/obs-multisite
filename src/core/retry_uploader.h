// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// retry_uploader.h — drains the durable spool to the object store in strict
// sequence order, retrying with exponential backoff + jitter. No segment is
// abandoned while the event is live; a segment is confirmed (and its spool file
// removed) only after the store returns success.
//
#include "link_health.h"
#include "spool_queue.h"
#include "transport.h"
#include <functional>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <cstdint>
#include <optional>

namespace multisite {

struct UploaderConfig {
    int    base_backoff_ms = 250;     // first retry delay
    int    max_backoff_ms  = 15000;   // cap
    double jitter          = 0.30;    // ±30%
    int    max_attempts    = 0;       // 0 = retry forever (production)
    std::string content_type = "video/mp4";
    // Verify (via HEAD) that the first N successful uploads really persisted,
    // with the expected byte count. Catches a store that returns 2xx without
    // storing, and mis-signed requests. 0 disables.
    int verify_first_n = 3;
    std::map<std::string, std::string> tags = { {"MultisiteExpiry", "7d"} };
    // Which target this uploader advances: 0 the primary, 1 the second bucket
    // (PROJECT-SCOPE.md §10 Phase 9). A target-1 uploader confirms as target 1
    // and YIELDS — it uploads nothing while the primary has anything
    // outstanding, so a second copy can never be the reason the live feed
    // suffers. On a link always behind the primary it simply never runs, which
    // is the honest outcome; what it does not finish during the event it
    // finishes afterwards.
    int target = 0;
};

struct UploaderStats {
    std::atomic<uint64_t> confirmed{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> retries{0};
    std::atomic<uint64_t> permanent_failures{0};
    std::atomic<uint64_t> verify_failures{0};
};

// Called after each segment is confirmed durable, so the caller can update the
// manifest (honouring the write-ordering rule: manifest only lists confirmed
// segments).
using ConfirmCallback = std::function<void(const SpooledSegment&)>;

class RetryUploader {
public:
    RetryUploader(SpoolQueue& spool, Transport& transport, UploaderConfig cfg = {});
    ~RetryUploader();

    void set_confirm_callback(ConfirmCallback cb) { m_on_confirm = std::move(cb); }
    // Fires after the spool entry is cleared, so status() counters are accurate.
    void set_post_confirm_callback(ConfirmCallback cb) { m_on_confirmed_after = std::move(cb); }

    void start();
    void stop();

    LinkHealth health() const { return m_health.load(); }
    // A copy, not a reference. The upload thread writes this string while a
    // status poll reads it, and that poll runs on somebody else's thread
    // holding somebody else's lock — the Session's, which the uploader never
    // takes. Handing out a reference copied the buffer on the reader's thread,
    // which is the read that raced.
    std::string last_verify_note() const {
        std::lock_guard<std::mutex> lk(m_note_mtx);
        return m_last_verify_note;
    }
    const UploaderStats& stats() const { return m_stats; }

    // Drain synchronously until the spool is empty or `deadline` passes. Returns
    // true if fully drained. Used by tests and by clean shutdown.
    bool drain_blocking(std::chrono::milliseconds deadline);

private:
    SpoolQueue&    m_spool;
    Transport&     m_transport;
    UploaderConfig m_cfg;
    UploaderStats  m_stats;
    ConfirmCallback m_on_confirm;
    ConfirmCallback m_on_confirmed_after;

    std::string          m_last_verify_note;
    // Guards only the note above. Its own mutex rather than the Session's,
    // because the writer is the upload thread and the reader is a status poll
    // that already holds the Session's. Never held across the confirm
    // callbacks, so the two locks cannot invert.
    mutable std::mutex   m_note_mtx;
    std::thread          m_thread;
    std::atomic<bool>    m_running{false};
    std::atomic<LinkHealth> m_health{LinkHealth::Healthy};

    // The only writer of the note above, so the lock lives in one place.
    void set_verify_note(std::string note) {
        std::lock_guard<std::mutex> lk(m_note_mtx);
        m_last_verify_note = std::move(note);
    }
    void run();
    // Whether this uploader may take a segment right now. Always true for the
    // primary; for the mirror it is the yield rule — see `target` above.
    bool may_upload_now() const;
    // Upload one segment with retry until success/stop/deadline. Returns true
    // on confirm. `deadline` is optional (the background run() thread retries
    // forever); drain_blocking() passes its own deadline through so a stuck
    // segment can't hang shutdown past it.
    bool upload_one(const SpooledSegment& seg,
                    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
    int  backoff_ms(int attempt) const;
};

} // namespace multisite
