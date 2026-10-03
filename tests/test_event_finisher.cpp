// SPDX-License-Identifier: GPL-3.0-or-later
// test_event_finisher.cpp — End never throws away an unsent backlog.
//
// End drains to a deadline. What it could not send used to be marked ended
// with the event and deleted by the next Go Live, which cleared the one spool
// folder there was. Now each event has its own folder (spool_queue.h) and
// EventFinisher uploads an ended event's backlog after it is over, adding each
// segment to that event's manifest, the live event first.
#include "../src/core/event_finisher.h"
#include "../src/core/session.h"
#include "../src/core/model.h"
#include "test_tmpdir.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// A bucket whose segment uploads can be switched off — the uplink failing —
// while the small control objects still land.
class Store : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    mutable std::mutex mtx;
    std::atomic<bool> segments_down{false};
    PutResult put(const std::string& k, const std::vector<uint8_t>& b, const std::string&,
                  const std::map<std::string,std::string>&) override {
        if (segments_down && k.find("/segments/") != std::string::npos)
            return {false, 0, true, "network down"};
        std::lock_guard<std::mutex> l(mtx); objects[k] = b; return {true, 200, true, ""};
    }
    GetResult get(const std::string& k) override {
        std::lock_guard<std::mutex> l(mtx); GetResult r; auto it = objects.find(k);
        if (it == objects.end()) { r.http_status = 404; return r; }
        r.success = true; r.http_status = 200; r.body = it->second; return r;
    }
    bool has(const std::string& k) const { std::lock_guard<std::mutex> l(mtx); return objects.count(k) > 0; }
    Manifest manifest(const std::string& id) const {
        std::lock_guard<std::mutex> l(mtx);
        auto it = objects.find("events/" + id + "/manifest.json");
        if (it == objects.end()) return Manifest{};
        return Manifest::from_json(std::string(it->second.begin(), it->second.end()));
    }
};

static std::vector<uint8_t> blob(int n, size_t sz = 2048) {
    std::vector<uint8_t> v(sz); for (size_t i = 0; i < sz; ++i) v[i] = (uint8_t)((n * 31 + i) & 0xFF); return v;
}
static size_t seg_files(const fs::path& dir) {
    size_t n = 0; std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) if (e.path().extension() == ".seg") ++n;
    return n;
}

int main() {
    const fs::path root = unique_temp_dir("multisite_finisher");
    fs::remove_all(root);
    const VideoInfo video{ "h264", 1920, 1080, 30.0 };
    const std::vector<AudioTrack> tracks = { { 0, "Main", "aac", 2, 48000 } };
    auto session_cfg = [&] {
        SessionConfig c; c.room_id = "r"; c.spool_dir = root.string();
        c.base_backoff_ms = 2; c.max_backoff_ms = 10; c.backoff_jitter = 0.0;
        return c;
    };

    std::printf("== End with a dead uplink, then a new event, then the uplink back ==\n");
    Store store;
    std::string first, second;
    {
        Session a(session_cfg(), store);
        a.start_new(blob(0, 300), video, tracks);
        first = a.event_id();
        a.publish_segment(blob(1), 6.0, 0.0);              // goes up
        for (int i = 0; i < 200 && a.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        store.segments_down = true;                         // the uplink fails
        for (int i = 2; i <= 4; ++i) a.publish_segment(blob(i), 6.0, (i - 1) * 6.0);
        a.end(std::chrono::milliseconds(150));              // End: the drain runs out
    }
    CHECK(seg_files(root / first) == 3,
          "End leaves the 3 unsent segments in the event's own folder");
    CHECK(SpoolQueue::finishing_events(root.string()) == std::vector<std::string>{first},
          "and the event is listed as ended with a backlog");
    CHECK(store.manifest(first).status == "ended" && store.manifest(first).latest_seq == 0,
          "it ended on air at once: the manifest says ended, with what had landed");

    {
        Session b(session_cfg(), store);
        CHECK(!b.check_resumable().resumable, "an ended event is not offered for resume");
        b.start_new(blob(0, 300), video, tracks);           // the next Go Live
        second = b.event_id();
        b.publish_segment(blob(9), 6.0, 0.0);
        CHECK(seg_files(root / first) == 3,
              "the next Go Live does not delete the previous event's backlog");
        store.segments_down = false;
        for (int i = 0; i < 200 && b.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        b.end();
    }
    CHECK(!fs::exists(root / second), "an event that sent everything leaves no folder");

    std::printf("== The finisher yields to the live event, then completes the old one ==\n");
    {
        bool live_busy = true;
        FinisherConfig fc;
        fc.spool_root = root.string();
        fc.transport = [&]() -> std::shared_ptr<Transport> {
            return std::shared_ptr<Transport>(&store, [](Transport*) {});
        };
        fc.may_upload = [&] { return !live_busy; };
        fc.base_backoff_ms = 2; fc.max_backoff_ms = 10;
        EventFinisher fin(fc);
        CHECK(fin.status().events == 1 && fin.status().segments == 3, "status counts the backlog");
        fin.run_once();
        CHECK(seg_files(root / first) == 3, "while the live event has work, nothing is sent");
        live_busy = false;
        fin.run_once();
        const Manifest m = store.manifest(first);
        CHECK(m.latest_seq == 3 && m.status == "ended",
              "then the old event's manifest runs to its last segment, still ended");
        bool all = true;
        for (int s = 0; s <= 3; ++s) {
            char k[96]; std::snprintf(k, sizeof k, "events/%s/segments/%08d.m4s", first.c_str(), s);
            if (!store.has(k)) all = false;
        }
        CHECK(all, "and all four of its segments are in the bucket");
        CHECK(!fs::exists(root / first), "its folder is removed once empty");
        CHECK(fin.status().events == 0, "and there is nothing left to finish");
    }

    std::printf("== Abandoning is explicit, and deletes ==\n");
    {
        Store dead; dead.segments_down = true;
        {
            Session c(session_cfg(), dead);
            c.start_new(blob(0, 300), video, tracks);
            c.publish_segment(blob(1), 6.0, 0.0);
            c.end(std::chrono::milliseconds(50));
        }
        FinisherConfig fc; fc.spool_root = root.string();
        fc.transport = [&]() -> std::shared_ptr<Transport> {
            return std::shared_ptr<Transport>(&dead, [](Transport*) {});
        };
        EventFinisher fin(fc);
        CHECK(fin.status().events == 1, "a backlog waits");
        fin.abandon();
        CHECK(fin.status().events == 0 && SpoolQueue::finishing_events(root.string()).empty(),
              "abandon() removes it, and only when asked");
    }

    std::printf("== A spool in the old single-folder layout carries on ==\n");
    {
        const fs::path old = unique_temp_dir("multisite_oldspool");
        fs::remove_all(old);
        {
            // Write the old layout by hand: state.json and segments in the root.
            fs::create_directories(old);
            std::ofstream(old / "state.json")
                << "{\"event_id\":\"01OLDLAYOUT\",\"first_seq\":0,\"last_enqueued\":1,"
                   "\"last_confirmed\":0,\"targets\":1,\"any_confirmed\":true,\"ended\":false,"
                   "\"last_activity_ms\":1}";
            std::ofstream(old / "00000001.seg") << "bytes";
            std::ofstream(old / "00000001.meta") << "{\"seq\":1,\"checksum\":\"\",\"key\":\"k\"}";
        }
        SpoolQueue q(old.string());
        CHECK(fs::exists(old / "01OLDLAYOUT" / "00000001.seg") && !fs::exists(old / "00000001.seg"),
              "its files are moved into the event's own folder");
        const ResumeInfo info = q.inspect();
        CHECK(info.resumable && info.event_id == "01OLDLAYOUT" && info.pending_count == 1,
              "and the unfinished event is still offered for resume, segment and all");
        fs::remove_all(old);
    }

    fs::remove_all(root);
    std::printf("\n%s\n", g_fail == 0 ? "ALL EVENT FINISHER TESTS PASSED" : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
