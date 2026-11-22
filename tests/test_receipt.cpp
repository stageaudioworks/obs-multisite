// SPDX-License-Identifier: GPL-3.0-or-later
// test_receipt.cpp — the delivery receipt (receipt.h;
// docs/scope/upload-delivery-assurance.md R1).
//
//   1. The counts, including the zero-sentinel case: one segment numbered 0
//      is one captured, not none.
//   2. JSON round-trip, and the state an operator reads.
//   3. A clean End writes "complete" with every segment counted.
//   4. An End with the link down writes "finishing"; EventFinisher, once the
//      backlog lands, rewrites it as "complete".
//   5. Segments evicted for disk space make it "incomplete", with the gaps.
//
#include "../src/core/event_finisher.h"
#include "../src/core/receipt.h"
#include "../src/core/session.h"
#include "test_tmpdir.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace multisite;

static int g_failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)

class Store : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    std::mutex mtx;
    std::atomic<bool> segments_down{false};
    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string, std::string>&) override {
        if (segments_down && key.find("/segments/") != std::string::npos)
            return {false, 0, true, "network down"};
        std::lock_guard<std::mutex> lk(mtx);
        objects[key] = body;
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
    DeliveryReceipt receipt(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx);
        auto& b = objects.at(receipt_key(id));
        return DeliveryReceipt::from_json(std::string(b.begin(), b.end()));
    }
    bool has(const std::string& k) { std::lock_guard<std::mutex> lk(mtx); return objects.count(k) > 0; }
};

static std::vector<uint8_t> blob(uint64_t n, size_t sz = 2048) {
    std::vector<uint8_t> v(sz);
    for (size_t i = 0; i < sz; ++i) v[i] = (uint8_t)((n * 31 + i) & 0xFF);
    return v;
}

int main() {
    std::printf("== 1. The counts ==\n");
    {
        Manifest m; m.event_id = "E"; m.first_available_seq = 0;
        auto r = make_receipt(m, false, 0, 0);
        CHECK(r.captured == 0 && r.in_cloud == 0, "nothing enqueued: nothing captured");
        r = make_receipt(m, true, 0, 0);
        CHECK(r.captured == 1 && r.in_cloud == 1 && r.state() == "complete",
              "one segment numbered 0 is one captured, not none");
        r = make_receipt(m, true, 99, 3);
        CHECK(r.captured == 100 && r.pending == 3 && r.in_cloud == 97 &&
              r.state() == "finishing", "100 captured, 3 pending: 97 in the cloud, finishing");
        m.gaps = { {10, 12}, {50, 50} };
        r = make_receipt(m, true, 99, 0);
        CHECK(r.lost == 4 && r.in_cloud == 96 && r.gaps.size() == 2 &&
              r.state() == "incomplete", "4 lost in two gaps: incomplete, 96 in the cloud");
        Manifest m2; m2.event_id = "E"; m2.first_available_seq = 300;
        m2.gaps = { {290, 305}, {400, 410} };
        r = make_receipt(m2, true, 399, 0);
        CHECK(r.captured == 100 && r.lost == 6 && r.gaps.size() == 1 && r.gaps[0].first == 300,
              "a resumed event counts from its first seq; gaps are clipped to what was captured");
    }

    std::printf("== 2. Format ==\n");
    {
        Manifest m; m.event_id = "E"; m.gaps = { {3, 4} }; m.group_k = 10;
        auto r = make_receipt(m, true, 9, 1);
        r.writer = "encoder"; r.written_at_ms = 123;
        r.mirror_configured = true;
        auto b = DeliveryReceipt::from_json(r.to_json());
        CHECK(b.captured == 10 && b.lost == 2 && b.pending == 1 && b.in_cloud == 7 &&
              b.gaps == r.gaps && b.group_k == 10 && b.writer == "encoder" &&
              b.mirror_configured && !b.mirror_complete, "round-trips");
        CHECK(r.to_json().find("\"state\":\"incomplete\"") != std::string::npos,
              "the state is written out for a reader that only wants that");
        std::printf("      summary: %s\n", r.summary().c_str());
        CHECK(r.summary().find("7 of 10") != std::string::npos &&
              r.summary().find("2 were lost") != std::string::npos, "the summary says it plainly");
    }

    fs::path base = unique_temp_dir("multisite_receipt");
    fs::remove_all(base);
    VideoInfo video{ "h264", 1920, 1080, 30.0 };
    std::vector<AudioTrack> tracks = { { 0, "Main mix", "aac", 2, 48000 } };
    auto config = [&](const std::string& name) {
        SessionConfig cfg;
        cfg.room_id = "r";
        cfg.spool_dir = (base / name).string();
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        return cfg;
    };

    std::printf("== 3. A clean End is complete ==\n");
    {
        Store store;
        Session ses(config("clean"), store);
        CHECK(ses.start_new(blob(0), video, tracks), "start_new");
        for (uint64_t i = 0; i < 12; ++i) ses.publish_segment(blob(i + 1), 6.0, i * 6.0);
        CHECK(ses.end(), "end");
        auto r = store.receipt(ses.event_id());
        CHECK(r.captured == 12 && r.in_cloud == 12 && r.state() == "complete" &&
              r.writer == "encoder", "12 of 12 in the cloud, complete, written by the encoder");
        CHECK(ses.receipt().in_cloud == 12, "and the Session hands the same receipt to its host");
    }

    std::printf("== 4. Finishing, then complete ==\n");
    {
        Store store;
        std::string id;
        {
            Session ses(config("finish"), store);
            CHECK(ses.start_new(blob(0), video, tracks), "start_new");
            id = ses.event_id();
            store.segments_down = true;
            for (uint64_t i = 0; i < 6; ++i) ses.publish_segment(blob(i + 1), 6.0, i * 6.0);
            ses.end(std::chrono::milliseconds(200));
            auto r = store.receipt(id);
            CHECK(r.state() == "finishing" && r.pending == 6 && r.lost == 0,
                  "End with the link down: finishing, 6 pending, none lost");
        }
        store.segments_down = false;
        FinisherConfig fc;
        fc.spool_root = (base / "finish").string();
        fc.transport = [&] { return std::shared_ptr<Transport>(&store, [](Transport*) {}); };
        fc.base_backoff_ms = 1; fc.max_backoff_ms = 2;
        EventFinisher fin(fc);
        fin.start();
        for (int i = 0; i < 200 && fin.status().events > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fin.stop();
        auto r = store.receipt(id);
        CHECK(r.state() == "complete" && r.in_cloud == 6 && r.writer == "finisher",
              "the finisher rewrites it: 6 of 6, complete");
    }

    std::printf("== 5. Evicted for disk space: incomplete ==\n");
    {
        Store store;
        auto cfg = config("evict");
        cfg.max_spool_bytes = 3 * 2048;
        Session ses(cfg, store);
        CHECK(ses.start_new(blob(0), video, tracks), "start_new");
        store.segments_down = true;
        for (uint64_t i = 0; i < 8; ++i) ses.publish_segment(blob(i + 1), 6.0, i * 6.0);
        store.segments_down = false;
        CHECK(ses.end(), "end");
        auto r = store.receipt(ses.event_id());
        std::printf("      %s\n", r.summary().c_str());
        CHECK(r.state() == "incomplete" && r.lost > 0 && !r.gaps.empty() &&
              r.captured == 8 && r.in_cloud + r.lost + r.pending == 8,
              "incomplete, naming what was lost, and the counts add up");
    }

    fs::remove_all(base);
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
