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
#if defined(_WIN32)
#include <io.h>        // _commit, _fileno
#else
#include <fcntl.h>     // open
#include <unistd.h>    // fsync, close
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace multisite {

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

std::string SpoolQueue::seg_path_in(const std::string& dir, uint64_t seq) {
    return (fs::path(dir) / (seq_name(seq) + ".seg")).string();
}
std::string SpoolQueue::meta_path_in(const std::string& dir, uint64_t seq) {
    return (fs::path(dir) / (seq_name(seq) + ".meta")).string();
}

namespace {

// state.json of one event folder, or an invalid state when it has none or it
// does not parse.
SpoolState read_state_file(const fs::path& file) {
    SpoolState st;
    std::ifstream f(file);
    if (!f) return st;
    try {
        json j; f >> j;
        st.event_id       = j.value("event_id", "");
        st.first_seq      = j.value("first_seq", (uint64_t)0);
        st.last_enqueued  = j.value("last_enqueued", (uint64_t)0);
        st.last_confirmed = j.value("last_confirmed", (uint64_t)0);
        st.targets        = j.value("targets", 1);
        st.last_confirmed_2 = j.value("last_confirmed_2", (uint64_t)0);
        st.any_confirmed  = j.value("any_confirmed", false);
        st.any_confirmed_2 = j.value("any_confirmed_2", false);
        st.ended          = j.value("ended", false);
        st.last_activity_ms = j.value("last_activity_ms", (int64_t)0);
        st.media_end_s    = j.value("media_end_s", 0.0);
        st.end_unpublished = j.value("end_unpublished", false);
        st.valid          = true;
    } catch (...) {
        st = SpoolState{};   // corrupt state → treat as none
    }
    return st;
}

bool has_segments(const fs::path& dir) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".seg") return true;
    return false;
}

// The single-folder layout this replaced: state.json and the segments sat in
// the root. Moved into the event's own folder, once, so an upgrade carries on
// with whatever was pending rather than stranding it.
void migrate_single_folder(const fs::path& root) {
    const fs::path old_state = root / "state.json";
    if (!fs::exists(old_state)) return;
    const SpoolState st = read_state_file(old_state);
    const fs::path dest = root / (st.valid && !st.event_id.empty() ? st.event_id
                                                                   : std::string("unknown-event"));
    std::error_code ec;
    fs::create_directories(dest, ec);
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension();
        if (ext == ".tmp") { fs::remove(e.path(), ec); continue; }
        if (ext == ".seg" || ext == ".meta" || e.path().filename() == "state.json")
            fs::rename(e.path(), dest / e.path().filename(), ec);
    }
}

} // namespace

SpoolQueue::SpoolQueue(std::string root, uint64_t max_bytes,
                       const std::string& event_id)
    : m_root(std::move(root)), m_max_bytes(max_bytes) {
    fs::create_directories(m_root);
    migrate_single_folder(m_root);

    if (!event_id.empty()) {
        m_dir = (fs::path(m_root) / event_id).string();
    } else {
        // The one event still unfinished is the current (resumable) one. If
        // more than one is — not reachable through begin_event, which ends the
        // previous, but a crash or an old layout could leave it — only the most
        // recently active can be resumed, so the rest are ended and left for
        // EventFinisher, never stranded.
        std::string best;
        int64_t best_ms = -1;
        std::vector<std::pair<fs::path, SpoolState>> unfinished;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(m_root, ec)) {
            if (!e.is_directory()) continue;
            SpoolState st = read_state_file(e.path() / "state.json");
            if (!st.valid || st.ended) continue;
            unfinished.emplace_back(e.path(), st);
            if (st.last_activity_ms > best_ms) { best_ms = st.last_activity_ms; best = e.path().string(); }
        }
        for (auto& [dir, st] : unfinished) {
            if (dir.string() == best) continue;
            m_dir = dir.string();
            m_state = st;
            m_state.ended = true;
            m_state.end_unpublished = true;   // it never had an End: owed
            write_state_doc(m_dir, state_json(m_state));
        }
        m_dir = best;
    }
    load_state();
    // Recompute pending bytes from what's actually on disk (not persisted:
    // trivial to get wrong across a crash, trivial to recompute here).
    for (uint64_t seq : pending_seqs_in(m_dir)) {
        std::error_code ec;
        auto sz = fs::file_size(seg_path_in(m_dir, seq), ec);
        if (!ec) m_bytes_pending += sz;
    }
}

std::vector<std::string> SpoolQueue::finishing_events(const std::string& root) {
    std::vector<std::pair<int64_t, std::string>> found;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        const SpoolState st = read_state_file(e.path() / "state.json");
        if (st.valid && st.ended && (has_segments(e.path()) || st.end_unpublished))
            found.emplace_back(st.last_activity_ms, e.path().filename().string());
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> ids;
    for (auto& f : found) ids.push_back(f.second);
    return ids;
}

size_t SpoolQueue::remove_finished(const std::string& root) {
    std::vector<fs::path> done;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        const SpoolState st = read_state_file(e.path() / "state.json");
        if (st.valid && st.ended && !st.end_unpublished && !has_segments(e.path()))
            done.push_back(e.path());
    }
    for (const auto& d : done) fs::remove_all(d, ec);
    return done.size();
}

void SpoolQueue::discard_event(const std::string& root, const std::string& event_id) {
    if (event_id.empty()) return;
    std::error_code ec;
    fs::remove_all(fs::path(root) / event_id, ec);
}

bool SpoolQueue::remove_if_done() {
    std::lock_guard<std::mutex> iol(m_io_mtx);
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_dir.empty() || !m_state.ended || m_state.end_unpublished) return false;
        dir = m_dir;
    }
    // The folder scan and the removal are disk work: done with m_mtx released,
    // so a status read is never behind them. m_io_mtx keeps begin_event() from
    // adopting this folder while it is being removed.
    if (has_segments(dir)) return false;
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_dir != dir) return true;   // cannot happen while m_io_mtx is held
    m_dir.clear();
    m_bytes_pending = 0;
    return true;
}

std::string SpoolQueue::state_path() const {
    return (fs::path(m_dir) / "state.json").string();
}

// Atomic AND durable: write <path>.tmp, flush it to the disk, rename it over
// <path>, then flush the directory so the rename itself survives.
//
// The flushes are the "durable" half. This used to stop at ofstream::flush(),
// which only reaches the OS cache: an OBS crash was survived, but a power cut
// could bring a renamed segment back empty or truncated — and a truncated
// segment was uploaded as it was, under the checksum of the bytes it should
// have held, failing at every campus. PROJECT-SCOPE §5 promises power loss.
// One sync per segment (every few seconds) is nothing next to the upload.
// ponytail: fsync, not F_FULLFSYNC — a Mac's own disk cache is trusted, as
// every other app trusts it; switch if a Mac ever loses a synced segment.
static void atomic_write(const std::string& path, const void* data, size_t n) {
    std::string tmp = path + ".tmp";
    {
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) throw std::runtime_error("spool: cannot open " + tmp);
        const bool ok = std::fwrite(data, 1, n, f) == n && std::fflush(f) == 0 &&
#if defined(_WIN32)
                        _commit(_fileno(f)) == 0;
#else
                        ::fsync(::fileno(f)) == 0;
#endif
        std::fclose(f);
        if (!ok) throw std::runtime_error("spool: cannot write " + tmp);
    }
    fs::rename(tmp, path); // atomic on POSIX and Windows same-volume
#if !defined(_WIN32)
    // NTFS journals the rename; a POSIX filesystem needs the directory synced.
    const int dfd = ::open(fs::path(path).parent_path().string().c_str(), O_RDONLY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
#endif
}

void SpoolQueue::load_state() {
    m_state = m_dir.empty() ? SpoolState{} : read_state_file(state_path());
}

std::string SpoolQueue::state_json(const SpoolState& st) {
    json j;
    j["event_id"]       = st.event_id;
    j["first_seq"]      = st.first_seq;
    j["last_enqueued"]  = st.last_enqueued;
    j["last_confirmed"] = st.last_confirmed;
    j["targets"]        = st.targets;
    j["last_confirmed_2"] = st.last_confirmed_2;
    j["any_confirmed"]  = st.any_confirmed;
    j["any_confirmed_2"] = st.any_confirmed_2;
    j["ended"]          = st.ended;
    j["last_activity_ms"] = st.last_activity_ms;
    j["media_end_s"]    = st.media_end_s;
    j["end_unpublished"] = st.end_unpublished;
    return j.dump();
}

void SpoolQueue::write_state_doc(const std::string& dir, const std::string& doc) {
    if (dir.empty()) return;   // no current event: nothing of its own to record
    atomic_write((fs::path(dir) / "state.json").string(), doc.data(), doc.size());
}

std::vector<uint64_t> SpoolQueue::pending_seqs_in(const std::string& dir) {
    std::vector<uint64_t> out;
    if (dir.empty() || !fs::exists(dir)) return out;
    for (auto& e : fs::directory_iterator(dir)) {
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
    ResumeInfo r;
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_state.valid || m_state.ended) return r;
        // Resumable if there is a live (un-ended) event, whether or not segments
        // are still pending (operator may want to continue the sequence).
        r.resumable      = true;
        r.event_id       = m_state.event_id;
        r.last_confirmed = m_state.last_confirmed;
        r.last_enqueued  = m_state.last_enqueued;
        r.last_activity_ms = m_state.last_activity_ms;
        r.media_end_s    = m_state.media_end_s;
        dir = m_dir;
    }
    // Counted outside the lock: it is a folder scan, and this runs at start-up
    // alongside nothing that needs the number to be atomic with the state above.
    r.pending_count = pending_seqs_in(dir).size();
    return r;
}

void SpoolQueue::begin_event(const std::string& event_id, uint64_t first_seq) {
    std::lock_guard<std::mutex> iol(m_io_mtx);
    // The previous event is NOT cleared. It used to be — every .seg and .meta
    // deleted — which threw away whatever it had not yet uploaded. Now it is
    // ended and left in its folder for EventFinisher, or removed if there is
    // nothing left in it to send.
    std::string prev_dir, prev_doc;
    bool prev_owed = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_dir.empty() && m_state.valid) {
            // An event left unfinished (it was never Ended) is over now; storage
            // has never been told, so that is owed too, with whatever it had left.
            if (!m_state.ended) m_state.end_unpublished = true;
            m_state.ended = true;
            prev_dir = m_dir;
            prev_owed = m_state.end_unpublished;
            prev_doc = state_json(m_state);
        }
    }
    if (!prev_dir.empty()) {
        write_state_doc(prev_dir, prev_doc);
        // Disk work with m_mtx released; m_io_mtx keeps this folder from
        // being written to while it is tidied away.
        if (!prev_owed && !has_segments(prev_dir)) {
            std::error_code ec; fs::remove_all(prev_dir, ec);
        }
    }
    const std::string dir = (fs::path(m_root) / event_id).string();
    {
        std::error_code ec;
        fs::remove_all(dir, ec);   // a fresh id; nothing of its own to keep
        fs::create_directories(dir);
    }
    std::string doc;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // How many targets there are is CONFIGURATION, not event state — a new
        // event does not un-configure the second bucket. Everything else here is
        // per-event and is reset.
        const int targets = m_state.targets;
        m_dir = dir;
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
        doc = state_json(m_state);
    }
    write_state_doc(dir, doc);
}

void SpoolQueue::resume_event() {
    std::lock_guard<std::mutex> iol(m_io_mtx);
    std::string dir, doc;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_state.valid) throw std::runtime_error("spool: nothing to resume");
        m_state.ended = false;
        m_state.last_activity_ms = now_ms();
        dir = m_dir;
        doc = state_json(m_state);
    }
    write_state_doc(dir, doc);
}

std::string SpoolQueue::enqueue(SpooledSegment seg) {
    std::vector<SpoolDrop> drops;
    std::string checksum;
    {
        // Writers are serialised by m_io_mtx, not by m_mtx: an in-flight segment
        // write must not race a begin_event() that swaps the folder, and two
        // state.json documents must not land out of order. Readers — state(),
        // pending_count(), Session::status() — take only m_mtx and are never
        // behind this (obs-multisite#36: one enqueue took 336 ms on a ROCK 5B
        // encoder and every one of those waited for it).
        std::lock_guard<std::mutex> iol(m_io_mtx);
        std::string dir;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_dir.empty()) throw std::runtime_error("spool: no event begun");
            dir = m_dir;   // stable while m_io_mtx is held
        }

        // Hash and write with NO spool lock held. This is the 336 ms window.
        seg.checksum = sha256_hex(seg.data);
        checksum = seg.checksum;

        // 1) segment bytes (atomic)
        atomic_write(seg_path_in(dir, seg.seq), seg.data.data(), seg.data.size());
        if (m_enqueue_pause) m_enqueue_pause();
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
        atomic_write(meta_path_in(dir, seg.seq), ms.data(), ms.size());

        // The eviction list is taken before m_mtx so the directory scan is not
        // done under it either; m_io_mtx keeps the folder still.
        std::vector<uint64_t> pend;
        if (m_max_bytes > 0) pend = pending_seqs_in(dir);

        std::vector<std::string> remove;
        std::string doc;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (seg.seq > m_state.last_enqueued) m_state.last_enqueued = seg.seq;
            m_state.media_end_s = std::max(m_state.media_end_s,
                                           seg.pts_offset_s + seg.duration_s);
            m_state.last_activity_ms = now_ms();
            m_bytes_pending += seg.data.size();

            // Over the disk cap: drop the OLDEST unconfirmed segments (never the
            // one just written) until back under it, or only one is left.
            if (m_max_bytes > 0) {
                size_t i = 0;
                while (m_bytes_pending > m_max_bytes && pend.size() - i > 1) {
                    uint64_t victim = pend[i];
                    if (victim == seg.seq) break; // never evict what we just wrote
                    std::error_code ec;
                    auto sz = fs::file_size(seg_path_in(dir, victim), ec);
                    if (!ec && sz <= m_bytes_pending) m_bytes_pending -= sz;
                    if (victim + 1 > m_state.first_seq) m_state.first_seq = victim + 1;
                    m_dropped_count++;
                    drops.push_back(SpoolDrop{ victim, m_state.first_seq });
                    remove.push_back(seg_path_in(dir, victim));
                    remove.push_back(meta_path_in(dir, victim));
                    ++i;
                }
            }
            doc = state_json(m_state);
        }
        // Removals stay ahead of the state write, the order the crash rule was
        // written for: a .seg with no .meta is skipped, and a floor that is
        // ahead of a segment still on disk can never be uploaded.
        for (const auto& p : remove) { std::error_code ec; fs::remove(p, ec); }
        write_state_doc(dir, doc);
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
    std::string dir;
    bool     any;
    uint64_t mark;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        dir  = m_dir;
        any  = target == 1 ? m_state.any_confirmed_2
                           : m_state.any_confirmed;
        mark = target == 1 ? m_state.last_confirmed_2
                           : m_state.last_confirmed;
    }
    // Listing the folder and reading the segment are disk work, so both happen
    // with m_mtx released: an uploader reading a multi-megabyte segment used to
    // hold the lock for the whole read, and every status() with it. A file that
    // is confirmed, evicted or removed between the list and the read is simply
    // not offered this time — the uploader comes back in 50 ms, and the floor
    // check in upload_one() still refuses a segment the spool has given up on.
    for (uint64_t seq : pending_seqs_in(dir)) {
        if (any && seq <= mark) continue;      // this target already has it
        std::string mp = meta_path_in(dir, seq), sp = seg_path_in(dir, seq);
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
    std::string dir;
    bool     any;
    uint64_t mark;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        dir  = m_dir;
        any  = target == 1 ? m_state.any_confirmed_2
                           : m_state.any_confirmed;
        mark = target == 1 ? m_state.last_confirmed_2
                           : m_state.last_confirmed;
    }
    // "Nothing is waiting for this target" — the same predicate peek_next uses,
    // so the yield rule and the queue can never disagree about whether there is
    // work. Deliberately not derived from last_enqueued: that starts at
    // first_seq-1 too, and carries the same ambiguity about sequence 0.
    // The listing is done with no lock held; the answer may be one tick stale,
    // which is all a yield rule needs (it is re-asked every 50–200 ms).
    for (uint64_t seq : pending_seqs_in(dir))
        if (!(any && seq <= mark)) return false;
    return true;
}

uint64_t SpoolQueue::behind(int target) const {
    std::string dir;
    bool     any;
    uint64_t mark;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        dir  = m_dir;
        any  = target == 1 ? m_state.any_confirmed_2
                           : m_state.any_confirmed;
        mark = target == 1 ? m_state.last_confirmed_2
                           : m_state.last_confirmed;
    }
    uint64_t n = 0;
    for (uint64_t seq : pending_seqs_in(dir))
        if (!(any && seq <= mark)) ++n;
    return n;
}

int SpoolQueue::targets() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state.targets;
}

void SpoolQueue::set_targets(int n) {
    std::lock_guard<std::mutex> iol(m_io_mtx);
    std::string dir, doc;
    std::vector<std::string> remove;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (n < 1) n = 1;
        if (n > 2) n = 2;
        m_state.targets = n;
        dir = m_dir;
        // Going back to one target: anything the remaining target already has is
        // no longer waiting on anybody, so remove it now rather than leaving
        // orphans on disk until the next event clears them.
        if (n < 2 && !dir.empty()) {
            std::error_code ec;
            for (uint64_t seq : pending_seqs_in(dir)) {
                if (!m_state.any_confirmed || seq > m_state.last_confirmed) continue;
                auto sz = fs::file_size(seg_path_in(dir, seq), ec);
                if (!ec && sz <= m_bytes_pending) m_bytes_pending -= sz;
                remove.push_back(seg_path_in(dir, seq));
                remove.push_back(meta_path_in(dir, seq));
            }
        }
        // Deliberately does NOT touch last_activity_ms: that clock is how long
        // the EVENT has been quiet, and how many buckets it goes to is
        // configuration. Stamping it here made a stale event read as freshly
        // alive the moment Session normalised the target count — which the
        // session test caught as a resume prompt that stopped appearing.
        doc = state_json(m_state);
    }
    // Same order as the original (remove, then record), with the fsync after
    // the lock instead of under it.
    for (const auto& p : remove) { std::error_code ec; fs::remove(p, ec); }
    write_state_doc(dir, doc);
}

void SpoolQueue::confirm(uint64_t seq, int target) {
    std::lock_guard<std::mutex> iol(m_io_mtx);
    std::string dir, doc;
    bool remove = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        dir = m_dir;

        uint64_t& mark = (target == 1) ? m_state.last_confirmed_2
                                       : m_state.last_confirmed;
        if (target == 1) m_state.any_confirmed_2 = true;
        else             m_state.any_confirmed   = true;
        if (seq > mark) mark = seq;
        m_state.last_activity_ms = now_ms();
        doc = state_json(m_state);

        // Removed only once every target in play holds it.
        //
        // Each half is `any && seq <= mark`, NOT `seq <= mark` — the sequence-0
        // ambiguity, in its third and worst form. `last_confirmed_2` starts at 0
        // both when the second target has nothing and when it has sequence 0, so a
        // bare `seq <= last_confirmed_2` said "the second target already has
        // sequence 0" and deleted the file while only the primary had it. The
        // segment then never reached the second bucket, which is the one thing this
        // whole mechanism exists to prevent.
        const bool has_primary =
            m_state.any_confirmed && seq <= m_state.last_confirmed;
        const bool has_second =
            m_state.targets < 2 ||
            (m_state.any_confirmed_2 && seq <= m_state.last_confirmed_2);
        if (has_primary && has_second) {
            remove = true;
            std::error_code ec;
            auto sz = fs::file_size(seg_path_in(dir, seq), ec);
            if (!ec && sz <= m_bytes_pending) m_bytes_pending -= sz;
        }
    }
    // The state write comes first, as it always did: the position is durable
    // before the file it refers to disappears. Then the removals, with m_mtx
    // released (the fsync was the part status() used to wait on).
    write_state_doc(dir, doc);
    if (remove) {
        std::error_code ec;
        fs::remove(seg_path_in(dir, seq), ec);
        fs::remove(meta_path_in(dir, seq), ec);
    }
}

void SpoolQueue::mark_ended(bool published) {
    std::lock_guard<std::mutex> iol(m_io_mtx);
    std::string dir, doc;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_state.ended = true;
        m_state.end_unpublished = !published;
        dir = m_dir;
        doc = state_json(m_state);
    }
    write_state_doc(dir, doc);
}

size_t SpoolQueue::pending_count() const {
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        dir = m_dir;
    }
    // The count is a scan of the folder: no lock, so Session::status() never
    // waits on the disk for it (obs-multisite#36).
    return pending_seqs_in(dir).size();
}

SpoolState SpoolQueue::state() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state;
}

} // namespace multisite
