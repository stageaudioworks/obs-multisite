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
    std::atomic<bool> all_down{false};   // the whole link, control objects too
    // Sticky, as S3Transport's is: once cancelled (RetryUploader::stop() does
    // it, so a join never waits out a request), every request fails at once
    // until resume_pending(). A fake without this hid a finisher that
    // published through a transport its own uploader had just cancelled.
    std::atomic<bool> cancelled{false};
    void cancel_pending() override { cancelled = true; }
    void resume_pending() override { cancelled = false; }
    PutResult put(const std::string& k, const std::vector<uint8_t>& b, const std::string&,
                  const std::map<std::string,std::string>&) override {
        if (cancelled) return {false, 0, true, "cancelled"};
        if (all_down || (segments_down && k.find("/segments/") != std::string::npos))
            return {false, 0, true, "network down"};
        std::lock_guard<std::mutex> l(mtx); objects[k] = b; return {true, 200, true, ""};
    }
    GetResult get(const std::string& k) override {
        if (cancelled || all_down) { GetResult r; r.error = "network down"; return r; }
        std::lock_guard<std::mutex> l(mtx); GetResult r; auto it = objects.find(k);
        if (it == objects.end()) { r.http_status = 404; return r; }
        r.success = true; r.http_status = 200; r.body = it->second; return r;
    }
    bool has(const std::string& k) const { std::lock_guard<std::mutex> l(mtx); return objects.count(k) > 0; }
    LivePointer live(const std::string& room) const {
        std::lock_guard<std::mutex> l(mtx);
        auto it = objects.find(live_pointer_key(room));
        if (it == objects.end()) return LivePointer{};
        return LivePointer::from_json(std::string(it->second.begin(), it->second.end()));
    }
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

    std::printf("== An End the link could not carry is published once it can ==\n");
    {
        // Seen live 2026-10-03: End during an outage, the final manifest.json
        // and live.json PUTs timed out, and live.json went on saying "live" —
        // campuses would call a finished event "interrupted".
        const fs::path r2 = unique_temp_dir("multisite_finisher_end");
        fs::remove_all(r2);
        Store st;
        SessionConfig c; c.room_id = "r"; c.spool_dir = r2.string();
        c.base_backoff_ms = 2; c.max_backoff_ms = 10; c.backoff_jitter = 0.0;
        std::string id;
        {
            Session s1(c, st);
            s1.start_new(blob(0, 300), video, tracks);
            id = s1.event_id();
            s1.publish_segment(blob(1), 6.0, 0.0);
            for (int i = 0; i < 200 && s1.status().pending > 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            st.all_down = true;                 // the link goes, then End
            s1.end(std::chrono::milliseconds(50));
        }
        CHECK(st.live("r").status == "live",
              "End could not say so: live.json still says live");
        CHECK(fs::exists(r2 / id) && seg_files(r2 / id) == 0,
              "the event's folder is kept, with no segments, because the ending is owed");
        CHECK(SpoolQueue::finishing_events(r2.string()) == std::vector<std::string>{id},
              "and the finisher has it as work");

        FinisherConfig fc; fc.spool_root = r2.string();
        fc.transport = [&]() -> std::shared_ptr<Transport> {
            return std::shared_ptr<Transport>(&st, [](Transport*) {});
        };
        EventFinisher fin(fc);
        fin.run_once();
        CHECK(fs::exists(r2 / id), "while the link is down, nothing is lost and it waits");
        st.all_down = false;
        fin.run_once();
        CHECK(st.manifest(id).status == "ended", "once it is back, the manifest says ended");
        CHECK(st.live("r").status == "ended" && st.live("r").event_id == id,
              "and so does live.json, for this event");
        CHECK(!fs::exists(r2 / id), "then the folder goes");
        fs::remove_all(r2);
    }

    std::printf("== A newer broadcast's live.json is never told it has ended ==\n");
    {
        const fs::path r3 = unique_temp_dir("multisite_finisher_newer");
        fs::remove_all(r3);
        Store st;
        SessionConfig c; c.room_id = "r"; c.spool_dir = r3.string();
        c.base_backoff_ms = 2; c.max_backoff_ms = 10; c.backoff_jitter = 0.0;
        std::string old_id, new_id;
        {
            Session s1(c, st);
            s1.start_new(blob(0, 300), video, tracks);
            old_id = s1.event_id();
            st.all_down = true;
            s1.end(std::chrono::milliseconds(50));
        }
        st.all_down = false;
        {
            Session s2(c, st);
            s2.start_new(blob(0, 300), video, tracks);   // the room's next broadcast
            new_id = s2.event_id();
            FinisherConfig fc; fc.spool_root = r3.string();
            fc.transport = [&]() -> std::shared_ptr<Transport> {
                return std::shared_ptr<Transport>(&st, [](Transport*) {});
            };
            EventFinisher fin(fc);
            fin.run_once();
            CHECK(st.manifest(old_id).status == "ended", "the old event's manifest is ended");
            CHECK(st.live("r").event_id == new_id && st.live("r").status == "live",
                  "but live.json, which names the newer broadcast, is left alone");
            CHECK(!fs::exists(r3 / old_id), "and the old folder goes");
            s2.end();
        }
        fs::remove_all(r3);
    }

    std::printf("== Starting a new event over an unfinished one owes the old one its End ==\n");
    {
        const fs::path r4 = unique_temp_dir("multisite_finisher_crash");
        fs::remove_all(r4);
        Store st;
        SessionConfig c; c.room_id = "r"; c.spool_dir = r4.string();
        std::string crashed;
        {
            Session s1(c, st);
            s1.start_new(blob(0, 300), video, tracks);
            crashed = s1.event_id();
        }   // a crash: never Ended
        {
            Session s2(c, st);
            s2.start_new(blob(0, 300), video, tracks);   // "start new", not resume
            FinisherConfig fc; fc.spool_root = r4.string();
            fc.transport = [&]() -> std::shared_ptr<Transport> {
                return std::shared_ptr<Transport>(&st, [](Transport*) {});
            };
            EventFinisher fin(fc);
            fin.run_once();
            CHECK(st.manifest(crashed).status == "ended",
                  "the abandoned event is published as ended, not left looking interrupted");
            s2.end();
        }
        fs::remove_all(r4);
    }

    std::printf("== Ended folders with nothing owed are tidied away ==\n");
    {
        const fs::path r5 = unique_temp_dir("multisite_finisher_tidy");
        fs::remove_all(r5);
        auto folder = [&](const char* id, bool ended, bool owed, bool seg) {
            fs::create_directories(r5 / id);
            std::ofstream(r5 / id / "state.json")
                << "{\"event_id\":\"" << id << "\",\"ended\":" << (ended ? "true" : "false")
                << ",\"end_unpublished\":" << (owed ? "true" : "false") << "}";
            if (seg) std::ofstream(r5 / id / "00000001.seg") << "bytes";
        };
        folder("01EMPTYENDED", true, false, false);   // what the migration left
        folder("01OWEDEND", true, true, false);       // its ending is still owed
        folder("01BACKLOG", true, false, true);       // segments still to send
        folder("01LIVE", false, false, false);        // the live (resumable) event
        fs::create_directories(r5 / "01UNREADABLE");  // no state.json: leave it
        CHECK(SpoolQueue::remove_finished(r5.string()) == 1,
              "exactly one folder is removed");
        CHECK(!fs::exists(r5 / "01EMPTYENDED"), "the empty, ended one");
        CHECK(fs::exists(r5 / "01OWEDEND") && fs::exists(r5 / "01BACKLOG") &&
                  fs::exists(r5 / "01LIVE") && fs::exists(r5 / "01UNREADABLE"),
              "and none that is owed anything, still live, or unreadable");
        fs::remove_all(r5);
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
