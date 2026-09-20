// SPDX-License-Identifier: GPL-3.0-or-later
#include "retry_uploader.h"
#include "log.h"
#include <random>
#include <cmath>
#include <algorithm>

namespace multisite {

// Human-readable note about the most recent verification attempt.
RetryUploader::RetryUploader(SpoolQueue& spool, Transport& transport,
                             UploaderConfig cfg)
    : m_spool(spool), m_transport(transport), m_cfg(std::move(cfg)) {}

RetryUploader::~RetryUploader() { stop(); }

int RetryUploader::backoff_ms(int attempt) const {
    // exponential: base * 2^(attempt-1), capped, with ± jitter
    double base = (double)m_cfg.base_backoff_ms * std::pow(2.0, std::max(0, attempt - 1));
    double capped = std::min(base, (double)m_cfg.max_backoff_ms);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> d(1.0 - m_cfg.jitter, 1.0 + m_cfg.jitter);
    return (int)std::llround(capped * d(rng));
}

bool RetryUploader::upload_one(const SpooledSegment& seg,
                                std::optional<std::chrono::steady_clock::time_point> deadline) {
    int attempt = 0;
    while (m_running) {
        // A retry can sit here for a long time (that is the whole point of
        // an outage). If the spool evicted THIS segment for disk space while
        // we were retrying it — see SpoolQueue's disk cap — abandon it rather
        // than risk a stale attempt succeeding after the fact and publishing
        // into the manifest a segment the spool has already declared gone.
        if (seg.seq < m_spool.floor()) return false;
        if (deadline && std::chrono::steady_clock::now() >= *deadline) return false;
        ++attempt;
        PutResult r = m_transport.put(seg.key, seg.data, m_cfg.content_type, m_cfg.tags);
        if (r.success && m_cfg.verify_first_n > 0 &&
            (int)m_stats.confirmed.load() < m_cfg.verify_first_n) {
            // Trust nothing: confirm the bytes are actually in the store.
            int64_t stored = m_transport.object_size(seg.key);
            if (stored >= 0 && stored != (int64_t)seg.data.size()) {
                m_stats.verify_failures++;
                set_verify_note("stored size " + std::to_string(stored) +
                    " != sent " + std::to_string(seg.data.size()) +
                    " for " + seg.key);
            } else if (stored < 0) {
                m_stats.verify_failures++;
                set_verify_note(
                    "object not found after a successful PUT: " + seg.key);
            } else {
                set_verify_note("verified " + seg.key + " (" +
                    std::to_string(stored) + " bytes)");
            }
        }
        if (r.success) {
            // Order matters for durability: publish the manifest entry FIRST,
            // then drop the spool file. If we crashed the other way round the
            // object would exist in the bucket but never be listed, leaving a
            // silent gap. This way a crash simply re-uploads (PUT is idempotent)
            // and the manifest de-duplicates by seq.
            m_stats.confirmed++;
            m_stats.bytes += seg.data.size();
            // Sampled BEFORE the assignment below, because the recovery line
            // asks "was the link unwell until now?" and reading it afterwards
            // always answers no. Same mistake as the clock diagnostic in
            // BUGS #2 — a value checked after it has been set.
            const LinkHealth was = m_health;
            m_health = LinkHealth::Healthy;
            // Publish the manifest entry BEFORE clearing the spool file (a
            // crash in between must not orphan the object), then clear it so
            // status counters reflect reality for the confirm callback.
            if (m_on_confirm) m_on_confirm(seg);
            // Recovery is worth exactly one line, and only when there was
            // something to recover from. Without it a log shows an outage
            // starting and never ending, which reads far worse than it was —
            // and the case that matters most is the link coming back and the
            // NEXT segment succeeding first try, which `attempt` alone misses.
            if (attempt > 1 || was != LinkHealth::Healthy)
                log_info("upload: %s confirmed after %d attempt(s) — link healthy",
                         seg.key.c_str(), attempt);
            m_spool.confirm(seg.seq, m_cfg.target);
            if (m_on_confirmed_after) m_on_confirmed_after(seg);
            return true;
        }
        if (!r.retryable) {
            // Permanent error (e.g. auth). Don't spin forever on this segment;
            // surface it and stop draining so the operator can fix credentials.
            m_stats.permanent_failures++;
            m_health = LinkHealth::Offline;
            // ERROR, not warn, and never rate-limited: this one stops the drain
            // and will not fix itself. An operator who sees the queue frozen
            // needs the provider's own words, because the answer is almost
            // always a key scoped to the wrong bucket.
            log_error("upload: %s refused permanently — HTTP %ld%s%s. "
                      "Uploading has stopped; check the storage credentials.",
                      seg.key.c_str(), r.http_status,
                      r.error.empty() ? "" : " — ", r.error.c_str());
            return false;
        }
        m_stats.retries++;
        const LinkHealth health_before = m_health;
        m_health = (attempt >= 2) ? LinkHealth::Offline : LinkHealth::Degraded;

        if (m_cfg.max_attempts > 0 && attempt >= m_cfg.max_attempts) {
            log_warn("upload: gave up on %s after %d attempt(s) — HTTP %ld%s%s",
                     seg.key.c_str(), attempt, r.http_status,
                     r.error.empty() ? "" : " — ", r.error.c_str());
            return false;
        }

        int wait = backoff_ms(attempt);
        // The FIRST failure of a segment, and any change of health, get a line.
        // Every subsequent retry of the same segment does not: a long outage
        // would otherwise write a line every few seconds and bury the one that
        // says when it started. The attempt number and the backoff are in here
        // because "it retried" is not the question — "how far behind is this
        // getting" is.
        if (attempt == 1 || m_health != health_before) {
            log_warn("upload: %s failed (attempt %d) — HTTP %ld%s%s. "
                     "Retrying in %d ms; link is now %s.",
                     seg.key.c_str(), attempt, r.http_status,
                     r.error.empty() ? "" : " — ", r.error.c_str(), wait,
                     m_health == LinkHealth::Offline ? "offline" : "degraded");
        }
        // sleep in small slices so stop() and a drain deadline are responsive
        for (int slept = 0; slept < wait && m_running; slept += 25) {
            if (deadline && std::chrono::steady_clock::now() >= *deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    return false;
}

void RetryUploader::run() {
    while (m_running) {
        auto next = m_spool.peek_next(m_cfg.target);
        if (!next) {
            m_health = LinkHealth::Healthy;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // The mirror's yield (PROJECT-SCOPE.md §10 Phase 9). Checked per
        // segment rather than once at start-up, because the primary falling
        // behind mid-drain is exactly when the second copy must stop — it
        // interleaves with the feed instead of competing with it. Waiting here
        // is not a fault, so the health readout stays healthy.
        if (!may_upload_now()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        // Strict in-order: if this one hits a permanent failure, pause the loop
        // briefly rather than reordering past it.
        if (!upload_one(*next) && m_running)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

bool RetryUploader::may_upload_now() const {
    return !m_cfg.may_upload || m_cfg.may_upload();
}

void RetryUploader::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void RetryUploader::stop() {
    if (!m_running.exchange(false)) return;
    // Without this, joining a thread blocked inside a transport's put() (an
    // up-to-30-second timeout, nothing else watching it) could hang shutdown
    // for however long that one request had left — the same hazard the
    // decoder had on teardown (see BUGS.md). The default no-op is fine for a
    // transport that never blocks, such as the mocks the test suite uses.
    m_transport.cancel_pending();
    if (m_thread.joinable()) m_thread.join();
}

bool RetryUploader::drain_blocking(std::chrono::milliseconds deadline) {
    auto end = std::chrono::steady_clock::now() + deadline;
    bool was_running = m_running.load();
    if (!was_running) m_running = true; // allow upload_one loops
    while (std::chrono::steady_clock::now() < end) {
        auto next = m_spool.peek_next(m_cfg.target);
        if (!next) { if (!was_running) m_running = false; return true; }
        // A mirror that is yielding cannot drain, and waiting here would hold
        // shutdown open on a primary that may never catch up. It finishes after
        // the event instead — which is the bargain the yield rule makes.
        if (!may_upload_now()) break;
        // Pass the deadline through: upload_one's own retry backoff must not
        // be allowed to run past it, or a single stuck segment hangs shutdown
        // indefinitely (max_attempts is 0 — retry forever — in production).
        if (!upload_one(*next, end)) break; // permanent failure, deadline, or attempts exhausted
    }
    if (!was_running) m_running = false;
    return m_spool.pending_count() == 0;
}

} // namespace multisite
