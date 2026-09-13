// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// spool_queue.h — durable, crash-safe store-and-forward queue.
//
// The encoder writes each finished segment to disk *before* it is uploaded, so
// nothing is lost if the network drops, OBS crashes, or the machine loses power.
// Segments are drained in strict sequence order; a segment's spool file is only
// removed once the upload is confirmed durable in the bucket.
//
// That guarantee is unconditional only when `max_bytes` is 0. Given a nonzero
// cap, an upload link that stays down (or too slow to keep up) for long
// enough will make enqueue() start deleting its own oldest unconfirmed
// segments to keep the backlog off the local disk — see the constructor and
// SpoolDrop. This is a deliberate, bounded trade of "never lose a frame" for
// "never fill the operator's disk"; it does not fire in normal operation.
//
// On-disk layout (all under `dir`):
//   state.json                 event_id, first_seq, last_enqueued, last_confirmed, ended
//   <seq:08d>.seg              raw segment bytes (written tmp+rename → crash-safe)
//   <seq:08d>.meta             JSON sidecar: seq, duration_s, pts_offset_s, checksum, key
//
// This is deliberately dependency-free (no SQLite): plain files with atomic
// write-then-rename give the durability guarantees we need and are trivial to
// reason about and test.
//
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <functional>

namespace multisite {

struct SpooledSegment {
    uint64_t             seq = 0;
    std::vector<uint8_t> data;
    double               duration_s = 6.0;
    double               pts_offset_s = 0.0;
    std::string          checksum;   // sha256 hex, filled on enqueue
    std::string          key;        // object key it will be uploaded to
};

struct SpoolState {
    std::string event_id;
    // Lowest sequence number still guaranteed obtainable from this spool.
    // Starts as the event's first seq and only ever increases: an eviction
    // under the disk-space cap (see SpoolQueue::SpoolQueue) advances it past
    // whatever it just discarded, so nothing downstream waits forever on a
    // segment declared gone.
    uint64_t    first_seq      = 0;
    uint64_t    last_enqueued  = 0;   // highest seq written to spool
    uint64_t    last_confirmed = 0;   // highest seq confirmed durable in bucket
    bool        ended          = false;
    bool        valid          = false; // false if no prior state on disk
    // Wall-clock time of the last enqueue() or confirm() — i.e. the last sign
    // of life this event showed. Set on every write, not just the first, so
    // it answers "how long has this been quiet", not "how old is it". See
    // PROJECT-SCOPE.md §5.1: this is what tells a genuine crash-and-restart,
    // minutes old, apart from a stale leftover event nobody cleaned up.
    int64_t     last_activity_ms = 0;
};

// Result of opening a spool dir: tells the caller whether a prior, unfinished
// event is present and can be resumed.
struct ResumeInfo {
    bool        resumable = false;
    std::string event_id;
    uint64_t    last_confirmed = 0;
    uint64_t    last_enqueued  = 0;
    size_t      pending_count  = 0;   // segments on disk not yet confirmed
    // Raw timestamp only — SpoolQueue has no opinion on what counts as
    // stale, since that threshold is a Session-level policy
    // (SessionConfig::resume_stale_after_ms). See Session::check_resumable().
    int64_t     last_activity_ms = 0;
    // Always false from SpoolQueue::inspect() itself. Session::check_resumable()
    // sets this before returning, once it has a threshold to compare against.
    bool        stale = false;
};

// A segment evicted from the spool before it was ever uploaded, because the
// backlog outgrew `max_bytes`. `new_floor` is the lowest sequence number
// still guaranteed to exist; anything below it is gone for good.
struct SpoolDrop {
    uint64_t seq = 0;
    uint64_t new_floor = 0;
};

class SpoolQueue {
public:
    // `max_bytes` bounds how much unconfirmed data the spool keeps on disk —
    // 0 (the default) preserves the original "retry forever, never lose a
    // segment" behaviour. A nonzero cap protects the local disk from filling
    // during a long or badly degraded upload link, at the cost of dropping the
    // OLDEST still-unconfirmed segment (the one furthest from being resolved)
    // rather than the newest: it is the choice most likely to already be
    // stale to a live viewer, and the one that unblocks the strict in-order
    // uploader fastest.
    explicit SpoolQueue(std::string dir, uint64_t max_bytes = 0);

    // Inspect an existing spool without starting a new event. Used at startup to
    // drive the "resume previous event, or start new?" prompt.
    ResumeInfo inspect() const;

    // Begin a fresh event: clears any old spool and writes new state.
    void begin_event(const std::string& event_id, uint64_t first_seq);

    // Resume the existing on-disk event (keeps pending segments).
    void resume_event();

    // Durably enqueue a segment (atomic write). Computes+stores its checksum.
    // Returns the checksum. Safe to call from the encode thread.
    std::string enqueue(SpooledSegment seg);

    // Lowest-seq pending segment (not yet confirmed), or nullopt if none.
    std::optional<SpooledSegment> peek_next() const;

    // Mark a segment confirmed durable in the bucket → removes its spool files
    // and advances last_confirmed.
    void confirm(uint64_t seq);

    // Number of segments on disk awaiting confirmation.
    size_t pending_count() const;

    // Mark the event ended (clean shutdown) so it is not offered for resume.
    void mark_ended();

    SpoolState state() const;

    // Lowest sequence number this spool still vouches for. A segment whose
    // seq is below this was evicted for disk space and will never exist —
    // the uploader checks this mid-retry so a stale attempt that started
    // before an eviction cannot resurrect a segment after the fact.
    uint64_t floor() const;

    // Total bytes of pending (not yet confirmed) segments currently on disk.
    uint64_t bytes_pending() const;
    // How many segments this spool has ever had to drop for disk space.
    uint64_t dropped_count() const { return m_dropped_count.load(); }

    // Called (off any internal lock) whenever enqueue() has to evict an
    // unconfirmed segment to stay under `max_bytes`. Set once, before the
    // spool is used from more than one thread.
    using DropCallback = std::function<void(const SpoolDrop&)>;
    void set_drop_callback(DropCallback cb) { m_on_drop = std::move(cb); }

private:
    std::string m_dir;
    uint64_t    m_max_bytes = 0;
    mutable std::mutex m_mtx;
    SpoolState  m_state;
    uint64_t    m_bytes_pending = 0;
    std::atomic<uint64_t> m_dropped_count{0};
    DropCallback m_on_drop;

    std::string seg_path(uint64_t seq) const;
    std::string meta_path(uint64_t seq) const;
    std::string state_path() const;
    void load_state();
    void save_state();
    std::vector<uint64_t> pending_seqs() const; // sorted ascending
    // Removes seq's files, advances the floor past it, records the drop.
    // Caller holds m_mtx and appends the result to `out` for the callback to
    // be invoked once the lock is released (the callback may re-enter code
    // that itself locks a caller's mutex — never call it locked).
    uint64_t evict_locked(uint64_t seq, std::vector<SpoolDrop>& out);
};

} // namespace multisite
