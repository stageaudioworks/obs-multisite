// SPDX-License-Identifier: GPL-3.0-or-later
// Measures what a UI call costs while the decoder is downloading. This is the
// lockup: a timeline click had to wait for whatever transfer was in flight.
#include "decoder_session.h"
#include "checksum.h"
#include "test_tmpdir.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
namespace fs = std::filesystem;
using namespace multisite;

// A store where every GET takes a realistic amount of time.
class SlowStore : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    std::mutex mtx;
    int delay_ms = 120;                 // a plausible segment fetch
    PutResult put(const std::string& k, const std::vector<uint8_t>& b,
                  const std::string&, const std::map<std::string,std::string>&) override {
        std::lock_guard<std::mutex> lk(mtx); objects[k] = b;
        return {true, 200, true, ""};
    }
    GetResult get(const std::string& k) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        std::lock_guard<std::mutex> lk(mtx);
        GetResult r;
        auto it = objects.find(k);
        if (it == objects.end()) { r.http_status = 404; r.retryable = false; return r; }
        r.success = true; r.http_status = 200; r.body = it->second;
        return r;
    }
};

int main(){
    fs::path base = unique_temp_dir("ms_responsive");
    fs::remove_all(base); fs::create_directories(base);

    SlowStore store;
    const int64_t start_ms = 1700000000000LL;
    // publish an event with 200 segments
    Manifest m;
    m.event_id = "01EVENTRESPONSIVEEEEEEEEE";
    m.status = "live"; m.init = "init.mp4";
    m.video = { "h264", 1280, 720, 30.0 };
    m.audio_tracks = { { 0, "Main", "aac", 2, 48000 } };
    m.started_at_ms = start_ms; m.first_available_seq = 0;
    std::vector<uint8_t> init(1200, 0x11);
    store.put("events/" + m.event_id + "/init.mp4", init, "", {});
    for (uint64_t i = 0; i < 200; ++i) {
        std::vector<uint8_t> body(4096, (uint8_t)i);
        char nm[16]; std::snprintf(nm, sizeof(nm), "%08llu", (unsigned long long)i);
        store.put("events/" + m.event_id + "/segments/" + std::string(nm) + ".m4s",
                  body, "", {});
        ManifestSegment ms; ms.seq = i; ms.duration_s = 6.0;
        ms.checksum = sha256_hex(body);
        ms.at_ms = start_ms + (int64_t)(i * 6000);
        m.push(ms, 50);
    }
    m.updated_at_ms = start_ms + 200 * 6000;
    { auto j = m.to_json();
      store.put("events/" + m.event_id + "/manifest.json",
                std::vector<uint8_t>(j.begin(), j.end()), "", {}); }
    { LivePointer lp; lp.room_id = "r"; lp.event_id = m.event_id;
      lp.status = "live"; lp.updated_at_ms = m.updated_at_ms;
      auto j = lp.to_json();
      store.put("rooms/r/live.json", std::vector<uint8_t>(j.begin(), j.end()), "", {}); }

    DecoderConfig cfg;
    cfg.room_id = "r"; cfg.cache_dir = (base / "c").string();
    cfg.buffer_minutes = 10; cfg.prebuffer_segments = 2;
    DecoderSession dec(cfg, store);
    dec.poll(m.updated_at_ms);

    // Downloading continuously in the background, as the poll loop does, AND
    // polling. Windows mutexes favour the thread already running, so a tight
    // loop here starved the UI thread for seconds even once the network calls
    // were outside the lock. Reproducing that needs real contention, not a
    // single well-behaved worker.
    std::atomic<bool> run{true};
    std::vector<std::thread> workers;
    for (int i = 0; i < 3; ++i)
        workers.emplace_back([&]{ while (run.load()) dec.pump_downloads(8); });
    workers.emplace_back([&]{
        while (run.load()) dec.poll(m.updated_at_ms);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Now time the calls the UI makes every refresh, plus a timeline click.
    auto time_call = [](const char* what, auto fn) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("  %-28s %7.1f ms\n", what, ms);
        return ms;
    };

    double worst = 0;
    for (int i = 0; i < 5; ++i) {
        worst = std::max(worst, time_call("snapshot: behind_live_s", [&]{ dec.behind_live_s(); }));
        worst = std::max(worst, time_call("snapshot: buffered_ahead_s", [&]{ dec.buffered_ahead_s(); }));
        worst = std::max(worst, time_call("snapshot: cached_ranges", [&]{ dec.cached_ranges(); }));
        worst = std::max(worst, time_call("timeline click: seek_to_wall_ms",
            [&]{ dec.seek_to_wall_ms(start_ms + 60000 + i * 1000); }));
    }
    run = false;
    for (auto& t : workers) t.join();
    fs::remove_all(base);

    std::printf("\n  worst UI call while downloading: %.1f ms\n", worst);
    std::printf("%s\n", worst < 50.0
        ? "UI stays responsive during downloads"
        : "UI STILL BLOCKS ON DOWNLOADS");
    return worst < 50.0 ? 0 : 1;
}
