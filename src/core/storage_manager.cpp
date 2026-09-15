// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_manager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <utility>

namespace multisite {

namespace {

// A page is not a page: a store may cap results well below max_keys, and a
// prefix that never finishes paging would otherwise hold a worker for ever —
// there is a test for "truncated with no token", but a store that repeated a
// token would still spin. 400 pages is 400,000 objects under one event: far
// past any real event, and the point where continuing is a fault, not work.
constexpr int kMaxPagesPerPrefix = 400;

void count_request(ListStats* stats) {
    if (stats) ++stats->requests;
}

bool cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load();
}

int64_t ms_since(const std::chrono::steady_clock::time_point& t0) {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

} // namespace

// Crockford base32, the ULID alphabet: no I, L, O or U, so the encoding cannot
// produce a string that reads as a different one when written down by hand.
int64_t event_id_time_ms(const std::string& event_id) {
    static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    if (event_id.size() < 10) return 0;

    int64_t ms = 0;
    for (int i = 0; i < 10; ++i) {
        const unsigned char c = (unsigned char)event_id[i];
        if (c == 0) return 0;
        const char upper = (char)std::toupper(c);
        const char* at = std::strchr(kAlphabet, upper);
        if (!at) return 0;                    // not a ULID at all
        ms = ms * 32 + (int64_t)(at - kAlphabet);
    }

    // Only a plausible date is worth anything here. This value decides what
    // gets permanently deleted, and a mis-decode landing near zero would date
    // the event to 1970 and make it the oldest thing in the bucket.
    constexpr int64_t kY2020 = 1577836800000LL;   // 2020-01-01
    const int64_t tomorrow =
        (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() + 24LL * 60 * 60 * 1000;
    if (ms < kY2020 || ms > tomorrow) return 0;
    return ms;
}

StorageManager::StorageManager(std::string room_id, Transport& transport)
    : m_room_id(std::move(room_id)), m_tx(transport),
      m_catalog(CatalogConfig{m_room_id, 600000}, transport) {}

// ── The fast half: which events exist ────────────────────────────────────────
// One listing of the room index, one read of each event's manifest, and one
// read of the live pointer. Nothing here walks an event's objects, which is the
// expensive part and the reason this is separable at all: an operator should see
// the events — names, dates, which one is on air — while the sizes are still
// being counted.
bool StorageManager::list_events(std::vector<ManagedEvent>& out, std::string& error,
                                 ListStats* stats,
                                 const std::atomic<bool>* cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    if (stats) *stats = ListStats{};

    if (!m_catalog.refresh()) {
        error = m_catalog.last_error();
        if (stats) stats->elapsed_ms = ms_since(t0);
        return false;
    }
    if (cancelled(cancel)) {
        error = "cancelled";
        if (stats) { stats->cancelled = true; stats->elapsed_ms = ms_since(t0); }
        return false;
    }

    count_request(stats);                       // the live pointer
    const std::string live = live_event_id();

    out.clear();
    out.reserve(m_catalog.events().size());
    for (const auto& s : m_catalog.events()) {
        ManagedEvent e;
        e.event_id      = s.event_id;
        e.name          = s.name;
        e.started_at_ms = s.started_at_ms;
        e.state         = s.state;
        e.is_live       = (!live.empty() && live == s.event_id);
        // Sizes are deliberately left unmeasured: tally_size() fills them in.
        out.push_back(std::move(e));
    }

    // Live first, then newest first.
    std::sort(out.begin(), out.end(), [](const ManagedEvent& a, const ManagedEvent& b) {
        if (a.is_live != b.is_live) return a.is_live;
        if (a.started_at_ms != b.started_at_ms)
            return a.started_at_ms > b.started_at_ms;
        return a.event_id > b.event_id;
    });

    if (stats) {
        stats->events = (int)out.size();
        stats->elapsed_ms = ms_since(t0);
    }
    return true;
}

// ── The expensive half: how much one event holds ────────────────────────────
bool StorageManager::tally_size(ManagedEvent& e, std::string& error,
                                ListStats* stats,
                                const std::atomic<bool>* cancel) {
    std::vector<std::string> keys;
    uint64_t bytes = 0;
    if (!list_prefix(event_prefix_for(e.event_id), keys, bytes, error, stats,
                     cancel)) {
        e.size_known = false;
        e.size_error = error;
        if (stats && error != "cancelled") ++stats->tallies_failed;
        return false;
    }
    e.objects    = keys.size();
    e.bytes      = bytes;
    e.size_known = true;
    e.size_error.clear();
    return true;
}

// Both, in sequence: the cleanup path wants the whole answer at once, where the
// window wants it progressively and calls the two halves itself.
bool StorageManager::list(std::vector<ManagedEvent>& out, std::string& error,
                          ListStats* stats, const std::atomic<bool>* cancel) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!list_events(out, error, stats, cancel)) return false;

    for (auto& e : out) {
        if (cancelled(cancel)) {
            error = "cancelled";
            if (stats) { stats->cancelled = true; stats->elapsed_ms = ms_since(t0); }
            return false;
        }
        std::string err;
        tally_size(e, err, stats, cancel);
        // A failed tally is reported on the event itself, through size_known and
        // size_error; the event still lists.
    }

    if (stats) stats->elapsed_ms = ms_since(t0);
    return true;
}

bool StorageManager::list_prefix(const std::string& prefix,
                                 std::vector<std::string>& keys,
                                 uint64_t& bytes, std::string& error,
                                 ListStats* stats,
                                 const std::atomic<bool>* cancel) {
    keys.clear();
    bytes = 0;
    std::string token;
    int pages = 0;
    do {
        if (cancelled(cancel)) { error = "cancelled"; return false; }
        if (++pages > kMaxPagesPerPrefix) {
            error = "listing " + prefix + " did not finish after " +
                    std::to_string(kMaxPagesPerPrefix) + " pages";
            return false;
        }
        count_request(stats);
        ListResult r = m_tx.list(prefix, "", token, 1000);
        if (!r.success) {
            error = r.error;
            return false;
        }
        for (const auto& e : r.keys) {
            keys.push_back(e.key);
            if (e.size > 0) bytes += (uint64_t)e.size;
        }
        token = r.truncated ? r.next_continuation_token : std::string();
    } while (!token.empty());
    return true;
}

std::string StorageManager::live_event_id() {
    GetResult g = m_tx.get(live_pointer_key(m_room_id));
    if (!g.success) return "";
    try {
        return LivePointer::from_json(
                   std::string(g.body.begin(), g.body.end())).event_id;
    } catch (...) {
        return "";
    }
}

bool StorageManager::remove_key(const std::string& key, std::string& error) {
    DeleteResult r = m_tx.remove(key);
    if (r.success) return true;
    error = "DELETE " + key + ": " +
            (r.error.empty() ? ("HTTP " + std::to_string(r.http_status)) : r.error);
    return false;
}

DeleteReport StorageManager::delete_event(const std::string& event_id,
                                          const DeleteProgressFn& progress) {
    DeleteReport rep;
    rep.events_considered = 1;

    const std::string live = live_event_id();
    if (!live.empty() && live == event_id) {
        rep.error = "That event is on air now and cannot be deleted.";
        rep.events_live_kept = 1;
        return rep;
    }

    std::vector<std::string> keys;
    uint64_t bytes = 0;
    std::string err;
    if (!list_prefix(event_prefix_for(event_id), keys, bytes, err)) {
        rep.error = err.empty() ? "could not list the event" : err;
        return rep;
    }

    // The room-index entry lives outside events/{id}/; remove it too so the
    // event list stops advertising an event whose media is gone.
    keys.push_back(room_event_key(m_room_id, event_id));

    const uint64_t total = keys.size();
    DeleteProgress pr;
    pr.event_id      = event_id;
    pr.objects_total = total;
    for (uint64_t i = 0; i < keys.size(); ++i) {
        pr.objects_done = i;
        if (progress && !progress(pr)) {
            rep.objects_deleted = i;
            rep.bytes_freed = bytes;
            rep.cancelled = true;
            rep.error = "cancelled";
            return rep;
        }
        std::string e;
        if (!remove_key(keys[i], e)) {
            rep.objects_deleted = i;
            rep.bytes_freed = bytes;
            rep.error = e;
            return rep;
        }
    }

    // Verification: the event prefix must now be empty.
    std::vector<std::string> remaining;
    uint64_t rem_bytes = 0;
    std::string verr;
    bool confirmed = false;
    if (list_prefix(event_prefix_for(event_id), remaining, rem_bytes, verr))
        confirmed = remaining.empty();

    rep.ok = true;
    rep.objects_deleted = total;
    rep.bytes_freed = bytes;
    rep.confirmed = confirmed;
    rep.events_matched = 1;
    rep.events_deleted = 1;
    rep.deleted_ids.push_back(event_id);
    return rep;
}

DeleteReport StorageManager::delete_older_than(int older_than_days, int64_t now_ms,
                                               const DeleteProgressFn& progress) {
    DeleteReport rep;

    // No sizes needed here: the cutoff is the event's start time, and the bytes
    // freed come back from each delete. Using the full listing would spend
    // minutes counting objects to decide something that counting cannot change.
    std::vector<ManagedEvent> events;
    std::string err;
    if (!list_events(events, err)) {
        rep.error = err;
        return rep;
    }
    rep.events_considered = events.size();

    const int64_t cutoff = now_ms - (int64_t)older_than_days * 24LL * 60 * 60 * 1000;

    struct Target { std::string id, name; };
    std::vector<Target> targets;
    for (const auto& e : events) {
        if (e.is_live) { ++rep.events_live_kept; continue; }
        // The manifest's start time, or failing that the one in the event's own
        // id. An event whose manifest never recorded a start is precisely the
        // debris this button exists to clear, and insisting on the field made
        // those events undeletable from here for ever.
        int64_t started = e.started_at_ms;
        if (started <= 0) started = event_id_time_ms(e.event_id);
        if (started <= 0) { ++rep.events_undated; continue; }
        if (started < cutoff) targets.push_back({ e.event_id, e.name });
    }
    rep.events_matched = targets.size();

    for (uint64_t i = 0; i < targets.size(); ++i) {
        const Target& t = targets[i];

        // Re-frame this event's own object progress as progress through the
        // whole run, so a caller showing a bar does not see it restart from
        // zero at every event.
        DeleteProgressFn per_event;
        if (progress) {
            const uint64_t index = i;
            const uint64_t count = targets.size();
            const std::string name = t.name;
            per_event = [&progress, index, count, name](const DeleteProgress& p) {
                DeleteProgress q = p;
                q.event_index = index;
                q.event_count = count;
                q.event_name  = name;
                return progress(q);
            };
        }

        DeleteReport r = delete_event(t.id, per_event);
        rep.objects_deleted += r.objects_deleted;
        rep.bytes_freed     += r.bytes_freed;

        if (!r.ok) {
            rep.cancelled = r.cancelled;
            // Cancelling is the operator's own decision, not a failure: report
            // what was removed before they stopped it, and say it was stopped.
            if (r.cancelled) {
                rep.ok = true;
                return rep;
            }
            rep.error = r.error;
            return rep;
        }

        ++rep.events_deleted;
        rep.deleted_ids.push_back(t.id);
        // Every event deleted so far has re-listed empty. Starting this at
        // false and ANDing meant a bulk run could never report confirmation at
        // all, so the window said "removed, but re-checking failed" after a
        // run in which every check had in fact passed.
        rep.confirmed = (i == 0) ? r.confirmed : (rep.confirmed && r.confirmed);
    }

    // Nothing matched is a legitimate outcome, and a confirmed one: there was
    // nothing to verify.
    if (targets.empty()) rep.confirmed = true;

    rep.ok = true;
    return rep;
}

} // namespace multisite
