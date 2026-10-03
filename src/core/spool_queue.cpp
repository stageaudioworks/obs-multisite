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
            save_state();
        }
        m_dir = best;
    }
    load_state();
    // Recompute pending bytes from what's actually on disk (not persisted:
    // trivial to get wrong across a crash, trivial to recompute here).
    for (uint64_t seq : pending_seqs()) {
        std::error_code ec;
        auto sz = fs::file_size(seg_path(seq), ec);
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
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_dir.empty() || !m_state.ended || m_state.end_unpublished ||
        has_segments(m_dir)) return false;
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    m_dir.clear();
    m_bytes_pending = 0;
    return true;
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

void SpoolQueue::save_state() {
    if (m_dir.empty()) return;   // no current event: nothing of its own to record
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
    j["media_end_s"]    = m_state.media_end_s;
    j["end_unpublished"] = m_state.end_unpublished;
    std::string s = j.dump();
    atomic_write(state_path(), s.data(), s.size());
}

std::vector<uint64_t> SpoolQueue::pending_seqs() const {
    std::vector<uint64_t> out;
    if (m_dir.empty() || !fs::exists(m_dir)) return out;
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
    r.media_end_s    = m_state.media_end_s;
    return r;
}

void SpoolQueue::begin_event(const std::string& event_id, uint64_t first_seq) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // The previous event is NOT cleared. It used to be — every .seg and .meta
    // deleted — which threw away whatever it had not yet uploaded. Now it is
    // ended and left in its folder for EventFinisher, or removed if there is
    // nothing left in it to send.
    if (!m_dir.empty() && m_state.valid) {
        // An event left unfinished (it was never Ended) is over now; storage
        // has never been told, so that is owed too, with whatever it had left.
        if (!m_state.ended) m_state.end_unpublished = true;
        m_state.ended = true;
        save_state();
        if (!has_segments(m_dir) && !m_state.end_unpublished) {
            std::error_code ec; fs::remove_all(m_dir, ec);
        }
    }
    m_dir = (fs::path(m_root) / event_id).string();
    {
        std::error_code ec;
        fs::remove_all(m_dir, ec);   // a fresh id; nothing of its own to keep
        fs::create_directories(m_dir);
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
        if (m_dir.empty()) throw std::runtime_error("spool: no event begun");
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
        m_state.media_end_s = std::max(m_state.media_end_s,
                                       seg.pts_offset_s + seg.duration_s);
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

uint64_t SpoolQueue::behind(int target) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const bool     any  = target == 1 ? m_state.any_confirmed_2
                                      : m_state.any_confirmed;
    const uint64_t mark = target == 1 ? m_state.last_confirmed_2
                                      : m_state.last_confirmed;
    uint64_t n = 0;
    for (uint64_t seq : pending_seqs())
        if (!(any && seq <= mark)) ++n;
    return n;
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
    // Deliberately does NOT touch last_activity_ms: that clock is how long the
    // EVENT has been quiet, and how many buckets it goes to is configuration.
    // Stamping it here made a stale event read as freshly alive the moment
    // Session normalised the target count — which the session test caught as a
    // resume prompt that stopped appearing.
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
    if (!(has_primary && has_second)) return;

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

void SpoolQueue::mark_ended(bool published) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_state.ended = true;
    m_state.end_unpublished = !published;
    save_state();
}

SpoolState SpoolQueue::state() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state;
}

} // namespace multisite
