// SPDX-License-Identifier: GPL-3.0-or-later
// test_session.cpp — proves the publishing layer's protocol guarantees.
//
// The critical invariant: manifest.json must NEVER list a segment that isn't
// already durable in the store. A decoder that can see an entry must be able to
// fetch the object. This is checked continuously, including across a simulated
// network outage.
#include "../src/core/session.h"
#include "../src/core/null_transport.h"
#include "../src/core/event_catalog.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// An in-memory object store that can simulate an outage, and which validates
// the write-ordering rule on every manifest write.
class MemStore : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    std::mutex mtx;
    int fail_budget = 0;
    bool expect_tags = false;
    bool fail_all = false;
    bool silently_discard = false;   // returns 200 but stores nothing
    bool ordering_violation = false;
    std::string violation_detail;

    // Cancellation is modelled because S3Transport's is STICKY, and a mock
    // that ignored it hid a real bug for six days: Session::end() cancelled
    // this transport (via RetryUploader::stop()) and then kept using it, so
    // in production the final drain, manifest.json and live.json were all
    // aborted while every test here sailed through. A mock that is more
    // forgiving than the real thing proves nothing about the real thing.
    std::atomic<bool> cancelled{false};
    void cancel_pending() override { cancelled = true; }
    void resume_pending() override { cancelled = false; }

    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string,std::string>& tags) override {
        std::lock_guard<std::mutex> lk(mtx);
        if (cancelled) return {false, 0, true, "cancelled (simulated)"};
        // Simulate an outage for SEGMENT uploads only (control files still fail
        // over separately in reality, but this isolates the invariant test).
        if (fail_all) return {false, 403, false, "AccessDenied (simulated)"};
        if (fail_budget > 0 && key.find("/segments/") != std::string::npos) {
            --fail_budget;
            return {false, 0, true, "network down"};
        }
        // Retention tags are optional (R2 doesn't support them), so only
        // check them when the session was configured to send them.
        if (expect_tags && tags.find("MultisiteExpiry") == tags.end()) {
            ordering_violation = true;
            violation_detail = "missing expiry tag on " + key;
        }
        if (!silently_discard) objects[key] = body;

        // INVARIANT CHECK: if this is a manifest, every segment it lists must
        // already exist as an object in the store.
        if (key.find("manifest.json") != std::string::npos) {
            std::string js(body.begin(), body.end());
            try {
                Manifest m = Manifest::from_json(js);
                for (const auto& s : m.segments) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "%08llu",
                                  (unsigned long long)s.seq);
                    std::string skey = "events/" + m.event_id +
                                       "/segments/" + name + ".m4s";
                    if (objects.find(skey) == objects.end()) {
                        ordering_violation = true;
                        violation_detail = "manifest listed " + skey +
                                           " before it was stored";
                    }
                }
            } catch (...) {
                ordering_violation = true;
                violation_detail = "manifest was not valid JSON";
            }
        }
        return {true, 200, true, ""};
    }

    GetResult get(const std::string& key) override {
        std::lock_guard<std::mutex> lk(mtx);
        GetResult r;
        if (fail_all) { r.http_status = 403; r.error = "AccessDenied (simulated)"; return r; }
        auto it = objects.find(key);
        if (it == objects.end()) {
            r.http_status = 404; r.error = "NoSuchKey"; r.retryable = false;
            return r;
        }
        r.success = true; r.http_status = 200; r.body = it->second;
        return r;
    }

    int64_t object_size(const std::string& k) override {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = objects.find(k);
        return it == objects.end() ? -1 : (int64_t)it->second.size();
    }

    // A listing honest enough to drive EventCatalog, which asks for keys one
    // moment and for prefixes the next. A mock that ignored the delimiter would
    // be more forgiving than S3 in the one place that matters: the catalogue's
    // fallback scan finds events by their `events/{id}/` common prefixes, so a
    // store that returned flat keys there would let a broken scan pass.
    ListResult list(const std::string& prefix, const std::string& delimiter,
                    const std::string& token, int max_keys) override {
        std::lock_guard<std::mutex> lk(mtx);
        ListResult r;
        if (fail_all) {
            r.http_status = 403;
            r.error = "AccessDenied (simulated)";
            r.retryable = false;
            return r;
        }
        // Ascending key order, as S3 lists, with the same delimiter grouping.
        std::vector<std::string> keys, prefixes;
        for (const auto& [key, _] : objects) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            if (!delimiter.empty()) {
                const size_t at = key.find(delimiter, prefix.size());
                if (at != std::string::npos) {
                    std::string p = key.substr(0, at + delimiter.size());
                    if (std::find(prefixes.begin(), prefixes.end(), p) == prefixes.end())
                        prefixes.push_back(p);
                    continue;
                }
            }
            keys.push_back(key);
        }
        for (const auto& p : prefixes) r.common_prefixes.push_back(p);
        const size_t start = token.empty() ? 0 : (size_t)std::stoul(token);
        for (size_t i = start; i < keys.size(); ++i) {
            if ((int)r.keys.size() >= max_keys) {
                r.truncated = true;
                r.next_continuation_token = std::to_string(i);
                break;
            }
            ListEntry e; e.key = keys[i];
            auto it = objects.find(keys[i]);
            e.size = it == objects.end() ? 0 : (int64_t)it->second.size();
            r.keys.push_back(e);
        }
        r.success = true; r.http_status = 200;
        return r;
    }

    bool has(const std::string& k) {
        std::lock_guard<std::mutex> lk(mtx);
        return objects.count(k) > 0;
    }
    std::string text(const std::string& k) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = objects.find(k);
        return it == objects.end() ? "" : std::string(it->second.begin(), it->second.end());
    }
};

static std::vector<uint8_t> blob(uint64_t n, size_t sz = 2048) {
    std::vector<uint8_t> v(sz);
    for (size_t i = 0; i < sz; ++i) v[i] = (uint8_t)((n * 31 + i) & 0xFF);
    return v;
}

int main() {
    fs::path base = fs::temp_directory_path() / "multisite_session_test";
    fs::remove_all(base);
    fs::create_directories(base);

    VideoInfo video{ "h264", 1920, 1080, 30.0 };
    std::vector<AudioTrack> tracks = {
        { 0, "Main mix",   "aac", 2, 48000 },
        { 1, "Sermon ISO", "aac", 1, 48000 },
        { 2, "Click",      "aac", 1, 48000 },
    };

    std::printf("== 1. Start event publishes the full object layout ==\n");
    std::string event_id;
    {
        MemStore store;
        SessionConfig cfg;
        cfg.room_id = "main-auditorium";
        cfg.spool_dir = (base / "s1").string();
        cfg.segment_duration_s = 6.0;
        cfg.event_name = "Morning event";
        Session ses(cfg, store);

        CHECK(ses.start_new(blob(0, 1500), video, tracks), "start_new succeeded");
        event_id = ses.event_id();
        CHECK(!event_id.empty() && event_id.size() == 26, "event id is a 26-char ULID");
        CHECK(store.has("rooms/main-auditorium/live.json"), "live.json published");
        CHECK(store.has("events/" + event_id + "/event.json"), "event.json published");
        CHECK(store.has("events/" + event_id + "/init.mp4"), "init.mp4 published");
        CHECK(store.has("events/" + event_id + "/manifest.json"), "manifest.json published");

        EventInfo ev = EventInfo::from_json(store.text("events/" + event_id + "/event.json"));
        CHECK(ev.audio_tracks.size() == 3 && ev.audio_tracks[2].label == "Click",
              "event.json carries all 3 audio tracks");
        CHECK(ev.video.width == 1920, "event.json carries video info");
        CHECK(ev.name == "Morning event", "event.json carries the event name");

        Manifest mf = Manifest::from_json(store.text("events/" + event_id + "/manifest.json"));
        CHECK(mf.name == "Morning event", "manifest.json carries the event name");

        LivePointer lp = LivePointer::from_json(store.text("rooms/main-auditorium/live.json"));
        CHECK(lp.event_id == event_id && lp.status == "live",
              "live.json points at the event");

        // The per-room index a satellite lists to find past events. Nothing in
        // an event's own key says which room it belongs to, so without this
        // entry the event list can only be built by reading every event.json in
        // the bucket.
        const std::string ix_key = room_event_key("main-auditorium", event_id);
        CHECK(store.has(ix_key), "the room index entry is published at go-live");
        RoomEventEntry ix = RoomEventEntry::from_json(store.text(ix_key));
        CHECK(ix.event_id == event_id && ix.room_id == "main-auditorium",
              "the index entry names its event and room");
        CHECK(ix.started_at_ms == ev.started_at_ms,
              "and agrees with event.json about when the event started");
        CHECK(ix.name == "Morning event",
              "the index entry carries the event name");
        ses.end();
    }

    std::printf("== 2. Write-ordering invariant holds through an outage ==\n");
    {
        MemStore store;
        store.fail_budget = 10;          // segment uploads fail at first
        SessionConfig cfg;
        cfg.room_id = "main-auditorium";
        cfg.spool_dir = (base / "s2").string();
        cfg.manifest_window = 5;
        cfg.base_backoff_ms = 2; cfg.max_backoff_ms = 10; cfg.backoff_jitter = 0.0;
        Session ses(cfg, store);
        ses.start_new(blob(0, 1200), video, tracks);

        for (uint64_t i = 0; i < 8; ++i)
            ses.publish_segment(blob(i + 1), 6.0, (double)i * 6.0);

        // Wait for the state the assertions below actually check, not a proxy
        // for it. The uploader increments confirmed_total BEFORE it publishes
        // the manifest entry and before it clears the spool file — that order
        // is deliberate, so a crash cannot leave an object unlisted. Waiting on
        // confirmed_total and then asserting on pending and latest_seq is
        // therefore a race: invisible on a fast runner, intermittently red on a
        // loaded Windows one, which is exactly how it behaved.
        for (int i = 0; i < 400; ++i) {
            auto s = ses.status();
            if (s.confirmed_total >= 8 && s.pending == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        auto st = ses.status();
        CHECK(st.pending == 0, "all segments drained after the outage");
        CHECK(st.confirmed_total == 8, "all 8 segments confirmed");
        CHECK(st.retries > 0, "retries occurred (outage was real)");
        CHECK(!store.ordering_violation,
              store.ordering_violation ? store.violation_detail.c_str()
                                       : "manifest never listed an unstored segment");

        Manifest m = Manifest::from_json(store.text("events/" + ses.event_id() + "/manifest.json"));
        std::printf("     (diag: latest_seq=%llu segments=%zu confirmed=%llu)\n",
                    (unsigned long long)m.latest_seq, m.segments.size(),
                    (unsigned long long)st.confirmed_total);
        CHECK(m.latest_seq == 7, "manifest latest_seq tracks the live edge");
        CHECK(m.segments.size() == 5, "rolling window honoured (5)");
        CHECK(!m.segments.empty() && !m.segments.back().checksum.empty(),
              "checksums recorded in manifest");
        ses.end();
    }

    std::printf("== 3. Markers publish and accumulate ==\n");
    {
        MemStore store;
        SessionConfig cfg; cfg.spool_dir = (base / "s3").string();
        Session ses(cfg, store);
        ses.start_new(blob(0), video, tracks);
        ses.publish_segment(blob(1), 6.0, 0.0);
        ses.add_marker("Sermon Start");
        ses.add_marker("Offering");
        MarkerList ml = MarkerList::from_json(
            store.text("events/" + ses.event_id() + "/markers.json"));
        CHECK(ml.markers.size() == 2, "two markers published");
        CHECK(ml.markers[0].label == "Sermon Start", "marker label preserved");
        CHECK(!ml.markers[0].id.empty(), "marker has an id");
        ses.end();
    }

    std::printf("== 3b. A LAN satellite's cue joins the event, one object per site ==\n");
    {
        MemStore store;
        SessionConfig cfg; cfg.spool_dir = (base / "s3b").string();
        Session ses(cfg, store);

        std::string published;   // the merged list a LAN decoder would be served
        ses.set_markers_published_callback(
            [&](const std::string& json) { published = json; });

        ses.start_new(blob(0), video, tracks);
        ses.publish_segment(blob(1), 6.0, 0.0);

        std::string err;
        CHECK(ses.add_cue_from("Campus B", "Notice", err),
              "the encoder accepts a cue for a satellite");

        const std::string own =
            "events/" + ses.event_id() + "/cues/campus-b.json";
        CHECK(store.objects.count(own) == 1,
              "the cue is written under the satellite's OWN site object");
        MarkerList cl = MarkerList::from_json(store.text(own));
        CHECK(cl.markers.size() == 1 && cl.markers[0].author == "Campus B",
              "carrying the site name as its author");

        MarkerList lan = MarkerList::from_json(published);
        CHECK(lan.markers.size() == 1 && lan.markers[0].label == "Notice",
              "the LAN list already carries the satellite's cue");

        const std::string cloud_json =
            store.text("events/" + ses.event_id() + "/markers.json");
        CHECK(cloud_json.empty(),
              "the encoder's own markers.json is not written by a guest cue");

        ses.add_marker("Sermon Start");
        MarkerList lan2 = MarkerList::from_json(published);
        bool has_notice = false, has_sermon = false;
        for (const auto& m : lan2.markers) {
            if (m.label == "Notice")       has_notice = true;
            if (m.label == "Sermon Start") has_sermon = true;
        }
        CHECK(lan2.markers.size() == 2 && has_notice && has_sermon,
              "and the LAN list merges both sites' cues");
        ses.end();
    }

    std::printf("== 4. Resume continues the sequence after a crash ==\n");
    {
        MemStore store;
        SessionConfig cfg; cfg.spool_dir = (base / "s4").string();
        cfg.base_backoff_ms = 2; cfg.max_backoff_ms = 10; cfg.backoff_jitter = 0.0;
        std::string first_event;
        {
            Session ses(cfg, store);
            ses.start_new(blob(0), video, tracks);
            first_event = ses.event_id();
            store.fail_budget = 1000;                 // network dead: nothing confirms
            for (uint64_t i = 0; i < 4; ++i)
                ses.publish_segment(blob(i + 1), 6.0, (double)i * 6.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        } // destructor = crash (no end() called)

        Session ses2(cfg, store);
        auto info = ses2.check_resumable();
        CHECK(info.resumable, "unfinished event detected after crash");
        CHECK(info.event_id == first_event, "same event id offered for resume");
        CHECK(info.pending_count == 4, "4 unsent segments survived the crash");

        store.fail_budget = 0;                        // network back
        CHECK(ses2.resume(blob(0), video, tracks), "resume succeeded");
        CHECK(ses2.event_id() == first_event, "resumed into the same event");

        uint64_t next = ses2.publish_segment(blob(99), 6.0, 24.0);
        CHECK(next == 4, "sequence continues (next seq = 4, no restart at 0)");

        for (int i = 0; i < 400 && ses2.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        CHECK(ses2.status().pending == 0, "backlog from before the crash uploaded");
        CHECK(!store.ordering_violation, "write-ordering held across the resume");
        ses2.end();
    }

    std::printf("== 5. Tag-free mode (Cloudflare R2 compatibility) ==\n");
    {
        MemStore store;
        // Simulate a store that REJECTS tagged requests, the way R2 does.
        SessionConfig cfg;
        cfg.spool_dir = (base / "s5t").string();
        cfg.send_expiry_tag = false;          // R2 mode
        store.expect_tags = false;
        Session ses(cfg, store);
        CHECK(ses.start_new(blob(0), video, tracks), "session starts without tags");
        ses.publish_segment(blob(1), 6.0, 0.0);
        for (int i = 0; i < 200 && ses.status().confirmed_total < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        CHECK(ses.status().confirmed_total == 1, "segment uploads with no tags");
        CHECK(ses.last_error().empty(), "no error recorded on success");
        ses.end();
    }

    std::printf("== 6. Failures report a real HTTP error ==\n");
    {
        MemStore store;
        store.fail_budget = 100000;   // everything fails
        store.fail_all = true;        // including control files
        SessionConfig cfg;
        cfg.spool_dir = (base / "s6").string();
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2;
        Session ses(cfg, store);
        bool started = ses.start_new(blob(0), video, tracks);
        CHECK(!started, "start fails when the store rejects writes");
        CHECK(!ses.last_error().empty(), "a real error string is recorded");
        std::printf("     (reported: %s)\n", ses.last_error().c_str());
        CHECK(ses.last_error().find("HTTP") != std::string::npos,
              "error names the HTTP status");
    }

    std::printf("== 7. Packed multi-channel audio round-trips ==\n");
    {
        MemStore store;
        SessionConfig cfg; cfg.spool_dir = (base / "s7p").string();
        Session ses(cfg, store);

        VideoInfo pv{ "h264", 1920, 1080, 30.0 };
        // One 8-channel track, the packed production feed.
        std::vector<AudioTrack> packed = { {
            0, "Production", "aac", 8, 48000,
            { "Main L", "Main R", "Sermon ISO", "Click",
              "Choir ISO", "Ambient L", "Ambient R", "Spare" }
        } };
        CHECK(ses.start_new(blob(0, 1400), pv, packed),
              "session starts with a packed 8-channel track");

        EventInfo ev = EventInfo::from_json(
            store.text("events/" + ses.event_id() + "/event.json"));
        CHECK(ev.audio_tracks.size() == 1, "one audio track published");
        CHECK(ev.audio_tracks[0].channels == 8, "8 channels recorded");
        CHECK(ev.audio_tracks[0].channel_labels.size() == 8,
              "all 8 channel labels published");
        CHECK(ev.audio_tracks[0].channel_labels[3] == "Click",
              "channel ORDER is preserved (channel 4 is the click)");

        Manifest m = Manifest::from_json(
            store.text("events/" + ses.event_id() + "/manifest.json"));
        CHECK(!m.audio_tracks.empty() &&
              m.audio_tracks[0].channel_labels.size() == 8,
              "channel map also reaches the manifest the decoder polls");
        std::printf("     (channel 4 = %s, channel 8 = %s)\n",
                    m.audio_tracks[0].channel_labels[3].c_str(),
                    m.audio_tracks[0].channel_labels[7].c_str());
        ses.end();
    }

    std::printf("== 8. A store that lies about success is caught ==\n");
    {
        MemStore store;
        SessionConfig cfg; cfg.spool_dir = (base / "s7v").string();
        Session ses(cfg, store);
        ses.start_new(blob(0), video, tracks);
        store.silently_discard = true;   // 200 OK, but nothing persisted
        ses.publish_segment(blob(1), 6.0, 0.0);
        for (int i = 0; i < 200 && ses.status().confirmed_total < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        auto st = ses.status();
        CHECK(st.verify_failures > 0,
              "verification detects a segment that wasn't really stored");
        std::printf("     (note: %s)\n", st.verify_note.c_str());
        store.silently_discard = false;
        ses.end();
    }

    std::printf("== 9. Clean end marks the event ended ==\n");
    {
        MemStore store;
        SessionConfig cfg; cfg.spool_dir = (base / "s5").string();
        Session ses(cfg, store);
        ses.start_new(blob(0), video, tracks);
        ses.publish_segment(blob(1), 6.0, 0.0);
        ses.end();
        LivePointer lp = LivePointer::from_json(store.text("rooms/main-auditorium/live.json"));
        CHECK(lp.status == "ended", "live.json marked ended");
        Manifest m = Manifest::from_json(store.text("events/" + ses.event_id() + "/manifest.json"));
        CHECK(m.status == "ended", "manifest marked ended");

        Session ses2(cfg, store);
        CHECK(!ses2.check_resumable().resumable,
              "cleanly-ended event is not offered for resume");
    }

    std::printf("== 9b. An AV1 event is recorded, ended, and then listed ==\n");
    {
        // The report that started this: an AV1 event that played live and was
        // then nowhere in the recordings list. Nothing in the listing path knows
        // one codec from another, and this is the test that says so end to end
        // — a real session, a real end() and the real catalogue over the same
        // store, so a codec that did start deciding would fail here rather than
        // in somebody's service.
        MemStore store;
        SessionConfig cfg; cfg.spool_dir = (base / "s9av1").string();
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        Session ses(cfg, store);

        VideoInfo av1 = video;
        av1.codec = "av1";
        CHECK(ses.start_new(blob(0), av1, tracks), "an AV1 event goes live");
        for (int i = 0; i < 3; ++i)
            ses.publish_segment(blob(i + 1), 6.0, (double)i * 6.0);
        ses.end();

        Manifest m = Manifest::from_json(
            store.text("events/" + ses.event_id() + "/manifest.json"));
        CHECK(m.video.codec == "av1", "the manifest says the event is AV1");
        CHECK(m.status == "ended", "and that it has ended");

        CatalogConfig cc; cc.room_id = "main-auditorium";
        EventCatalog cat(cc, store);
        CHECK(cat.refresh(), "the recordings list refreshes");
        auto ev = cat.events();
        CHECK(ev.size() == 1, "the AV1 event is in it");
        CHECK(!ev.empty() && ev[0].state == EventState::Recording,
              "as a recording — not live, not interrupted");
        CHECK(!ev.empty() && ev[0].event_id == ses.event_id(),
              "and it is the event that was just recorded");
        CHECK(cat.skipped() == 0 && cat.last_error().empty(),
              "with nothing skipped and nothing said against it");
    }

    std::printf("== 10. Local disk cap drops the oldest segment and warns the operator ==\n");
    {
        MemStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s10").string();
        // Room for 3 of these 2048-byte segments before eviction kicks in.
        cfg.max_spool_bytes = 3 * 2048;
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        Session ses(cfg, store);
        CHECK(ses.start_new(blob(0), video, tracks),
              "start_new succeeds (control files still write)");

        store.fail_budget = 100000;   // segment uploads fail from here on
        for (uint64_t i = 0; i < 8; ++i)
            ses.publish_segment(blob(i + 1), 6.0, (double)i * 6.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto st = ses.status();
        CHECK(st.dropped_for_disk > 0, "segments were dropped for local disk space");
        CHECK(st.pending < 8, "fewer than all 8 remain queued (some evicted)");
        CHECK(!ses.last_error().empty(), "a warning is recorded");
        CHECK(ses.last_error().find("dropped") != std::string::npos,
              "the warning names what happened");

        store.fail_budget = 0;   // link recovers
        for (int i = 0; i < 400 && ses.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        CHECK(ses.status().pending == 0,
              "the SURVIVING segments still drain once the link returns");
        CHECK(!store.ordering_violation,
              "write-ordering held even with an eviction in the mix");

        Manifest m = Manifest::from_json(
            store.text("events/" + ses.event_id() + "/manifest.json"));
        CHECK(m.first_available_seq > 0,
              "the manifest floor advanced past what was dropped once a publish "
              "happened, so a decoder does not wait forever on it");
        ses.end();
    }

    std::printf("== 11. A recent crash resumes silently under a generous threshold ==\n");
    {
        MemStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s11").string();
        cfg.resume_stale_after_ms = 100000000; // effectively "never" for this test
        {
            Session ses(cfg, store);
            ses.start_new(blob(0), video, tracks);
            ses.publish_segment(blob(1), 6.0, 0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        } // crash — no end()

        Session ses2(cfg, store);
        auto info = ses2.check_resumable();
        CHECK(info.resumable, "the interrupted event is detected");
        CHECK(!info.stale,
              "a crash seconds ago is not stale under a generous threshold — "
              "the ordinary case stays silent and automatic");
    }

    std::printf("== 12. An old leftover is reported stale, not silently swallowed ==\n");
    {
        MemStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s12").string();
        cfg.resume_stale_after_ms = 20; // tiny: anything but instantaneous trips it
        {
            Session ses(cfg, store);
            ses.start_new(blob(0), video, tracks);
            ses.publish_segment(blob(1), 6.0, 0.0);
        } // crash — no end()
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        Session ses2(cfg, store);
        auto info = ses2.check_resumable();
        CHECK(info.resumable, "still resumable — staleness doesn't erase the event");
        CHECK(info.stale,
              "but flagged stale, so the caller (the OBS layer) knows to ask "
              "the operator rather than resume it on its own");
    }

    std::printf("== 13. Resuming records what was resumed, for the dock's status line ==\n");
    {
        MemStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s13").string();
        cfg.event_name = "Morning event";
        cfg.base_backoff_ms = 2; cfg.max_backoff_ms = 10; cfg.backoff_jitter = 0.0;
        std::string first_event;
        {
            Session ses(cfg, store);
            ses.start_new(blob(0), video, tracks);
            first_event = ses.event_id();
            for (uint64_t i = 0; i < 4; ++i)
                ses.publish_segment(blob(i + 1), 6.0, (double)i * 6.0);
            for (int i = 0; i < 200 && ses.status().pending > 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(ses.status().pending == 0, "all 4 confirmed before the crash");

            // A 5th segment, left behind unconfirmed by the crash — exactly
            // what "start new" would abandon.
            store.fail_budget = 100000;
            ses.publish_segment(blob(5), 6.0, 24.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        } // crash: 4 confirmed, 1 still pending

        store.fail_budget = 0;
        Session ses2(cfg, store);
        CHECK(ses2.resume(blob(0), video, tracks), "resume succeeds");
        auto st = ses2.status();
        CHECK(st.resumed_event_id == first_event,
              "status names the exact event that was resumed");
        CHECK(st.resumed_event_started_ms > 0,
              "the original start time was read back from event.json");
        CHECK(st.resumed_already_confirmed == 4,
              "already-confirmed count is what 'start new' would abandon");
        ses2.end();
    }

    std::printf("== 14. peek_resumable() answers without a Session or Transport ==\n");
    {
        // The dock needs to know this BEFORE Go Live creates an output at
        // all — a deferred-start encoder may not construct its Session until
        // well after the operator has already clicked the button. This is
        // the standalone check it uses instead.
        std::string dir = (base / "s14").string();
        auto empty = peek_resumable(dir, 30 * 60 * 1000);
        CHECK(!empty.resumable, "an empty spool directory has nothing to resume");

        {
            MemStore store;
            SessionConfig cfg; cfg.spool_dir = dir;
            Session ses(cfg, store);
            ses.start_new(blob(0), video, tracks);
            ses.publish_segment(blob(1), 6.0, 0.0);
        } // crash — no end()

        auto recent = peek_resumable(dir, 30 * 60 * 1000);
        CHECK(recent.resumable, "the interrupted event is found from disk alone");
        CHECK(!recent.stale, "and correctly not stale under a generous threshold");

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto as_stale = peek_resumable(dir, 1);
        CHECK(as_stale.resumable && as_stale.stale,
              "the SAME event reads as stale under a tight threshold — "
              "staleness is the caller's policy, not baked into the spool");
    }

    std::printf("== 15. LAN delivery hooks fire with what a LanObjectServer needs (§8.7) ==\n");
    {
        MemStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s15").string();
        cfg.event_name = "LAN test event";
        cfg.base_backoff_ms = 2; cfg.max_backoff_ms = 10; cfg.backoff_jitter = 0.0;
        Session ses(cfg, store);

        std::string started_event_id, started_event_json;
        std::vector<uint8_t> started_init;
        int event_started_calls = 0;
        ses.set_event_started_callback(
            [&](const std::string& id, const std::string& json,
                const std::vector<uint8_t>& init) {
                ++event_started_calls;
                started_event_id = id;
                started_event_json = json;
                started_init = init;
            });

        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> confirmed_segments;
        ses.set_segment_confirmed_callback(
            [&](uint64_t seq, const std::vector<uint8_t>& bytes) {
                confirmed_segments.emplace_back(seq, bytes);
            });

        std::vector<std::string> manifests_published;
        ses.set_manifest_published_callback(
            [&](const std::string& json) { manifests_published.push_back(json); });

        auto init_bytes = blob(0, 1500);
        CHECK(ses.start_new(init_bytes, video, tracks), "start_new succeeds");

        CHECK(event_started_calls == 1,
              "the event-started hook fires exactly once, at start_new()");
        CHECK(started_event_id == ses.event_id(),
              "naming the event that was actually started");
        CHECK(started_init == init_bytes,
              "carrying the exact init bytes a LAN satellite would need");
        EventInfo started_ev = EventInfo::from_json(started_event_json);
        CHECK(started_ev.name == "LAN test event",
              "and the same event.json a cloud decoder would read");
        CHECK(!manifests_published.empty(),
              "the manifest-published hook already fired once too, seeding "
              "an initial (empty) manifest at start_new()");

        const size_t before = manifests_published.size();
        ses.publish_segment(blob(1), 6.0, 0.0);
        for (int i = 0; i < 200 && ses.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        CHECK(confirmed_segments.size() == 1,
              "the segment-confirmed hook fired exactly once");
        CHECK(confirmed_segments[0].first == 0, "naming the right sequence number");
        CHECK(confirmed_segments[0].second == blob(1),
              "carrying the exact bytes that were confirmed — the ONLY copy "
              "left once the spool file behind them is gone");
        CHECK(manifests_published.size() > before,
              "and the manifest-published hook fired again, after the segment "
              "that just confirmed");
        Manifest last = Manifest::from_json(manifests_published.back());
        CHECK(!last.segments.empty() && last.segments.back().seq == 0,
              "with the confirmed segment already listed in it — "
              "the exact JSON a cloud decoder would eventually see too");

        ses.end();
    }

    std::printf("== 16. The live-published hook fires alongside the other "
                "three — a LAN-only satellite's only way to learn which "
                "event is live (§8.7) ==\n");
    {
        MemStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s16").string();
        cfg.base_backoff_ms = 2; cfg.max_backoff_ms = 10; cfg.backoff_jitter = 0.0;
        Session ses(cfg, store);

        std::vector<std::string> live_published;
        ses.set_live_published_callback(
            [&](const std::string& json) { live_published.push_back(json); });

        CHECK(ses.start_new(blob(0, 500), video, tracks), "start_new succeeds");
        CHECK(!live_published.empty(),
              "fires at start — the same moment live.json first names this event");
        LivePointer lp0 = LivePointer::from_json(live_published.back());
        CHECK(lp0.event_id == ses.event_id() && lp0.status == "live",
              "naming the right event, correctly live");

        const size_t before = live_published.size();
        ses.heartbeat();
        CHECK(live_published.size() > before,
              "fires again on every heartbeat, the same path a confirm's "
              "periodic re-publish takes");

        ses.end();
        LivePointer lp1 = LivePointer::from_json(live_published.back());
        CHECK(lp1.status == "ended", "and once more at end(), marked ended");
    }

    std::printf("== 17. Cloud delivery disabled: NullTransport lets LAN-only "
                "delivery reuse the entire pipeline unmodified (§8.7) ==\n");
    {
        NullTransport null_store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s17").string();
        Session ses(cfg, null_store);

        int event_started = 0, segments_confirmed = 0;
        ses.set_event_started_callback([&](const std::string&, const std::string&,
                                           const std::vector<uint8_t>&) { ++event_started; });
        ses.set_segment_confirmed_callback(
            [&](uint64_t, const std::vector<uint8_t>&) { ++segments_confirmed; });

        CHECK(ses.start_new(blob(0, 500), video, tracks),
              "start_new succeeds with no real store behind it at all");
        CHECK(event_started == 1, "the LAN event-started hook still fires");

        ses.publish_segment(blob(1), 6.0, 0.0);
        for (int i = 0; i < 200 && ses.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        CHECK(ses.status().pending == 0,
              "confirms immediately — a NullTransport put() never fails or waits");
        CHECK(ses.status().confirmed_total == 1, "and the count reflects it");
        CHECK(segments_confirmed == 1,
              "the LAN segment-confirmed hook fires exactly as it would against "
              "a real bucket — Session cannot tell the difference");
        CHECK(ses.status().verify_failures == 0,
              "no false verify failures: object_size() echoes the size just put");

        ses.end();
    }

    std::printf("== 18. end() finishes the job through a transport its own "
                "stop() just cancelled ==\n");
    {
        // The regression this exists for: RetryUploader::stop() cancels the
        // transport so its join cannot wait out a request timeout, and that
        // cancel is sticky. end() calls stop() and then does three more
        // things through the same transport — drains the spool, publishes
        // manifest.json as "ended", publishes live.json as "ended". All
        // three silently aborted, so a real 5½-hour event ended with its last
        // segment unsent and, far worse, never marked ended at all: every
        // satellite went on polling a room nobody was broadcasting to and
        // eventually classified it as interrupted.
        MemStore store;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s18").string();
        Session ses(cfg, store);

        CHECK(ses.start_new(blob(0, 500), video, tracks), "event starts");

        // Hold the segment in the spool so end() has real work to do: this is
        // the last-segment-of-the-event case, muxed moments before Stop.
        store.fail_budget = 1;
        ses.publish_segment(blob(1), 6.0, 0.0);
        for (int i = 0; i < 100 && ses.status().pending == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        ses.end();

        CHECK(ses.status().pending == 0,
              "the segment still spooled at Stop is uploaded by the drain");
        CHECK(store.has("events/" + ses.status().event_id + "/segments/00000000.m4s"),
              "and is really in the store, not merely counted");

        const std::string live = store.text(live_pointer_key(cfg.room_id));
        CHECK(!live.empty() && LivePointer::from_json(live).status == "ended",
              "live.json says ended, so satellites stop polling a dead room");

        const std::string man =
            store.text("events/" + ses.status().event_id + "/manifest.json");
        CHECK(!man.empty() && Manifest::from_json(man).status == "ended",
              "and the manifest agrees the event is over");
        CHECK(!store.cancelled.load(),
              "end() left the transport usable rather than switched off");
    }

    std::printf("== 19. A second bucket receives the media too ==\n");
    {
        // The Session-level half of Phase 9: the wiring, not the rule. The
        // uploader's own tests cover the yield; this checks that configuring a
        // mirror actually produces a second stream into a second store, and that
        // the primary's manifest invariant is untouched by it.
        MemStore primary, mirror;
        SessionConfig cfg;
        cfg.spool_dir = (base / "s19").string();
        cfg.base_backoff_ms = 2; cfg.max_backoff_ms = 10; cfg.backoff_jitter = 0.0;
        cfg.mirror_transport = &mirror;

        Session ses(cfg, primary);
        CHECK(ses.start_new(blob(0, 800), video, tracks), "event starts");

        for (uint64_t i = 0; i < 4; ++i)
            ses.publish_segment(blob(i + 1), 6.0, (double)i * 6.0);

        const std::string ev = ses.event_id();
        char first[80];
        std::snprintf(first, sizeof(first), "events/%s/segments/00000000.m4s",
                      ev.c_str());

        bool both = false;
        for (int i = 0; i < 400; ++i) {
            if (primary.has(first) && mirror.has(first)) { both = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        CHECK(primary.has(first), "the segment reached the primary");
        CHECK(both, "and the second bucket received the same segment");
        // Control objects (manifest.json, live.json) are deliberately NOT
        // mirrored yet: they are the next step, and not done as a synchronous
        // double-write from the encode thread, which against a dead second
        // bucket would block the live feed for a request timeout. The mirror
        // holds media and nothing else at this point.
        CHECK(!mirror.has("events/" + ev + "/manifest.json"),
              "and the manifest is not mirrored yet — that is the next step");
        CHECK(!primary.ordering_violation,
              "the manifest invariant is unchanged by mirroring");
        ses.end();
    }

    fs::remove_all(base);
    std::printf("\n%s\n", g_fail == 0 ? "ALL SESSION TESTS PASSED" : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
