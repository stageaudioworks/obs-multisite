// SPDX-License-Identifier: GPL-3.0-or-later
// test_soak.cpp — the delivery claim, end to end, in compressed time
// (docs/scope/upload-delivery-assurance.md R7, the software half).
//
// One event of 1,500 segments — two and a half hours at 6 s — through:
//   • three uplink outages as long as 50, 300 and 900 segments (5, 30 and 90
//     minutes of programme), the link coming back between them;
//   • a store that acknowledges every PUT but afterwards loses some segments
//     and damages others (one per affected group, never two);
//   • a campus following along during the event, and one that starts after
//     it has ended.
//
// Pass: the receipt says complete, 1,500 of 1,500; both campuses hold every
// segment byte for byte, the store's losses rebuilt from parity.
//
// What this cannot show, and the hardware half of R7 must: real network
// behaviour, real stores (Content-MD5 acceptance among them), real disks, and
// OBS. Segments here are 2 KB, so this is about counts and order, not bitrate.
//
#include "../src/core/decoder_session.h"
#include "../src/core/receipt.h"
#include "../src/core/session.h"
#include "test_tmpdir.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using namespace multisite;

static int g_failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)

static constexpr uint64_t N = 1500;

static uint64_t seq_of(const std::string& key) {
    const size_t a = key.find("/segments/");
    return a == std::string::npos ? UINT64_MAX : std::stoull(key.substr(a + 10, 8));
}
// The store's own faults, after a 2xx: lost in groups 0, 4, 8…; damaged in
// groups 2, 6, 10… — one per group at most, which XOR parity can repair.
static bool store_loses(uint64_t s)   { return s % 40 == 5; }
static bool store_damages(uint64_t s) { return s % 40 == 25; }

class FaultyStore : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    std::mutex mtx;
    std::atomic<bool> uplink_down{false};
    std::atomic<uint64_t> refused{0};
    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string, std::string>&) override {
        if (uplink_down) { refused++; return {false, 0, true, "network down"}; }
        std::lock_guard<std::mutex> lk(mtx);
        const uint64_t s = seq_of(key);
        if (s != UINT64_MAX && store_loses(s)) return {true, 200, true, ""};  // said yes, kept nothing
        objects[key] = body;
        if (s != UINT64_MAX && store_damages(s)) objects[key][body.size() / 3] ^= 0x5A;
        return {true, 200, true, ""};
    }
    GetResult get(const std::string& key) override {
        std::lock_guard<std::mutex> lk(mtx);
        GetResult r;
        auto it = objects.find(key);
        if (it == objects.end()) { r.http_status = 404; r.retryable = false; return r; }
        r.success = true; r.http_status = 200; r.body = it->second;
        return r;
    }
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

static std::vector<uint8_t> seg(uint64_t s) {
    std::vector<uint8_t> v(1800 + (s * 53) % 500);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)((s * 2654435761u + i * 40503u) >> 7);
    return v;
}

static bool outage(uint64_t i) {
    // 50, 300 and 900 segments: 5, 30 and 90 minutes at 6 s.
    return (i >= 100 && i < 150) || (i >= 200 && i < 500) || (i >= 550 && i < 1450);
}

static size_t campus_has_all(DecoderSession& d, std::set<uint64_t>* wrong) {
    size_t ok = 0;
    for (uint64_t s = 0; s < N; ++s) {
        auto c = d.cache().load(s);
        if (c && *c == seg(s)) ++ok;
        else if (wrong) wrong->insert(s);
    }
    return ok;
}

static void pump(DecoderSession& d, int rounds) {
    for (int i = 0; i < rounds; ++i) { d.poll(); if (d.pump_downloads(200) == 0 && i > 2) break; }
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    fs::path base = unique_temp_dir("multisite_soak");
    fs::remove_all(base);

    FaultyStore store;
    SessionConfig cfg;
    cfg.room_id = "main";
    cfg.spool_dir = (base / "enc").string();
    cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 5; cfg.backoff_jitter = 0.0;
    cfg.disk_space = [](uint64_t& f, uint64_t& c) {
        f = 500ull << 30; c = 1000ull << 30; return true;   // a roomy encoder disk
    };
    VideoInfo video{ "h264", 1920, 1080, 30.0 };
    std::vector<AudioTrack> tracks = { { 0, "Main mix", "aac", 2, 48000 } };

    DecoderConfig dc;
    dc.room_id = "main";
    dc.buffer_minutes = 240;
    dc.max_cached_segments = 5000;
    dc.keep_behind_segments = 5000;
    dc.cache_dir = (base / "campus-live").string();
    DecoderSession live(dc, store);

    std::printf("== An event of %llu segments through three outages ==\n", (unsigned long long)N);
    std::string id;
    uint64_t peak_backlog = 0;
    {
        Session ses(cfg, store);
        CHECK(ses.start_new(seg(99999), video, tracks), "start_new");
        id = ses.event_id();
        for (uint64_t i = 0; i < N; ++i) {
            store.uplink_down = outage(i);
            ses.publish_segment(seg(i), 6.0, i * 6.0);
            peak_backlog = std::max<uint64_t>(peak_backlog, ses.status().pending);
            if (i % 50 == 0) pump(live, 1);      // the live campus keeps up as it can
            if (!outage(i) && i % 10 == 0)       // give the uplink time to drain
                for (int w = 0; w < 200 && ses.status().pending > 0; ++w)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        store.uplink_down = false;
        for (int w = 0; w < 3000 && ses.status().pending > 0; ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(ses.end(std::chrono::milliseconds(20000)), "End is marked in storage");
        std::printf("      peak backlog %llu segments; %llu PUTs refused by the outages\n",
                    (unsigned long long)peak_backlog, (unsigned long long)store.refused.load());
        CHECK(peak_backlog >= 800, "the 90-minute outage really did back up the spool");
    }

    {
        std::vector<uint8_t> b;
        { std::lock_guard<std::mutex> lk(store.mtx); b = store.objects.at(receipt_key(id)); }
        auto r = DeliveryReceipt::from_json(std::string(b.begin(), b.end()));
        std::printf("      receipt: %s\n", r.summary().c_str());
        CHECK(r.state() == "complete" && r.captured == N && r.in_cloud == N && r.lost == 0,
              "the receipt: 1500 of 1500 in the cloud, complete");
    }

    std::printf("== Both campuses end with every segment, intact ==\n");
    pump(live, 40);
    std::set<uint64_t> wrong_live;
    const size_t live_ok = campus_has_all(live, &wrong_live);
    std::printf("      live campus: %zu of %llu intact, %llu rebuilt\n", live_ok,
                (unsigned long long)N, (unsigned long long)live.stats().rebuilt);
    CHECK(live_ok == N, "the campus that followed along has all 1500, byte for byte");

    dc.cache_dir = (base / "campus-late").string();
    DecoderSession late(dc, store);
    pump(late, 40);
    std::set<uint64_t> wrong_late;
    const size_t late_ok = campus_has_all(late, &wrong_late);
    std::printf("      late campus: %zu of %llu intact, %llu rebuilt, %llu checked by group\n",
                late_ok, (unsigned long long)N, (unsigned long long)late.stats().rebuilt,
                (unsigned long long)late.stats().verified_by_group);
    CHECK(late_ok == N, "the campus that started after the end has all 1500, byte for byte");
    uint64_t faults = 0;
    for (uint64_t s = 0; s < N; ++s) if (store_loses(s) || store_damages(s)) ++faults;
    CHECK(late.stats().rebuilt == faults,
          "every segment the store lost or damaged was rebuilt, and nothing else was");
    if (!wrong_late.empty())
        std::printf("      first wrong at the late campus: %llu\n",
                    (unsigned long long)*wrong_late.begin());

    fs::remove_all(base);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%s (%d failure(s)) in %.1f s\n", g_failures ? "FAILED" : "PASSED", g_failures, secs);
    return g_failures ? 1 : 0;
}
