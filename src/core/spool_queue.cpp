// SPDX-License-Identifier: GPL-3.0-or-later
#include "spool_queue.h"
#include "checksum.h"
#include "session.h"   // for now_ms() — see decoder_session.cpp for the same use
#include "../vendor/nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace multisite {

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

SpoolQueue::SpoolQueue(std::string dir, uint64_t max_bytes)
    : m_dir(std::move(dir)), m_max_bytes(max_bytes) {
    fs::create_directories(m_dir);
    load_state();
    // Recompute pending bytes from what's actually on disk (not persisted:
    // trivial to get wrong across a crash, trivial to recompute here).
    for (uint64_t seq : pending_seqs()) {
        std::error_code ec;
        auto sz = fs::file_size(seg_path(seq), ec);
        if (!ec) m_bytes_pending += sz;
    }
}

std::string SpoolQueue::seg_path(uint64_t seq) const {
    return (fs::path(m_dir) / (seq_name(seq) + ".seg")).string();
}
std::string SpoolQueue::meta_path(uint64_t seq) const {
    return (fs::path(m_dir) / (seq_name(seq) + ".meta")).string();
}
std::string SpoolQueue::state_path() const {
    return (fs::path(m_dir) / "state.json").string();
}

// Atomic write: write to <path>.tmp then rename over <path>.
static void atomic_write(const std::string& path, const void* data, size_t n) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("spool: cannot open " + tmp);
        f.write(reinterpret_cast<const char*>(data), (std::streamsize)n);
        f.flush();
    }
    fs::rename(tmp, path); // atomic on POSIX and Windows same-volume
}

void SpoolQueue::load_state() {
    m_state = SpoolState{};
    std::ifstream f(state_path());
    if (!f) return;
    try {
        json j; f >> j;
        m_state.event_id       = j.value("event_id", "");
        m_state.first_seq      = j.value("first_seq", (uint64_t)0);
        m_state.last_enqueued  = j.value("last_enqueued", (uint64_t)0);
        m_state.last_confirmed = j.value("last_confirmed", (uint64_t)0);
        m_state.targets        = j.value("targets", 1);
        m_state.last_confirmed_2 = j.value("last_confirmed_2", (uint64_t)0);
        m_state.any_confirmed  = j.value("any_confirmed", false);
        m_state.any_confirmed_2 = j.value("any_confirmed_2", false);
        m_state.ended          = j.value("ended", false);
        m_state.last_activity_ms = j.value("last_activity_ms", (int64_t)0);
        m_state.valid          = true;
    } catch (...) {
        m_state = SpoolState{}; // corrupt state → treat as none
    }
}

void SpoolQueue::save_state() {
    json j;
    j["event_id"]       = m_state.event_id;
    j["first_seq"]      = m_state.first_seq;
    j["last_enqueued"]  = m_state.last_enqueued;
    j["last_confirmed"] = m_state.last_confirmed;
    j["targets"]        = m_state.targets;
    j["last_confirmed_2"] = m_state.last_confirmed_2;
    j["any_confirmed"]  = m_state.any_confirmed;
    j["any_confirmed_2"] = m_state.any_confirmed_2;
    j["ended"]          = m_state.ended;
    j["last_activity_ms"] = m_state.last_activity_ms;
    std::string s = j.dump();
    atomic_write(state_path(), s.data(), s.size());
}

std::vector<uint64_t> SpoolQueue::pending_seqs() const {
    std::vector<uint64_t> out;
    if (!fs::exists(m_dir)) return out;
    for (auto& e : fs::directory_iterator(m_dir)) {
        auto p = e.path();
        if (p.extension() == ".seg") {
            try { out.push_back(std::stoull(p.stem().string())); }
            catch (...) {}
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

ResumeInfo SpoolQueue::inspect() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    ResumeInfo r;
    if (!m_state.valid || m_state.ended) return r;
    auto pend = pending_seqs();
    // Resumable if there is a live (un-ended) event, whether or not segments
    // are still pending (operator may want to continue the sequence).
    r.resumable      = true;
    r.event_id       = m_state.event_id;
    r.last_confirmed = m_state.last_confirmed;
    r.last_enqueued  = m_state.last_enqueued;
    r.pending_count  = pend.size();
    r.last_activity_ms = m_state.last_activity_ms;
    return r;
}

void SpoolQueue::begin_event(const std::string& event_id, uint64_t first_seq) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // clear old spool
    for (auto& e : fs::directory_iterator(m_dir)) {
        auto ext = e.path().extension();
        if (ext == ".seg" || ext == ".meta") fs::remove(e.path());
    }
    // How many targets there are is CONFIGURATION, not event state — a new
    // event does not un-configure the second bucket. Everything else here is
    // per-event and is reset.
    const int targets = m_state.targets;
    m_state = SpoolState{};
    m_state.targets       = targets;
    m_state.event_id      = event_id;
    m_state.first_seq     = first_seq;
    m_state.last_enqueued = first_seq > 0 ? first_seq - 1 : 0;
    m_state.last_confirmed= first_seq > 0 ? first_seq - 1 : 0;
    m_state.last_confirmed_2 = m_state.last_confirmed;
    m_state.ended         = false;
    m_state.valid         = true;
    m_state.last_activity_ms = now_ms();
    m_bytes_pending       = 0;
    save_state();
}

void SpoolQueue::resume_event() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_state.valid) throw std::runtime_error("spool: nothing to resume");
    m_state.ended = false;
    m_state.last_activity_ms = now_ms();
    save_state();
}

std::string SpoolQueue::enqueue(SpooledSegment seg) {
    std::vector<SpoolDrop> drops;
    std::string checksum;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        seg.checksum = sha256_hex(seg.data);
        checksum = seg.checksum;

        // 1) segment bytes (atomic)
        atomic_write(seg_path(seg.seq), seg.data.data(), seg.data.size());
        // 2) sidecar meta (atomic) — written AFTER the bytes so a crash between
        //    the two leaves an orphan .seg with no .meta, which drain skips
        //    safely.
        json m;
        m["seq"]          = seg.seq;
        m["duration_s"]   = seg.duration_s;
        m["pts_offset_s"] = seg.pts_offset_s;
        m["checksum"]     = seg.checksum;
        m["key"]          = seg.key;
        std::string ms = m.dump();
        atomic_write(meta_path(seg.seq), ms.data(), ms.size());

        if (seg.seq > m_state.last_enqueued) m_state.last_enqueued = seg.seq;
        m_state.last_activity_ms = now_ms();
        m_bytes_pending += seg.data.size();

        // Over the disk cap: drop the OLDEST unconfirmed segments (never the
        // one just written) until back under it, or only one is left.
        if (m_max_bytes > 0) {
            auto pend = pending_seqs();
            size_t i = 0;
            while (m_bytes_pending > m_max_bytes && pend.size() - i > 1) {
                uint64_t victim = pend[i];
                if (victim == seg.seq) break; // never evict what we just wrote
                m_bytes_pending -= evict_locked(victim, drops);
                ++i;
            }
        }
        save_state();
    }
    // Fire the callback with no lock held: it may call back into code that
    // takes a caller-owned mutex (e.g. to update a manifest), and this lock is
    // already held by callers elsewhere (pending_count/state) in a different
    // order — invoking it here would risk the exact AB/BA deadlock this
    // codebase has been bitten by before.
    if (m_on_drop) for (const auto& d : drops) m_on_drop(d);
    return checksum;
}

std::optional<SpooledSegment> SpoolQueue::peek_next(int target) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const bool     any  = target == 1 ? m_state.any_confirmed_2
                                      : m_state.any_confirmed;
    const uint64_t mark = target == 1 ? m_state.last_confirmed_2
                                      : m_state.last_confirmed;
    for (uint64_t seq : pending_seqs()) {
        if (any && seq <= mark) continue;      // this target already has it
        std::string mp = meta_path(seq), sp = seg_path(seq);
        if (!fs::exists(mp)) continue; // orphaned .seg (crash between writes)
        std::ifstream mf(mp);
        json m; try { mf >> m; } catch (...) { continue; }

        std::ifstream sf(sp, std::ios::binary);
        if (!sf) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(sf)), {});

        SpooledSegment s;
        s.seq          = seq;
        s.data         = std::move(data);
        s.duration_s   = m.value("duration_s", 6.0);
        s.pts_offset_s = m.value("pts_offset_s", 0.0);
        s.checksum     = m.value("checksum", "");
        s.key          = m.value("key", "");
        return s;
    }
    return std::nullopt;
}

uint64_t SpoolQueue::evict_locked(uint64_t seq, std::vector<SpoolDrop>& out) {
    std::error_code ec;
    uint64_t freed = 0;
    auto sz = fs::file_size(seg_path(seq), ec);
    if (!ec) freed = sz;
    fs::remove(seg_path(seq), ec);
    fs::remove(meta_path(seq), ec);
    if (seq + 1 > m_state.first_seq) m_state.first_seq = seq + 1;
    m_dropped_count++;
    out.push_back(SpoolDrop{ seq, m_state.first_seq });
    return freed;
}

uint64_t SpoolQueue::bytes_pending() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_bytes_pending;
}

uint64_t SpoolQueue::floor() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state.first_seq;
}

void SpoolQueue::confirm(uint64_t seq) {
    confirm(seq, 0);
}

uint64_t SpoolQueue::last_confirmed_for(int target) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return target == 1 ? m_state.last_confirmed_2 : m_state.last_confirmed;
}

bool SpoolQueue::caught_up(int target) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const bool     any  = target == 1 ? m_state.any_confirmed_2
                                      : m_state.any_confirmed;
    const uint64_t mark = target == 1 ? m_state.last_confirmed_2
                                      : m_state.last_confirmed;
    // "Nothing is waiting for this target" — the same predicate peek_next uses,
    // so the yield rule and the queue can never disagree about whether there is
    // work. Deliberately not derived from last_enqueued: that starts at
    // first_seq-1 too, and carries the same ambiguity about sequence 0.
    for (uint64_t seq : pending_seqs())
        if (!(any && seq <= mark)) return false;
    return true;
}

int SpoolQueue::targets() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state.targets;
}

void SpoolQueue::set_targets(int n) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (n < 1) n = 1;
    if (n > 2) n = 2;
    m_state.targets = n;
    // Going back to one target: anything the remaining target already has is no
    // longer waiting on anybody, so remove it now rather than leaving orphans
    // on disk until the next event clears them.
    if (n < 2) {
        std::error_code ec;
        for (uint64_t seq : pending_seqs()) {
            if (!m_state.any_confirmed || seq > m_state.last_confirmed) continue;
            auto sz = fs::file_size(seg_path(seq), ec);
            if (!ec && sz <= m_bytes_pending) m_bytes_pending -= sz;
            fs::remove(seg_path(seq), ec);
            fs::remove(meta_path(seq), ec);
        }
    }
    m_state.last_activity_ms = now_ms();
    save_state();
}

void SpoolQueue::confirm(uint64_t seq, int target) {
    std::lock_guard<std::mutex> lk(m_mtx);

    uint64_t& mark = (target == 1) ? m_state.last_confirmed_2
                                   : m_state.last_confirmed;
    if (target == 1) m_state.any_confirmed_2 = true;
    else             m_state.any_confirmed   = true;
    if (seq > mark) mark = seq;
    m_state.last_activity_ms = now_ms();
    save_state();

    // Removed only once every target in play holds it. With one target this is
    // the same test the spool has always made.
    if (m_state.targets > 1) {
        if (seq > m_state.last_confirmed_2 || seq > m_state.last_confirmed)
            return;
    } else if (seq > m_state.last_confirmed) {
        return;
    }

    std::error_code ec;
    auto sz = fs::file_size(seg_path(seq), ec);
    if (!ec && sz <= m_bytes_pending) m_bytes_pending -= sz;
    fs::remove(seg_path(seq), ec);
    fs::remove(meta_path(seq), ec);
}

size_t SpoolQueue::pending_count() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return pending_seqs().size();
}

void SpoolQueue::mark_ended() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_state.ended = true;
    save_state();
}

SpoolState SpoolQueue::state() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state;
}

} // namespace multisite
