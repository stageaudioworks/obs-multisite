// SPDX-License-Identifier: GPL-3.0-or-later
#include "segment_cache.h"
#include "checksum.h"

#include <algorithm>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace multisite {

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

SegmentCache::SegmentCache(std::string dir, std::string event_id)
    : m_dir(std::move(dir)), m_event_id(std::move(event_id)) {
    ensure_dir();
    std::lock_guard<std::mutex> lk(m_mtx);
    build_index_locked();
}

// One directory scan per event; everything after that is answered from memory.
void SegmentCache::build_index_locked() {
    m_index.clear();
    std::error_code ec;
    if (fs::exists(event_dir(), ec)) {
        for (auto& e : fs::directory_iterator(event_dir(), ec)) {
            if (e.path().extension() != ".m4s") continue;
            try { m_index.insert(std::stoull(e.path().stem().string())); }
            catch (...) {}
        }
    }
    m_index_ready = true;
}

std::string SegmentCache::event_dir() const {
    return (fs::path(m_dir) / m_event_id).string();
}
std::string SegmentCache::seg_path(uint64_t seq) const {
    return (fs::path(event_dir()) / (seq_name(seq) + ".m4s")).string();
}
std::string SegmentCache::init_path() const {
    return (fs::path(event_dir()) / "init.mp4").string();
}
void SegmentCache::ensure_dir() const {
    std::error_code ec;
    fs::create_directories(event_dir(), ec);
}

void SegmentCache::set_event(const std::string& event_id) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (event_id == m_event_id) return;
    // A new event means a new init segment and a fresh sequence space; keeping
    // the old files would risk mixing incompatible codec configs.
    m_event_id = event_id;
    sweep_orphans_locked();
    ensure_dir();
    build_index_locked();
}

// A clean event switch has always removed the directory being switched AWAY
// from (immediately below). That leaves exactly one case uncovered: a crash,
// force-kill or power loss mid-event never gets to switch away from anything,
// so that event's whole directory is simply abandoned — invisible to
// prune_below()/the segment-count ceiling, which only ever look inside the
// CURRENT event's own folder. On a box that crashes occasionally (which is
// exactly the Pi appliance, unattended in the field), that is a slow, silent
// disk leak: one orphaned event's worth of segments per crash, forever.
//
// Sweeping every OTHER subdirectory of the cache root, rather than just the
// specific one being left, cleans up however many of those have piled up —
// not only the most recent — the next time this process changes events at
// all, including the very first switch away from the "pending" placeholder a
// fresh DecoderSession always starts with. See BUGS.md.
void SegmentCache::sweep_orphans_locked() {
    std::error_code ec;
    if (!fs::exists(m_dir, ec)) return;
    for (auto& e : fs::directory_iterator(m_dir, ec)) {
        if (ec) break;
        std::error_code dec;
        if (!e.is_directory(dec) || dec) continue;
        if (e.path().filename().string() == m_event_id) continue;
        std::error_code rec;
        fs::remove_all(e.path(), rec);
    }
}

static bool write_atomic(const std::string& path,
                         const std::vector<uint8_t>& bytes) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()),
                (std::streamsize)bytes.size());
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

bool SegmentCache::store_init(const std::vector<uint8_t>& bytes) {
    // An init segment is never empty: refusing one keeps "stored" meaning
    // "decodable", whatever went wrong upstream.
    if (bytes.empty()) return false;
    std::string path;
    { std::lock_guard<std::mutex> lk(m_mtx); ensure_dir(); path = init_path(); }
    return write_atomic(path, bytes);
}

std::optional<std::vector<uint8_t>> SegmentCache::load_init() const {
    std::string path;
    { std::lock_guard<std::mutex> lk(m_mtx); path = init_path(); }
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

bool SegmentCache::has_init() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    // An empty file is not an init segment. Caches written before the fix
    // above can hold one, and treating it as present held playback for ever;
    // treating it as absent fetches the real one again.
    const auto size = fs::file_size(init_path(), ec);
    return !ec && size > 0;
}

bool SegmentCache::store(uint64_t seq, const std::vector<uint8_t>& bytes,
                         const std::string& expected_checksum) {
    // Verify BEFORE caching: a corrupt segment must never become playable.
    if (!expected_checksum.empty() &&
        !verify_sha256(bytes, expected_checksum)) {
        return false;
    }
    // The lock protects the INDEX only, never the file write. A segment is
    // several megabytes and a write can take a while on Windows (antivirus
    // scanning included); holding the lock across it blocked every other
    // query, including the ones the UI makes when the operator clicks the
    // timeline.
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ensure_dir();
        path = seg_path(seq);
    }
    if (!write_atomic(path, bytes)) return false;
    std::lock_guard<std::mutex> lk(m_mtx);
    m_index.insert(seq);
    return true;
}

std::optional<std::vector<uint8_t>> SegmentCache::load(uint64_t seq) const {
    // Path under the lock; the multi-megabyte read outside it.
    std::string path;
    { std::lock_guard<std::mutex> lk(m_mtx); path = seg_path(seq); }
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

bool SegmentCache::has(uint64_t seq) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_index.count(seq) > 0;      // memory, not a filesystem call
}

std::set<uint64_t> SegmentCache::cached_seqs() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_index;
}

size_t SegmentCache::count() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_index.size();
}
uint64_t SegmentCache::lowest_seq() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_index.empty() ? 0 : *m_index.begin();
}
uint64_t SegmentCache::highest_seq() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_index.empty() ? 0 : *m_index.rbegin();
}

size_t SegmentCache::prune_below(uint64_t keep_from) {
    // Take the list and update the index under the lock; delete the files
    // afterwards, so a slow filesystem cannot stall other queries.
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_index.begin(); it != m_index.end(); ) {
            if (*it >= keep_from) break;
            paths.push_back(seg_path(*it));
            it = m_index.erase(it);
        }
    }
    size_t removed = 0;
    std::error_code ec;
    for (const auto& p : paths) if (fs::remove(p, ec)) ++removed;
    return removed;
}

void SegmentCache::clear() {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    fs::remove_all(event_dir(), ec);
    ensure_dir();
    m_index.clear();
}

} // namespace multisite
