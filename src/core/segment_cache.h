// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// segment_cache.h — the satellite's local DVR store.
//
// Downloaded segments are written to disk and verified against the checksum
// from the manifest. The cache is what makes timeslipping possible: playback
// reads from here, not from the network, so a campus can pause, sit behind
// live, or scrub backwards while downloads continue independently.
//
// Layout (under `dir`):
//   init.mp4                  the event's init segment
//   <seq:08d>.m4s             cached media fragments
//
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace multisite {

class SegmentCache {
public:
    SegmentCache(std::string dir, std::string event_id);

    // Switch to a different event (clears everything for the old one).
    void set_event(const std::string& event_id);
    const std::string& event_id() const { return m_event_id; }

    // Init segment (the codec configuration for the event).
    bool store_init(const std::vector<uint8_t>& bytes);
    std::optional<std::vector<uint8_t>> load_init() const;
    bool has_init() const;

    // Media fragments. store() verifies against `expected_checksum` when one is
    // supplied and refuses to cache a mismatch, so corruption can't reach the
    // decoder — the caller re-fetches instead.
    bool store(uint64_t seq, const std::vector<uint8_t>& bytes,
               const std::string& expected_checksum = "");
    std::optional<std::vector<uint8_t>> load(uint64_t seq) const;
    bool has(uint64_t seq) const;

    // What's cached right now.
    size_t   count() const;
    uint64_t lowest_seq() const;
    uint64_t highest_seq() const;
    std::set<uint64_t> cached_seqs() const;

    // Drop everything below `keep_from` (bounds disk use while preserving the
    // ability to seek back within the retained window).
    size_t prune_below(uint64_t keep_from);

    void clear();

private:
    std::string m_dir;
    std::string m_event_id;
    // In-memory index of what is cached. Queries (count, ranges, has) are
    // called from the UI several times a second; scanning the directory for
    // each one meant thousands of filesystem calls on the UI thread and made
    // the interface sluggish. The index is built once per event and maintained
    // on store/prune.
    std::set<uint64_t> m_index;
    bool m_index_ready = false;
    mutable std::mutex m_mtx;

    void build_index_locked();
    // Removes every subdirectory of the cache root except the current
    // event's own — see set_event()'s comment for why this exists.
    void sweep_orphans_locked();

    std::string event_dir() const;
    std::string seg_path(uint64_t seq) const;
    std::string init_path() const;
    void ensure_dir() const;
};

} // namespace multisite
