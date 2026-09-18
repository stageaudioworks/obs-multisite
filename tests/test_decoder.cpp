// SPDX-License-Identifier: GPL-3.0-or-later
// test_decoder.cpp — proves the satellite receive path and timeslipping.
//
// A fake object store stands in for R2, driven by a simulated encoder that
// publishes segments over time. The tests then check the behaviours a campus
// actually depends on: prebuffer, pause-while-cache-fills, resume from the
// exact position, jump-to-live, scrub, checksum rejection, stale detection,
// and never silently skipping a missing segment.
#include "../src/core/decoder_session.h"
#include "../src/core/checksum.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// ── A fake bucket that a simulated encoder writes into ───────────────────────
class FakeStore : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    std::mutex mtx;
    int fail_next_gets = 0;
    bool corrupt_segments = false;

    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string,std::string>&) override {
        std::lock_guard<std::mutex> lk(mtx);
        objects[key] = body;
        return {true, 200, true, ""};
    }
    GetResult get(const std::string& key) override {
        std::lock_guard<std::mutex> lk(mtx);
        GetResult r;
        if (fail_next_gets > 0) {
            --fail_next_gets;
            r.error = "simulated network failure";
            r.http_status = 0;
            return r;
        }
        auto it = objects.find(key);
        if (it == objects.end()) {
            r.http_status = 404; r.error = "NoSuchKey"; r.retryable = false;
            return r;
        }
        r.success = true; r.http_status = 200; r.body = it->second;
        // Simulate in-flight corruption of media segments only.
        if (corrupt_segments && key.find("/segments/") != std::string::npos &&
            !r.body.empty()) {
            r.body[r.body.size() / 2] ^= 0xFF;
        }
        return r;
    }
    // Enumerates the fake store. The cue merge lists the per-author prefix; a
    // bare prefix match is all this needs to stand in for ListObjectsV2.
    ListResult list(const std::string& prefix, const std::string&,
                    const std::string&, int) override {
        std::lock_guard<std::mutex> lk(mtx);
        ListResult r;
        for (const auto& kv : objects)
            if (kv.first.compare(0, prefix.size(), prefix) == 0)
                r.keys.push_back(ListEntry{ kv.first, (int64_t)kv.second.size(), "" });
        r.success = true; r.http_status = 200;
        return r;
    }
};


// Two stores behind one Transport, with a preference the caller can flip — the
// shape the decoder sees once an encoder has failed over: one end keeps
// answering 200 with a manifest that has stopped moving.
class SwitchingStore : public Transport {
public:
    SwitchingStore(FakeStore& primary, FakeStore& second)
        : m_primary(primary), m_second(second) {}

    void prefer_secondary(bool on) override {
        if (on && !m_prefer_secondary) ++switch_count;
        m_prefer_secondary = on;
    }
    bool preferring_secondary() const override { return m_prefer_secondary; }

    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string& ct,
                  const std::map<std::string,std::string>& tags) override {
        return m_primary.put(key, body, ct, tags);
    }
    GetResult get(const std::string& key) override {
        if (m_prefer_secondary) {
            GetResult r = m_second.get(key);
            if (r.success) return r;
        }
        GetResult r = m_primary.get(key);
        if (r.success || !m_prefer_secondary) return r;
        return m_second.get(key);
    }
    ListResult list(const std::string& prefix, const std::string& d,
                    const std::string& tok, int max) override {
        return m_prefer_secondary ? m_second.list(prefix, d, tok, max)
                                  : m_primary.list(prefix, d, tok, max);
    }

    int switch_count = 0;

private:
    FakeStore& m_primary;
    FakeStore& m_second;
    bool m_prefer_secondary = false;
};
// Publishes an event the way the encoder does, so the decoder sees realistic
// objects (live.json, event.json, init.mp4, manifest.json, segments).
struct FakeEncoder {
    FakeStore& store;
    std::string room, event;
    Manifest manifest;
    uint64_t next_seq = 0;
    double seg_dur = 6.0;
    size_t window = 50;
    int64_t clock_ms = 1000000;

    FakeEncoder(FakeStore& s, std::string r, std::string e)
        : store(s), room(std::move(r)), event(std::move(e)) {
        manifest.event_id = event;
        manifest.status = "live";
        manifest.init = "init.mp4";
        manifest.video = { "h264", 1280, 720, 30.0 };
        manifest.audio_tracks = { { 0, "Main mix", "aac", 2, 48000 } };
        manifest.first_available_seq = 0;
        manifest.started_at_ms = started_at_ms;
    }
    int64_t started_at_ms = 1700000000000LL;   // a fixed, realistic epoch

    static std::vector<uint8_t> body_for(uint64_t seq, size_t sz = 4096) {
        std::vector<uint8_t> v(sz);
        for (size_t i = 0; i < sz; ++i) v[i] = (uint8_t)((seq * 97 + i) & 0xFF);
        return v;
    }

    void publish_start() {
        std::vector<uint8_t> init(1200, 0x11);
        store.put("events/" + event + "/init.mp4", init, "", {});
        publish_manifest();
        publish_live("live");
    }
    void publish_segment() {
        uint64_t s = next_seq++;
        auto body = body_for(s);
        char name[16]; std::snprintf(name, sizeof(name), "%08llu",
                                     (unsigned long long)s);
        store.put("events/" + event + "/segments/" + name + ".m4s", body, "", {});
        ManifestSegment ms;
        ms.seq = s; ms.duration_s = seg_dur; ms.checksum = sha256_hex(body);
        ms.at_ms = started_at_ms + (int64_t)(s * seg_dur * 1000.0);
        manifest.push(ms, window);
        clock_ms += (int64_t)(seg_dur * 1000);
        publish_manifest();
    }
    void publish_manifest() {
        manifest.updated_at_ms = clock_ms;
        auto j = manifest.to_json();
        store.put("events/" + event + "/manifest.json",
                  std::vector<uint8_t>(j.begin(), j.end()), "", {});
    }
    void publish_live(const std::string& status) {
        LivePointer lp;
        lp.room_id = room; lp.event_id = event; lp.status = status;
        lp.updated_at_ms = clock_ms;
        auto j = lp.to_json();
        store.put("rooms/" + room + "/live.json",
                  std::vector<uint8_t>(j.begin(), j.end()), "", {});
    }
    // Simulates the encoder's spool cap evicting old segments for disk space:
    // declares everything below `seq` permanently gone.
    void drop_floor_to(uint64_t seq) {
        if (seq > manifest.first_available_seq) manifest.first_available_seq = seq;
        publish_manifest();
    }

    MarkerList markers;
    void drop_marker(const std::string& label) {
        Marker mk;
        mk.seq = next_seq;                 // applies at the current live edge
        mk.at_ms = clock_ms;
        mk.type = "cue";
        mk.label = label;
        mk.id = label + "-id";
        markers.markers.push_back(mk);
        auto j = markers.to_json();
        store.put("events/" + event + "/markers.json",
                  std::vector<uint8_t>(j.begin(), j.end()), "", {});
    }
    void end() {
        manifest.status = "ended";
        publish_manifest();
        publish_live("ended");
    }
};

// A store whose in-flight requests can be aborted BY US — what stopping a
// source does, and what must never be reported as the store failing.
class CancellableStore : public FakeStore {
public:
    std::atomic<bool> cancelled{false};
    void cancel_pending() override { cancelled = true; }
    void resume_pending() override { cancelled = false; }
    bool last_request_cancelled() const override { return cancelled.load(); }
    GetResult get(const std::string& key) override {
        if (cancelled.load()) {
            GetResult r;
            r.http_status = 0;
            r.error = "Operation was aborted by an application callback";
            return r;
        }
        return FakeStore::get(key);
    }
};

int main() {
    fs::path base = fs::temp_directory_path() / "multisite_decoder_test";
    fs::remove_all(base);
    fs::create_directories(base);

    std::printf("== 1. Discovers a live room and its event ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "main-auditorium", "01EVENTAAAAAAAAAAAAAAAAAAA");
        enc.publish_start();
        for (int i = 0; i < 6; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "main-auditorium";
        cfg.cache_dir = (base / "d1").string();
        DecoderSession dec(cfg, store);

        RoomState st = dec.poll(enc.clock_ms);
        CHECK(st == RoomState::Live, "room reports Live");
        CHECK(dec.event_id() == enc.event, "picked up the event id");
        CHECK(dec.live_edge() == 5, "live edge tracks the newest segment");
    }

    std::printf("== 2. Downloads ahead and starts after prebuffer ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTBBBBBBBBBBBBBBBBBBB");
        enc.publish_start();
        for (int i = 0; i < 10; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d2").string();
        cfg.prebuffer_segments = 2;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        int got = dec.pump_downloads(10);
        CHECK(got > 0, "downloaded segments into the cache");
        CHECK(dec.cache().has_init(), "init segment cached");
        CHECK(dec.start(), "playback starts once prebuffered");
        CHECK(dec.play_state() == PlayState::Playing, "state is Playing");

        auto seg = dec.next_segment();
        CHECK(seg.has_value(), "first segment served");
        CHECK(seg && !seg->init.empty(),
              "init travels with the first segment (decoder needs it first)");
        auto seg2 = dec.next_segment();
        CHECK(seg2 && seg2->init.empty(), "init not repeated on later segments");
        CHECK(seg2 && seg2->seq == seg->seq + 1, "segments served in order");
    }

    std::printf("== 3. TIMESLIPPING: pause holds position while cache fills ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTCCCCCCCCCCCCCCCCCCC");
        enc.publish_start();
        for (int i = 0; i < 8; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d3").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 3;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 5; ++i) dec.pump_downloads(10);
        dec.start();
        dec.next_segment();                       // serve one

        uint64_t head_at_pause = dec.playback_head();
        dec.pause();
        CHECK(dec.play_state() == PlayState::Paused, "paused");

        // The encoder keeps going while the campus is held.
        for (int i = 0; i < 6; ++i) enc.publish_segment();
        dec.poll(enc.clock_ms);
        int fetched = 0;
        for (int i = 0; i < 10; ++i) fetched += dec.pump_downloads(10);

        CHECK(fetched > 0, "cache KEPT FILLING while paused");
        CHECK(dec.playback_head() == head_at_pause,
              "playback head did not move while paused");
        CHECK(dec.next_segment().has_value() == false,
              "no segments served while paused");

        double behind = dec.behind_live_s();
        CHECK(behind > 30.0, "now well behind live (as expected after a hold)");

        dec.resume();
        auto after = dec.next_segment();
        CHECK(after && after->seq == head_at_pause,
              "resumed from EXACTLY where it paused (nothing skipped)");
        std::printf("     (behind live after hold: %.0fs, buffered ahead: %.0fs)\n",
                    behind, dec.buffered_ahead_s());
    }

    std::printf("== 4. Jump to live and scrub back ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTDDDDDDDDDDDDDDDDDDD");
        enc.publish_start();
        for (int i = 0; i < 20; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d4").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 4;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 8; ++i) dec.pump_downloads(10);
        dec.start();

        CHECK(dec.seek(3), "scrub back to segment 3");
        CHECK(dec.playback_head() == 3, "head moved to 3");
        double behind_after_seek = dec.behind_live_s();
        CHECK(behind_after_seek > 60.0, "reports being far behind live");

        dec.jump_to_live();
        CHECK(dec.playback_head() >= dec.live_edge() - 3,
              "jump_to_live snaps the head to the live edge");
        CHECK(dec.behind_live_s() < behind_after_seek,
              "behind-live figure shrinks after jumping");

        CHECK(!dec.seek(9999), "cannot seek beyond the live edge");

        // A jump must be signalled so the host can restart its decoder: the
        // next fragment has an unrelated baseMediaDecodeTime.
        uint64_t d0 = dec.discontinuity_id();
        dec.seek(5);
        CHECK(dec.discontinuity_id() > d0, "seek raises a discontinuity");
        uint64_t d1 = dec.discontinuity_id();
        dec.jump_to_live();
        CHECK(dec.discontinuity_id() > d1, "jump_to_live raises a discontinuity");
        // ...and the init segment must be re-sent after one.
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);
        auto after_jump = dec.next_segment();
        CHECK(after_jump && !after_jump->init.empty(),
              "init is re-sent after a discontinuity (decoder restarts)");
        CHECK(!dec.seek(0) || dec.playback_head() == 0,
              "seek within the retained window is allowed");
    }

    std::printf("== 5. Corrupt segments are rejected, not played ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTEEEEEEEEEEEEEEEEEEE");
        enc.publish_start();
        for (int i = 0; i < 5; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d5").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        store.corrupt_segments = true;             // flip a byte in flight
        dec.pump_downloads(10);
        CHECK(dec.stats().checksum_failures > 0,
              "checksum mismatch detected on download");
        CHECK(dec.cache().count() == 0,
              "corrupt segment was NOT cached (can't reach the decoder)");

        store.corrupt_segments = false;            // link recovers
        int got = dec.pump_downloads(10);
        CHECK(got > 0, "re-fetch after corruption succeeds");
        CHECK(dec.cache().count() > 0, "good segments cached on retry");
    }

    std::printf("== 6. A dead encoder is INTERRUPTED, and still watchable ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTFFFFFFFFFFFFFFFFFFF");
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d6").string();
        cfg.stale_after_ms = 600000;               // 10 min
        DecoderSession dec(cfg, store);

        CHECK(dec.poll(enc.clock_ms) == RoomState::Live, "live while updating");
        // Wall clock moves on but the encoder publishes nothing more.
        RoomState later = dec.poll(enc.clock_ms + 700000);
        CHECK(later == RoomState::Interrupted,
              "stale manifest -> Interrupted (the encoder died without ending)");
        CHECK(dec.was_interrupted(),
              "and it is distinguishable from a clean end");

        // The point of the state. Everything up to the moment the encoder went
        // is recorded and complete; reporting this as Offline (as it once did)
        // made a crashed event permanently unplayable, which is exactly when
        // you would want to watch it back.
        CHECK(dec.event_ended(), "an interrupted event behaves as video-on-demand");
        dec.pump_downloads(10);
        CHECK(dec.start(), "and playback can actually start");
        CHECK(dec.playback_head() == 0,
              "from the beginning, like any recording");
        std::printf("     (%s)\n", dec.last_error().c_str());
    }

    std::printf("== 6b. Pinning a past event ==\n");
    {
        FakeStore store;
        // An old event, finished cleanly.
        FakeEncoder past(store, "r", "01EVENTOLDAAAAAAAAAAAAAAAA");
        past.publish_start();
        for (int i = 0; i < 4; ++i) past.publish_segment();
        past.end();

        // …and a new one now on air in the same room.
        FakeEncoder now(store, "r", "01EVENTNEWBBBBBBBBBBBBBBBB");
        now.publish_start();
        for (int i = 0; i < 3; ++i) now.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d6b").string();
        DecoderSession dec(cfg, store);

        // Unpinned, it follows the room.
        CHECK(dec.poll(now.clock_ms) == RoomState::Live, "unpinned: follows live.json");
        CHECK(dec.event_id() == "01EVENTNEWBBBBBBBBBBBBBBBB", "playing the live event");
        CHECK(!dec.live_elsewhere(), "nothing is live elsewhere when playing live");

        // Pin the old event.
        const uint64_t disc_before = dec.discontinuity_id();
        dec.pin_event("01EVENTOLDAAAAAAAAAAAAAAAA");
        RoomState st = dec.poll(now.clock_ms);
        CHECK(dec.event_id() == "01EVENTOLDAAAAAAAAAAAAAAAA", "pinned event is played");
        CHECK(st == RoomState::Ended, "the pinned recording reports as a recording");
        CHECK(dec.discontinuity_id() != disc_before,
              "the host is told to restart its decoder: new event, new timeline");

        // The whole point of the second design question: an event starting
        // must not yank the operator out of what they are watching.
        for (int i = 0; i < 3; ++i) now.publish_segment();
        dec.poll(now.clock_ms);
        CHECK(dec.event_id() == "01EVENTOLDAAAAAAAAAAAAAAAA",
              "a new event going live does NOT steal a pinned playback");
        CHECK(dec.live_elsewhere(),
              "but the operator is told something is live now");
        CHECK(dec.live_event_id() == "01EVENTNEWBBBBBBBBBBBBBBBB",
              "and which event that is, so a jump can be offered");

        // Unpinning returns to the room.
        dec.unpin();
        dec.poll(now.clock_ms);
        CHECK(dec.event_id() == "01EVENTNEWBBBBBBBBBBBBBBBB", "unpin follows the room again");
        CHECK(!dec.live_elsewhere(), "and is no longer live-elsewhere");
    }

    std::printf("== 7. Clean end is reported as Ended ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTGGGGGGGGGGGGGGGGGGG");
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();
        enc.end();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d7").string();
        DecoderSession dec(cfg, store);
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended, "reports Ended");
    }

    std::printf("== 8. A missing segment mid-stream stalls rather than skipping ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTHHHHHHHHHHHHHHHHHHH");
        enc.publish_start();
        for (int i = 0; i < 6; ++i) enc.publish_segment();   // seqs 0..5

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d8").string();
        // Large prebuffer so the head starts at 0 and the gap is genuinely
        // mid-stream (a hole at the live edge is just "not published yet").
        cfg.prebuffer_segments = 6;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        CHECK(dec.start(), "playback started at the start of the window");
        CHECK(dec.playback_head() == 0, "head starts at segment 0");

        auto first = dec.next_segment();
        CHECK(first && first->seq == 0, "served segment 0");
        uint64_t head = dec.playback_head();
        CHECK(head == 1, "head advanced to 1");
        CHECK(head < dec.live_edge(), "head is behind the live edge (real gap)");

        char name[16];
        std::snprintf(name, sizeof(name), "%08llu", (unsigned long long)head);
        fs::remove(fs::path(cfg.cache_dir) / dec.event_id() /
                   (std::string(name) + ".m4s"));

        auto none = dec.next_segment();
        CHECK(!none.has_value(), "waits for the missing segment");
        CHECK(dec.playback_head() == head,
              "head does NOT advance past a gap (no silent skip)");
        CHECK(dec.stats().gaps_waited > 0, "gap recorded");
    }

    std::printf("== 8b. A segment the ENCODER declared gone is skipped, not stalled on ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTIIIIIIIIIIIIIIIIIII");
        enc.publish_start();
        for (int i = 0; i < 6; ++i) enc.publish_segment();   // seqs 0..5

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d8b").string();
        cfg.prebuffer_segments = 6;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        CHECK(dec.start(), "playback started");

        auto first = dec.next_segment();
        CHECK(first && first->seq == 0, "served segment 0");
        uint64_t head = dec.playback_head();
        CHECK(head == 1, "head at 1");

        // The encoder's disk-space eviction already happened: segment 1 is
        // gone from the cache AND the manifest now says nothing below 3 is
        // retained any more.
        char name[16];
        std::snprintf(name, sizeof(name), "%08llu", (unsigned long long)head);
        fs::remove(fs::path(cfg.cache_dir) / dec.event_id() /
                   (std::string(name) + ".m4s"));
        enc.drop_floor_to(3);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);

        uint64_t disc_before = dec.discontinuity_id();
        auto skipped = dec.next_segment();
        CHECK(skipped.has_value(),
              "does not stall forever on a segment the encoder declared gone");
        CHECK(skipped && skipped->seq == 3, "head jumped forward to the new floor");
        CHECK(dec.discontinuity_id() > disc_before,
              "the jump raises a discontinuity, same as a seek");
        CHECK(dec.stats().gap_skips > 0, "the skip is counted separately from an ordinary wait");
    }

    std::printf("== 9. Markers are read and can be jumped to ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTMMMMMMMMMMMMMMMMMMM");
        enc.publish_start();
        for (int i = 0; i < 4; ++i) enc.publish_segment();
        enc.drop_marker("Sermon Start");          // at seq 4
        for (int i = 0; i < 6; ++i) enc.publish_segment();
        enc.drop_marker("Offering");              // at seq 10
        for (int i = 0; i < 3; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d10").string();
        cfg.prebuffer_segments = 0; cfg.buffer_minutes = 4;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        auto mks = dec.markers();
        CHECK(mks.size() == 2, "both markers read from markers.json");
        CHECK(mks.size() == 2 && mks[0].label == "Sermon Start",
              "marker labels preserved");
        CHECK(mks.size() == 2 && mks[0].seq == 4,
              "marker carries the sequence it was dropped at");

        for (int i = 0; i < 8; ++i) dec.pump_downloads(10);
        dec.start();
        CHECK(dec.jump_to_marker("Sermon Start-id"), "jumped to a marker");
        CHECK(dec.playback_head() == 4, "head moved to the marker's segment");

        auto cur = dec.current_marker();
        CHECK(cur && cur->label == "Sermon Start",
              "current_marker reports where we are in the event");

        CHECK(dec.jump_to_marker("Offering-id"), "jumped to the later marker");
        CHECK(dec.playback_head() == 10, "head moved to segment 10");
        cur = dec.current_marker();
        CHECK(cur && cur->label == "Offering", "current marker updated");

        CHECK(!dec.jump_to_marker("does-not-exist"),
              "unknown marker id is rejected");

        // Markers must not survive an event change.
        FakeEncoder enc2(store, "r", "01EVENTNNNNNNNNNNNNNNNNNNN");
        enc2.publish_start();
        for (int i = 0; i < 3; ++i) enc2.publish_segment();
        dec.poll(enc2.clock_ms);
        CHECK(dec.markers().empty(), "markers cleared when the event changes");
    }

    std::printf("== 10. Positions map to wall-clock time ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTWWWWWWWWWWWWWWWWWWW");
        enc.publish_start();
        for (int i = 0; i < 10; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d11").string();
        cfg.prebuffer_segments = 0; cfg.buffer_minutes = 4;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);
        dec.start();

        CHECK(dec.event_started_ms() == enc.started_at_ms,
              "event start time reaches the satellite");
        // Segment 4 holds content 24s after the event started (4 x 6s).
        const int64_t expect4 = enc.started_at_ms + 24000;
        CHECK(dec.wall_clock_ms(4) == expect4,
              "a position converts to the clock time of its content");
        CHECK(dec.seek(4), "seek to that position");
        CHECK(dec.playhead_wall_ms() == expect4,
              "playhead reports the clock time being shown");
        CHECK(dec.live_wall_ms() > dec.playhead_wall_ms(),
              "live edge is later than the playhead when behind");
        std::printf("     (showing %lld ms into the epoch, live at %lld)\n",
                    (long long)dec.playhead_wall_ms(),
                    (long long)dec.live_wall_ms());

        // Outside the rolling window it must still estimate rather than give up.
        Manifest m; m.started_at_ms = enc.started_at_ms;
        CHECK(dec.wall_clock_ms(500) > enc.started_at_ms,
              "positions outside the manifest window are still estimated");
    }

    std::printf("== 11. Audio names published by the main site reach the satellite ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTAUDIOAUDIOAUDIOAUD");
        // A packed multi-channel feed, as the encoder publishes it.
        enc.manifest.audio_tracks = { {
            0, "Production", "aac", 8, 48000,
            { "Main L", "Main R", "Sermon ISO", "Click",
              "Choir ISO", "Ambient L", "Ambient R", "Spare" }
        } };
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d12").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        auto layout = dec.audio_layout();
        CHECK(layout.size() == 1, "audio layout received");
        CHECK(!layout.empty() && layout[0].channels == 8, "8 channels reported");
        CHECK(!layout.empty() && layout[0].channel_labels.size() == 8,
              "all channel names received");
        CHECK(!layout.empty() && layout[0].channel_labels[3] == "Click",
              "channel names keep their order (channel 4 is the click)");
        std::printf("     (channel 3 = %s, channel 4 = %s)\n",
                    layout[0].channel_labels[2].c_str(),
                    layout[0].channel_labels[3].c_str());
    }

    std::printf("== 12. Pause and resume survive any prior state ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTPAUSEPAUSEPAUSEPAU");
        enc.publish_start();
        for (int i = 0; i < 8 ; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d13").string();
        // A realistic prebuffer: playback sits behind live, which is the
        // normal case and the one where resume must continue immediately.
        cfg.prebuffer_segments = 3; cfg.buffer_minutes = 4;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);

        // Pausing before playback has begun must still register as "held".
        dec.pause();
        CHECK(dec.play_state() == PlayState::Paused,
              "pause before playback still holds");

        CHECK(dec.start(), "start after a pre-emptive pause");
        dec.resume();
        CHECK(dec.play_state() == PlayState::Playing, "resume starts playback");

        auto a = dec.next_segment();
        CHECK(a.has_value(), "serves a segment after resume");

        // Now the real sequence: play, hold, resume, and it must serve again.
        dec.pause();
        CHECK(dec.play_state() == PlayState::Paused, "held while playing");
        CHECK(!dec.next_segment().has_value(), "nothing served while held");
        const uint64_t held_at = dec.playback_head();

        dec.resume();
        CHECK(dec.play_state() == PlayState::Playing, "resumed");
        auto b = dec.next_segment();
        CHECK(b.has_value(), "SERVES AGAIN AFTER RESUME");
        CHECK(b && b->seq == held_at,
              "continues from exactly where it was held");

        // Resume when already playing must be harmless, not disruptive.
        dec.resume();
        CHECK(dec.play_state() == PlayState::Playing,
              "resume while already playing is harmless");
        CHECK(dec.next_segment().has_value(), "still serving");
    }

    std::printf("== 13. Resuming at the live edge waits for new content ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTEDGEEDGEEDGEEDGEED");
        enc.publish_start();
        for (int i = 0; i < 4; ++i) enc.publish_segment();      // 0..3

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d14").string();
        cfg.prebuffer_segments = 0;      // deliberately AT the live edge
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);
        dec.start();
        while (dec.next_segment().has_value()) {}               // catch right up

        CHECK(dec.playback_head() > dec.live_edge(),
              "playhead has caught up past the newest segment");
        dec.pause();
        dec.resume();
        CHECK(!dec.next_segment().has_value(),
              "resuming with nothing new serves nothing — correct, but the UI "
              "must explain it rather than look broken");

        // As soon as the main site publishes more, playback continues.
        enc.publish_segment();
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        CHECK(dec.next_segment().has_value(),
              "continues the moment new content arrives");
    }

    std::printf("== 14. Buffer target is honoured in minutes, and fetched fast ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTBUFFERBUFFERBUFFER");
        enc.publish_start();
        // 25 minutes of programme at 6s segments.
        for (int i = 0; i < 250; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d15").string();
        cfg.prebuffer_segments = 2;
        cfg.buffer_minutes = 10;            // 100 segments at 6s
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        // Jump back 15 minutes, the case that previously refused to buffer.
        const int64_t fifteen_back = dec.live_wall_ms() - 15 * 60 * 1000;
        CHECK(dec.seek_to_wall_ms(fifteen_back) != 0,
              "seek back 15 minutes by clock time");

        // Draining the queue must bank the whole buffer target, not 60s of it.
        for (int i = 0; i < 400; ++i) dec.pump_downloads(64);
        const double ahead = dec.buffered_ahead_s();
        std::printf("     (buffered %.0fs ahead after seeking back)\n", ahead);
        CHECK(ahead > 300.0,
              "buffers minutes ahead when behind live (not just 60s)");
        CHECK(dec.stats().downloaded > 50,
              "fetched aggressively rather than at playback speed");

        auto ranges = dec.cached_ranges();
        CHECK(!ranges.empty(), "cached ranges reported for the timeline");
        std::printf("     (%zu contiguous cached range(s), first %llu..%llu)\n",
                    ranges.size(),
                    (unsigned long long)ranges.front().first,
                    (unsigned long long)ranges.front().second);
    }

    std::printf("== 15. Seeking by clock time lands mid-segment ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTSEEKSEEKSEEKSEEKSE");
        enc.publish_start();
        for (int i = 0; i < 20; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d16").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 5;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 20; ++i) dec.pump_downloads(32);
        dec.start();

        // 40 seconds in: segment 6 (36s) plus 4 seconds.
        const int64_t target = enc.started_at_ms + 40000;
        CHECK(dec.seek_to_wall_ms(target) == target, "seek to 40s in");
        // Seeking outside what is cached means fetching again — the same
        // behaviour any DVR has, and worth asserting rather than assuming.
        for (int i = 0; i < 20; ++i) dec.pump_downloads(32);
        auto seg = dec.next_segment();
        CHECK(seg.has_value(), "segment served after a timed seek");
        CHECK(seg && seg->seq == 6, "landed on the segment containing 40s");
        CHECK(seg && seg->skip_to_ms == 4000,
              "reports 4s into the segment, so seeking is not limited to "
              "6-second boundaries");
        CHECK(seg && seg->starts_at_ms == enc.started_at_ms + 36000,
              "segment start time reported for the playing clock");
        // The offset applies to that segment only.
        auto seg2 = dec.next_segment();
        CHECK(seg2 && seg2->skip_to_ms == 0, "offset does not leak to the next");

        // A seek that cannot be honoured must say WHICH end it hit. All of
        // these used to be reported to the operator as "that moment is no
        // longer available in storage", which for a seek past the end is not
        // vague but wrong — nothing has been removed, there is simply no more
        // of the event yet.
        {
            const int64_t past_end = enc.started_at_ms + 60LL * 60 * 1000;
            CHECK(dec.seek_to_wall_ms(past_end) == 0, "a seek past the end fails");
            const std::string why = dec.last_error();
            CHECK(why.find("storage") == std::string::npos,
                  "and is not blamed on storage having lost it");
            CHECK(why.find("ahead of what has been broadcast") != std::string::npos ||
                  why.find("past the end") != std::string::npos,
                  "it says the moment is beyond what exists");

            CHECK(dec.seek_to_wall_ms(enc.started_at_ms - 60000) == 0,
                  "a seek before the start fails");
            CHECK(dec.last_error().find("before this event started") !=
                      std::string::npos,
                  "and says so, rather than blaming retention");
        }
    }

    std::printf("== 16. A finished recording plays as video-on-demand ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTVODVODVODVODVODVOD");
        enc.publish_start();
        for (int i = 0; i < 20; ++i) enc.publish_segment();
        enc.end();                                  // operator ends the broadcast

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d17").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 5;
        DecoderSession dec(cfg, store);
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended, "reports a finished event");
        CHECK(dec.event_ended(), "event_ended() is true");

        for (int i = 0; i < 10; ++i) dec.pump_downloads(32);
        CHECK(dec.start(), "playback starts");
        CHECK(dec.playback_head() == 0,
              "STARTS AT THE BEGINNING, not near the end");

        // The end time must be the end of the last segment, not its start.
        const int64_t expect_end = enc.started_at_ms + 20 * 6000;
        CHECK(dec.end_wall_ms() == expect_end,
              "end time is the end of the recording");

        // Play right through.
        int served = 0;
        while (dec.next_segment().has_value() && served < 40) ++served;
        CHECK(served == 20, "played every segment through to the end");
        CHECK(dec.at_end(), "reports having reached the end");
        CHECK(dec.playhead_wall_ms() <= dec.end_wall_ms(),
              "the displayed time NEVER runs past the end of the recording");
        std::printf("     (played %d segments; ends at %lld, playhead %lld)\n",
                    served, (long long)dec.end_wall_ms(),
                    (long long)dec.playhead_wall_ms());
    }

    std::printf("== 17. Ending mid-playback plays through to the end ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTFINISHFINISHFINISH");
        enc.publish_start();
        for (int i = 0; i < 10; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d18").string();
        cfg.prebuffer_segments = 4; cfg.buffer_minutes = 5;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 10; ++i) dec.pump_downloads(32);
        dec.start();
        const uint64_t from = dec.playback_head();
        dec.next_segment();

        enc.end();                                  // End broadcast pressed
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended,
              "notices the broadcast ended");
        for (int i = 0; i < 10; ++i) dec.pump_downloads(32);

        int served = 1;
        while (dec.next_segment().has_value() && served < 40) ++served;
        CHECK(dec.playback_head() > dec.live_edge(),
              "kept playing to the last segment rather than stopping");
        CHECK(served == (int)(9 - from + 1),
              "served exactly the segments that remained");
        CHECK(dec.playhead_wall_ms() <= dec.end_wall_ms(),
              "time stays within the recording after it finishes");
    }

    std::printf("== 18. 'Ended' means two different things ==\n");
    {
        // (a) Loaded while already finished: this is a recording of a past
        //     event, not something that just ended.
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTWASNTLIVEWASNTLIVE");
        enc.publish_start();
        for (int i = 0; i < 5; ++i) enc.publish_segment();
        enc.end();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d19").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        CHECK(dec.event_ended(), "reports a finished event");
        CHECK(!dec.was_live_this_session(),
              "knows it was NOT live while we were watching");
    }
    {
        // (b) Live when loaded, then the operator ends the broadcast.
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTWASLIVEWASLIVEWASL");
        enc.publish_start();
        for (int i = 0; i < 5; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d20").string();
        DecoderSession dec(cfg, store);
        CHECK(dec.poll(enc.clock_ms) == RoomState::Live, "live when loaded");
        CHECK(dec.was_live_this_session(), "remembers seeing it live");

        enc.end();
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended, "notices it ended");
        CHECK(dec.was_live_this_session(),
              "still knows the broadcast ended while we watched");

        // Watching an event through to its own end holds it. The next event
        // going live in the room must NOT steal the playback: being pulled out
        // of a recording somebody is part-way through is worse than being told
        // about the new one — which is what live_elsewhere() is for.
        FakeEncoder enc2(store, "r", "01EVENTNEXTNEXTNEXTNEXTNE");
        enc2.publish_start();
        for (int i = 0; i < 3; ++i) enc2.publish_segment();
        dec.poll(enc2.clock_ms);
        CHECK(dec.event_id() == "01EVENTWASLIVEWASLIVEWASL",
              "a new event does not steal a recording that has just finished");
        CHECK(dec.is_pinned(),
              "the finished event is held exactly as if it had been pinned");
        CHECK(dec.live_elsewhere(), "and the operator is told something is live");

        // Back to live is what follows the room again, and it is only then
        // that the new event takes over — with a clean slate.
        enc2.end();
        dec.unpin();
        dec.poll(enc2.clock_ms);
        CHECK(dec.event_id() == "01EVENTNEXTNEXTNEXTNEXTNE",
              "Back to live follows the room to the new event");
        CHECK(!dec.was_live_this_session(),
              "a different event starts with a clean slate");
    }

    std::printf("== 19. A finished recording reports its total length ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTLENGTHLENGTHLENGTH");
        enc.publish_start();
        for (int i = 0; i < 100; ++i) enc.publish_segment();   // 10 minutes
        enc.end();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d21").string();
        cfg.buffer_minutes = 2;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        const int64_t total = dec.end_wall_ms() - dec.event_started_ms();
        CHECK(total == 100 * 6000,
              "total length is the whole recording, not the manifest window");
        std::printf("     (reports %lld min %lld s)\n",
                    (long long)(total / 60000), (long long)((total / 1000) % 60));

        // The bounds a timeline is drawn from must not move during playback:
        // an expanding bar makes positions meaningless.
        for (int i = 0; i < 20; ++i) dec.pump_downloads(64);
        dec.start();
        const int64_t span_before = dec.end_wall_ms() - dec.event_started_ms();
        for (int i = 0; i < 20; ++i) dec.next_segment();
        const int64_t span_after = dec.end_wall_ms() - dec.event_started_ms();
        CHECK(span_before == span_after,
              "the timeline span is FIXED once the recording has ended");

        // And the manifest only lists a rolling window, so the length must not
        // be derived from it.
        CHECK(dec.earliest_available() == 0,
              "the whole recording is addressable from the first segment");
    }

    std::printf("== 20. Head never runs past the live edge ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTIIIIIIIIIIIIIIIIIII");
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();   // seqs 0..2

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d9").string();
        cfg.prebuffer_segments = 0;
        cfg.start_buffer_seconds = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        dec.start();
        int served = 0;
        while (dec.next_segment().has_value() && served < 20) ++served;
        CHECK(dec.playback_head() <= dec.live_edge() + 1,
              "head stops at the live edge instead of running away");
        CHECK(!dec.next_segment().has_value(),
              "nothing served while waiting for the encoder to publish more");
        for (int i = 0; i < 2; ++i) enc.publish_segment();
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        CHECK(dec.next_segment().has_value(),
              "resumes as soon as new segments appear");
    }

    std::printf("== 21. Link health follows the connection, not the room ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTJJJJJJJJJJJJJJJJJJJ");
        enc.publish_start();
        for (int i = 0; i < 4; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d20").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        CHECK(dec.link_health() == LinkHealth::Healthy,
              "healthy after the first successful poll");

        // A room with nothing live yet answers live.json with a clean 404.
        // That proves the store is reachable, so the link stays healthy even
        // though the room is offline — the two must never be conflated.
        {
            FakeStore empty;
            DecoderConfig c2;
            c2.room_id = "nothing"; c2.cache_dir = (base / "d21").string();
            DecoderSession d2(c2, empty);
            RoomState st = d2.poll(1000000);
            CHECK(st == RoomState::Offline, "empty room reports offline");
            CHECK(d2.link_health() == LinkHealth::Healthy,
                  "a 404 is the store answering, not an outage");
        }

        // Kill the link: live.json stops completing. The session goes offline,
        // but that is now distinguishable from the empty-room case above.
        store.fail_next_gets = 1;
        dec.poll(enc.clock_ms);
        CHECK(dec.link_health() == LinkHealth::Degraded,
              "one failed request degrades, not yet offline");

        store.fail_next_gets = 2;
        dec.poll(enc.clock_ms);
        CHECK(dec.link_health() == LinkHealth::Offline,
              "two consecutive failures are offline");

        store.fail_next_gets = 0;
        dec.poll(enc.clock_ms);
        CHECK(dec.link_health() == LinkHealth::Healthy,
              "recovery happens as soon as the store answers again");
    }

    std::printf("== 22. Playback waits for the start buffer ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTSTARTBUFFERSTARTBU");
        enc.publish_start();
        for (int i = 0; i < 15; ++i) enc.publish_segment();   // 90 s of programme

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d22").string();
        cfg.prebuffer_segments = 0;
        cfg.start_buffer_seconds = 60;          // 10 x 6 s segments
        cfg.buffer_minutes = 10;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        // A partial download must not be enough to go to air: the buffer has
        // to accumulate the full start window first, otherwise playback starts
        // near the live edge and stalls after a segment.
        dec.pump_downloads(4);
        CHECK(!dec.start(), "refuses to start with less than 60 s banked");
        CHECK(dec.play_state() == PlayState::Stopped,
              "playback stays stopped while the buffer accumulates");

        // Keep filling; once 60 s is contiguously cached it may begin.
        for (int i = 0; i < 20 && !dec.start(); ++i) dec.pump_downloads(4);
        CHECK(dec.play_state() == PlayState::Playing,
              "starts once 60 s is banked");
        CHECK(dec.behind_live_s() >= 60.0 - 1e-6,
              "begins about a minute behind live");
        CHECK(dec.buffered_ahead_s() >= 60.0 - 1e-6,
              "and has a minute buffered ahead of the playhead");
        std::printf("     (started %.0fs behind live, %.0fs buffered ahead)\n",
                    dec.behind_live_s(), dec.buffered_ahead_s());
    }

    std::printf("== 23. Cues: one object per author, merged for every site ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "main-auditorium", "01EVENTCUESCUESCUESCUESCCC");
        enc.publish_start();
        for (int i = 0; i < 8; ++i) enc.publish_segment();
        enc.drop_marker("Sermon Start");          // the encoder's own cue

        DecoderConfig cfg;
        cfg.room_id = "main-auditorium";
        cfg.cache_dir = (base / "cache_cues").string();
        cfg.author_name = "Campus B";
        cfg.can_author_cues = true;

        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(20);

        std::string err;
        CHECK(dec.add_cue("Our notice", err), "a satellite drops a cue of its own");
        const std::string own = "events/" + enc.event + "/cues/campus-b.json";
        CHECK(store.objects.count(own) == 1,
              "the cue is written to this site's OWN object");
        {
            const auto& raw = store.objects[own];
            auto cj = MarkerList::from_json(std::string(raw.begin(), raw.end()));
            CHECK(cj.markers.size() == 1 && cj.markers[0].author == "Campus B",
                  "and it carries the site name as its author");
        }
        CHECK(dec.markers().size() == 2,
              "the site sees its own cue at once, beside the encoder's");

        // A second site authors independently; the next poll merges all three.
        MarkerList other;
        other.markers.push_back(Marker{ 1, enc.clock_ms - 500, "cue", "Welcome",
                                        "campus-c-id", "Campus C" });
        const std::string oj = other.to_json();
        store.put("events/" + enc.event + "/cues/campus-c.json",
                  std::vector<uint8_t>(oj.begin(), oj.end()), "", {});
        enc.clock_ms += 6000;
        enc.publish_segment();
        dec.poll(enc.clock_ms);
        auto merged = dec.markers();
        CHECK(merged.size() == 3, "cues from three sites merge into one list");
        CHECK(merged.size() == 3 && merged[0].label == "Welcome" &&
              merged[0].author == "Campus C",
              "and are ordered by time, each author intact");
    }

    std::printf("== 24. A LAN satellite authors through the hub, not the bucket ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTHUBHUBHUBHUBHUBHUBH");
        enc.publish_start();
        for (int i = 0; i < 6; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r";
        cfg.cache_dir = (base / "cache_hub").string();
        cfg.author_name = "Campus D";
        cfg.can_author_cues = true;
        bool hub_used = false;
        cfg.cue_hub = [&](const std::string& author, const std::string& label,
                          std::string& merged, std::string& error) {
            hub_used = true;
            CHECK(author == "Campus D", "the hub is told which site is authoring");
            MarkerList ml;
            Marker mk; mk.seq = 1; mk.at_ms = enc.clock_ms; mk.type = "cue";
            mk.label = label; mk.id = "hub-id"; mk.author = author;
            ml.markers.push_back(mk);
            merged = ml.to_json();
            error.clear();
            return true;
        };

        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        std::string err;
        CHECK(dec.add_cue("Hub cue", err), "the cue is accepted through the hub");
        CHECK(hub_used, "and the hub was used, not a bucket write");
        CHECK(store.objects.count("events/" + enc.event + "/cues/campus-d.json") == 0,
              "no cue object was written by the satellite itself");
        CHECK(dec.markers().size() == 1 && dec.markers()[0].label == "Hub cue",
              "the hub's merged list is reflected locally at once");
    }

    std::printf("== 25. A cue on a RECORDING lands at the playhead, not the end ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTRECRECRECRECRECRECR");
        enc.publish_start();
        for (int i = 0; i < 30; ++i) enc.publish_segment();
        enc.end();                         // a finished recording

        DecoderConfig cfg;
        cfg.room_id = "r";
        cfg.cache_dir = (base / "cache_rec").string();
        cfg.author_name = "Campus B";
        cfg.can_author_cues = true;

        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(40);
        CHECK(dec.start(), "the recording starts playing");
        for (int i = 0; i < 3; ++i) { dec.next_segment(); dec.pump_downloads(4); }
        const uint64_t head = dec.playback_head();
        CHECK(head < dec.live_edge(),
              "the playhead is inside the recording, not at its end");

        std::string err;
        CHECK(dec.add_cue("Halfway", err), "a cue can be dropped on a recording");
        const std::string own = "events/" + enc.event + "/cues/campus-b.json";
        auto it = store.objects.find(own);
        CHECK(it != store.objects.end(), "the cue object was written");
        MarkerList cl;
        if (it != store.objects.end())
            cl = MarkerList::from_json(std::string(it->second.begin(), it->second.end()));
        CHECK(cl.markers.size() == 1 && cl.markers[0].seq == head,
              "and it is stamped at the playhead, not the live edge");
        CHECK(cl.markers.size() == 1 && cl.markers[0].seq < dec.live_edge(),
              "so it is nowhere near the end of the event");
        CHECK(cl.markers.size() == 1 &&
              cl.markers[0].at_ms >= enc.started_at_ms,
              "and carries the event's own time, not today's");

        // And when the host says which frame is on screen, the cue goes in the
        // segment that time falls IN, at exactly that time — not wherever the
        // session's head has run ahead to.
        const long long at = enc.started_at_ms + 5 * 6000 + 3000;   // 5.5 segments in
        std::string err2;
        CHECK(dec.add_cue("By time", err2, 0, at), "a cue can be placed by on-screen time");
        MarkerList cl2;
        auto it2 = store.objects.find(own);
        if (it2 != store.objects.end())
            cl2 = MarkerList::from_json(std::string(it2->second.begin(), it2->second.end()));
        bool placed = false;
        for (const auto& m : cl2.markers)
            if (m.label == "By time") placed = (m.seq == 5 && m.at_ms == at);
        CHECK(placed, "landing in the segment that time falls in, at exactly that time");

        // And the path a host actually uses: the on-screen segment, straight.
        // No clock involved, so a seek that re-pins the clock cannot move it.
        std::string err3;
        CHECK(dec.add_cue("By segment", err3, 17),
              "a cue can be placed by the on-screen segment");
        MarkerList cl3;
        auto it3 = store.objects.find(own);
        if (it3 != store.objects.end())
            cl3 = MarkerList::from_json(std::string(it3->second.begin(), it3->second.end()));
        bool placed3 = false;
        for (const auto& m : cl3.markers)
            if (m.label == "By segment") placed3 = (m.seq == 17);
        CHECK(placed3, "landing on exactly the segment the host named");
    }

    std::printf("== 22. A stalled primary, and a second bucket still being written\n");
    {
        // The write-side failover (PROJECT-SCOPE.md §10 Phase 9). The target
        // being read keeps answering 200 with a manifest that will never move
        // again, so no per-request fallback ever fires — only the session can
        // see this, and only by comparing how far each end has got.
        FakeStore primary, second;
        FakeEncoder old_enc(primary, "r", "01EVENTSTALLEDSTALLEDSTA");
        old_enc.publish_start();
        for (int i = 0; i < 5; ++i) old_enc.publish_segment();

        FakeEncoder new_enc(second, "r", "01EVENTMOVEDMOVEDMOVEDM");
        new_enc.publish_start();
        for (int i = 0; i < 3; ++i) new_enc.publish_segment();

        SwitchingStore sw(primary, second);

        DecoderConfig cfg;
        cfg.room_id = "r";
        cfg.cache_dir = (base / "d22").string();
        cfg.stale_after_ms = 60000;

        DecoderSession dec(cfg, sw);

        // Far enough past the primary's last write that it reads as stalled.
        const int64_t t1 = old_enc.clock_ms + cfg.stale_after_ms + 1;
        CHECK(dec.poll(t1) == RoomState::Interrupted,
              "the end being read has stopped advancing");
        CHECK(!sw.preferring_secondary(),
              "and nothing has moved yet — the decision is taken on the next poll");

        // The next poll acts on it: the other end is asked, and answers.
        CHECK(dec.poll(new_enc.clock_ms) == RoomState::Live,
              "the other end is still live, and is now the one being read");
        CHECK(sw.preferring_secondary(), "the session moved its reads across");
        CHECK(dec.event_id() == "01EVENTMOVEDMOVEDMOVEDM",
              "and it follows the event that is actually being written");

        // Exactly once: a genuinely stalled event must not flap between ends.
        dec.poll(new_enc.clock_ms);
        CHECK(sw.switch_count == 1, "the move happened once, not once per poll");
    }

    std::printf("== 25. Readiness is the session's answer, not the dock's ==\n");
    {
        // LIVE: the gate is the start-buffer window.
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTREADYREADYREADYRE");
        enc.publish_start();
        for (int i = 0; i < 15; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d24").string();
        cfg.prebuffer_segments = 0;
        cfg.start_buffer_seconds = 60;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        CHECK(!dec.can_start_now(), "not ready before the gate is banked");
        CHECK(dec.start_gate_s() >= 60.0 - 1e-6,
              "and the gate it reports is the start-buffer window");

        // The invariant the dock depends on: whenever it is told Ready, Play
        // really works. Two answers to one question is how the old dock came to
        // advertise Ready for something that would refuse to start.
        for (int i = 0; i < 40 && !dec.can_start_now(); ++i) dec.pump_downloads(4);
        CHECK(dec.can_start_now(), "ready once the whole window is contiguous");
        CHECK(dec.ready_buffer_s() >= dec.start_gate_s() - 1e-6,
              "with the buffered figure at least the gate");
        CHECK(dec.start(), "and Ready means Play works — the invariant");
    }
    {
        // A FINISHED RECORDING: one segment is the whole requirement, so it
        // must not be shown a 60-second countdown it does not have to wait for.
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTFINISHEDFINISHED");
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();
        enc.end();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d25").string();
        cfg.prebuffer_segments = 0;
        cfg.start_buffer_seconds = 60;      // the same setting as the live case
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        for (int i = 0; i < 10 && !dec.can_start_now(); ++i) dec.pump_downloads(4);
        CHECK(dec.can_start_now(),
              "a finished recording is ready once its first segment is");
        CHECK(dec.start_gate_s() <= 6.0 + 1e-6,
              "and its gate is one segment, not the 60 s the setting names");
        CHECK(dec.start(), "so Play works immediately");
    }

    std::printf("== 26. Our own cancellation is not a fault ==\n");
    {
        CancellableStore store;
        FakeEncoder enc(store, "r", "01EVENTCANCELCANCELCANC");
        enc.publish_start();
        for (int i = 0; i < 4; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d26").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        CHECK(dec.link_health() == LinkHealth::Healthy, "healthy after a normal poll");
        const RoomState before = dec.poll(enc.clock_ms);

        // Stop cancels what is in flight. That must read as no answer, not as
        // the store failing: the "errors" this produced were the operator's own
        // Stop button reported back to them as a connection loss.
        store.cancel_pending();
        dec.poll(enc.clock_ms);
        CHECK(dec.last_error().empty(),
              "a cancelled request records no error");
        CHECK(dec.link_health() != LinkHealth::Offline,
              "and does not degrade the link");
        CHECK(dec.poll(enc.clock_ms) == before,
              "nor does it blank the room that was being watched");

        // And the transport is usable again afterwards, as stop/play expects.
        store.resume_pending();
        dec.poll(enc.clock_ms);
        CHECK(dec.last_error().empty(), "a normal poll after resuming is clean");
    }

    std::printf("== 27. A pin is not ready until it is the thing being played ==\n");
    {
        FakeStore store;
        // Two events in one room, and the SECOND one published is the one
        // live.json names — so the long one has to go last for the session to be
        // following it when the pin happens.
        FakeEncoder b(store, "r", "01EVENTBBBBBBB");
        b.publish_start();
        for (int i = 0; i < 3; ++i) b.publish_segment();
        FakeEncoder a(store, "r", "01EVENTAAAAAAA");
        a.publish_start();
        for (int i = 0; i < 15; ++i) a.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d27").string();
        cfg.prebuffer_segments = 0; cfg.start_buffer_seconds = 60;
        DecoderSession dec(cfg, store);
        dec.poll(a.clock_ms);
        for (int i = 0; i < 40 && !dec.can_start_now(); ++i) dec.pump_downloads(4);
        CHECK(dec.can_start_now(), "ready on the event that is loaded");
        CHECK(dec.start(), "and it starts");

        // Pin a different event. poll() has not run yet, and the seat still
        // belongs to the first — so readiness must be withdrawn AT ONCE, or the
        // dock shows READY, leaves Play enabled, and the press plays the old
        // head against the new event. That is the jump, and it was up to a full
        // poll interval wide.
        dec.pin_event("01EVENTBBBBBBB");
        CHECK(!dec.can_start_now(),
              "pinning another event is not ready before it is applied");
    }

    fs::remove_all(base);
    std::printf("\n%s\n", g_fail == 0 ? "ALL DECODER TESTS PASSED"
                                      : "SOME DECODER TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
