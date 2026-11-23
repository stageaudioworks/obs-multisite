// SPDX-License-Identifier: GPL-3.0-or-later
// test_parity.cpp — segment groups: a checksum index and XOR parity, so a
// campus verifies any segment and rebuilds one the store lost or damaged,
// with nothing re-sent (parity.h; docs/scope/upload-delivery-assurance.md R5).
//
//   1. The accumulator closes groups at their boundary, at End, and across a
//      gap or a resume, and never names a segment it was not given.
//   2. Rebuild: exact bytes back; refused when the result does not match, when
//      two members are missing, or when there is no parity.
//   3. The index and the manifest's group_k round-trip; an old manifest reads
//      as no groups; a nonsense group_k is ignored.
//   4. ObjectQueue: in order, through an outage, and across a restart.
//   5. End to end: a Session publishes each group's parity BEFORE its index.
//   6. A campus far behind the manifest's window used to store segments
//      unchecked. It now verifies them against the index and refuses a damaged
//      one — and rebuilds a deleted one and a damaged one from parity.
//   7. The backlog an End leaves is grouped too: the finisher carries on the
//      group End published in part (a superset, never a subset), and the
//      late groups rebuild a deleted segment.
//   8. The second bucket gets the groups as well, without the live feed ever
//      waiting on it; what End leaves for it goes at the next Go Live.
//
#include "../src/core/decoder_session.h"
#include "../src/core/event_finisher.h"
#include "../src/core/object_queue.h"
#include "../src/core/parity.h"
#include "../src/core/session.h"
#include "../src/core/checksum.h"
#include "test_tmpdir.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace fs = std::filesystem;
using namespace multisite;

static int g_failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ std::printf("  [FAIL] %s\n", msg); ++g_failures; } \
                              else { std::printf("  [ok]   %s\n", msg); } } while(0)

static std::vector<uint8_t> seg_bytes(uint64_t seq) {
    // Different lengths, so padding is exercised.
    std::vector<uint8_t> v(1500 + (seq * 37) % 700);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)((seq * 131 + i * 7) & 0xFF);
    return v;
}

class Store : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    std::vector<std::string> put_order;
    std::mutex mtx;
    std::atomic<bool> down{false};
    std::atomic<bool> segments_down{false};   // only the segment uploads fail
    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string, std::string>&) override {
        if (down) return {false, 0, true, "network down"};
        if (segments_down && key.find("/segments/") != std::string::npos)
            return {false, 0, true, "network down"};
        std::lock_guard<std::mutex> lk(mtx);
        objects[key] = body;
        put_order.push_back(key);
        return {true, 200, true, ""};
    }
    GetResult get(const std::string& key) override {
        std::lock_guard<std::mutex> lk(mtx);
        GetResult r;
        auto it = objects.find(key);
        if (down) { r.error = "network down"; return r; }
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
    size_t index_of(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx);
        for (size_t i = 0; i < put_order.size(); ++i) if (put_order[i] == key) return i;
        return (size_t)-1;
    }
    size_t last_index_of(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx);
        for (size_t i = put_order.size(); i-- > 0;) if (put_order[i] == key) return i;
        return (size_t)-1;
    }
    bool has(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx);
        return objects.count(key) > 0;
    }
    std::optional<GroupIndex> index(const std::string& pfx, uint64_t g) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = objects.find(group_index_key(pfx, g));
        if (it == objects.end()) return std::nullopt;
        return GroupIndex::from_json(std::string(it->second.begin(), it->second.end()));
    }
};

// Cancels as S3Transport does: sticky until resume_pending(). The second
// bucket's queue is stopped at End, which cancels; the next Go Live must re-arm
// it or the leftovers never go.
class StickyStore : public Store {
public:
    std::atomic<bool> cancelled{false};
    void cancel_pending() override { cancelled = true; }
    void resume_pending() override { cancelled = false; }
    PutResult put(const std::string& k, const std::vector<uint8_t>& b, const std::string& t,
                  const std::map<std::string, std::string>& tags) override {
        if (cancelled) return {false, 0, true, "cancelled"};
        return Store::put(k, b, t, tags);
    }
};

static std::string seg_key_of(const std::string& pfx, uint64_t s) {
    char b[16]; std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)s);
    return pfx + "segments/" + b + ".m4s";
}

// Rebuild `missing` from what `store` holds, as a campus would.
static std::optional<std::vector<uint8_t>> rebuild_from(Store& st, const std::string& pfx,
                                                        uint64_t missing, int k) {
    auto gi = st.index(pfx, group_of(missing, k));
    if (!gi) return std::nullopt;
    std::lock_guard<std::mutex> lk(st.mtx);
    auto p = st.objects.find(group_parity_key(pfx, gi->group));
    if (p == st.objects.end()) return std::nullopt;
    std::map<uint64_t, std::vector<uint8_t>> others;
    for (const auto& m : gi->members) {
        if (m.seq == missing) continue;
        auto o = st.objects.find(seg_key_of(pfx, m.seq));
        if (o == st.objects.end()) return std::nullopt;
        others[m.seq] = o->second;
    }
    return rebuild_member(*gi, missing, p->second, others);
}

int main() {
    fs::path base = unique_temp_dir("multisite_parity");
    fs::remove_all(base);
    fs::create_directories(base);

    std::printf("== 1. The accumulator ==\n");
    {
        ParityAccumulator acc(4);
        std::vector<ParityAccumulator::Finished> out;
        for (uint64_t s = 0; s < 10; ++s) {
            auto b = seg_bytes(s);
            auto f = acc.add(s, b, sha256_hex(b));
            for (auto& x : f) out.push_back(std::move(x));
        }
        CHECK(out.size() == 2, "two full groups of 4 close as their last member arrives");
        CHECK(out.size() == 2 && out[0].index.members.size() == 4 &&
              out[0].index.group == 0 && out[1].index.group == 1, "groups 0 and 1, 4 members each");
        auto last = acc.flush();
        CHECK(last && last->index.group == 2 && last->index.members.size() == 2,
              "End closes the partial group with the 2 it holds");
        CHECK(!acc.flush(), "and a second flush has nothing");
        uint64_t longest = 0;
        for (uint64_t s = 0; s < 4; ++s) longest = std::max<uint64_t>(longest, seg_bytes(s).size());
        CHECK(!out.empty() && out[0].index.parity_len == longest, "parity is as long as the longest member");
    }
    {
        // Seq 2 was lost before upload (a gap), and the group closes when the
        // next group's first segment arrives.
        ParityAccumulator acc(4);
        std::vector<ParityAccumulator::Finished> out;
        for (uint64_t s : {0, 1, 3, 4}) {
            auto b = seg_bytes(s);
            for (auto& x : acc.add(s, b, sha256_hex(b))) out.push_back(std::move(x));
        }
        CHECK(out.size() == 1 && out[0].index.members.size() == 3 && !out[0].index.member(2),
              "a gap is not a member, and its group still closes");
    }
    {
        ParityAccumulator acc(4);
        std::vector<ParityAccumulator::Finished> out;
        for (uint64_t s : {0, 1, 2}) { auto b = seg_bytes(s); acc.add(s, b, sha256_hex(b)); }
        auto f = acc.skip(3);   // the last member was damaged on the encoder's disk
        CHECK(f.size() == 1 && f[0].index.members.size() == 3,
              "skip() of the last member closes the group at once");
    }
    {
        // A resume picks up mid-group: only what this run saw is a member.
        ParityAccumulator acc(4);
        auto b6 = seg_bytes(6), b7 = seg_bytes(7);
        acc.add(6, b6, sha256_hex(b6));
        auto f = acc.add(7, b7, sha256_hex(b7));
        CHECK(f.size() == 1 && f[0].index.members.size() == 2 && f[0].index.group == 1,
              "a resume mid-group publishes the members it has");
    }

    std::printf("== 2. Rebuild ==\n");
    {
        ParityAccumulator acc(5);
        std::map<uint64_t, std::vector<uint8_t>> all;
        std::vector<ParityAccumulator::Finished> out;
        for (uint64_t s = 10; s < 15; ++s) {
            all[s] = seg_bytes(s);
            for (auto& x : acc.add(s, all[s], sha256_hex(all[s]))) out.push_back(std::move(x));
        }
        CHECK(out.size() == 1, "one group");
        const auto& g = out[0];
        auto others = all; others.erase(12);
        auto r = rebuild_member(g.index, 12, g.parity, others);
        CHECK(r && *r == all[12], "the missing member comes back byte for byte");
        auto r_short = rebuild_member(g.index, 13, g.parity,
            [&] { auto o = all; o.erase(13); return o; }());
        CHECK(r_short && *r_short == all[13], "including one shorter than the parity");
        auto two = others; two.erase(14);
        CHECK(!rebuild_member(g.index, 12, g.parity, two), "two missing: refused");
        auto bad = others; bad[11][5] ^= 1;
        CHECK(!rebuild_member(g.index, 12, g.parity, bad),
              "a damaged other member: refused, by the checksum");
        GroupIndex nop = g.index; nop.parity_len = 0;
        CHECK(!rebuild_member(nop, 12, g.parity, others), "no parity: refused");
        CHECK(!rebuild_member(g.index, 99, g.parity, others), "not a member: refused");
    }

    std::printf("== 3. Formats ==\n");
    {
        GroupIndex g; g.k = 10; g.group = 7; g.parity_len = 1234;
        g.members = { {70, 1000, "aa"}, {72, 1234, "bb"} };
        GroupIndex r = GroupIndex::from_json(g.to_json());
        CHECK(r.k == 10 && r.group == 7 && r.parity_len == 1234 && r.members.size() == 2 &&
              r.member(72) && r.member(72)->checksum == "bb", "index round-trips");
        CHECK(group_index_key("events/E/", 7) == "events/E/groups/00000007.json" &&
              group_parity_key("events/E/", 7) == "events/E/parity/00000007.bin", "keys");
        Manifest m; m.event_id = "E"; m.group_k = 10;
        CHECK(Manifest::from_json(m.to_json()).group_k == 10, "manifest group_k round-trips");
        Manifest old; old.event_id = "E";
        CHECK(old.to_json().find("group_k") == std::string::npos &&
              Manifest::from_json(old.to_json()).group_k == 0,
              "no groups: the field is absent, and absent reads as 0");
        CHECK(Manifest::from_json(R"({"event_id":"E","group_k":"ten"})").group_k == 0 &&
              Manifest::from_json(R"({"event_id":"E","group_k":-3})").group_k == 0,
              "a nonsense group_k is ignored rather than trusted");
    }

    std::printf("== 4. ObjectQueue ==\n");
    {
        Store store;
        const std::string dir = (base / "oq").string();
        {
            ObjectQueue q(dir, store, 1, 5);
            store.down = true;
            q.enqueue("a", {1}, "application/octet-stream");
            q.enqueue("b", {2}, "application/json");
            q.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(q.pending() == 2 && store.objects.empty(), "held through an outage");
            q.stop();
        }
        {
            ObjectQueue q(dir, store, 1, 5);   // a restart on the same folder
            q.enqueue("c", {3}, "application/json");
            store.down = false;
            CHECK(q.drain_blocking(2000), "a restarted queue drains what was left");
            CHECK(store.put_order == std::vector<std::string>({"a", "b", "c"}),
                  "in the order they were queued, across the restart");
            CHECK(q.stored_of_type("application/json") == 2, "counts by type");
            q.remove_dir_if_empty();
            CHECK(!fs::exists(dir), "an empty queue's folder is removed");
        }
    }

    VideoInfo video{ "h264", 1920, 1080, 30.0 };
    std::vector<AudioTrack> tracks = { { 0, "Main mix", "aac", 2, 48000 } };
    Store store;
    std::string event_id;
    std::map<uint64_t, std::vector<uint8_t>> sent;

    std::printf("== 5. A Session publishes groups, parity before index ==\n");
    {
        SessionConfig cfg;
        cfg.room_id = "r";
        cfg.spool_dir = (base / "enc").string();
        cfg.parity_group = 10;
        cfg.manifest_window = 5;   // so most segments are outside the window
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        Session ses(cfg, store);
        CHECK(ses.start_new(seg_bytes(999), video, tracks), "start_new");
        event_id = ses.event_id();
        for (uint64_t i = 0; i < 25; ++i) {
            sent[i] = seg_bytes(i);
            ses.publish_segment(sent[i], 6.0, i * 6.0);
        }
        for (int i = 0; i < 200 && ses.status().pending > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(ses.end(), "end");
        const std::string pfx = "events/" + event_id + "/";
        bool all = true, ordered = true;
        for (uint64_t g = 0; g < 3; ++g) {
            const auto ik = group_index_key(pfx, g), pk = group_parity_key(pfx, g);
            if (!store.objects.count(ik) || !store.objects.count(pk)) all = false;
            else if (store.index_of(pk) > store.index_of(ik)) ordered = false;
        }
        CHECK(all, "groups 0, 1 and the partial group 2 each have an index and parity");
        CHECK(ordered, "each parity was stored before its index");
        auto gi = GroupIndex::from_json(std::string(
            store.objects[group_index_key(pfx, 2)].begin(), store.objects[group_index_key(pfx, 2)].end()));
        CHECK(gi.members.size() == 5 && gi.member(24) &&
              gi.member(24)->checksum == sha256_hex(sent[24]), "the partial group lists 20..24 with their checksums");
        auto m = Manifest::from_json(std::string(store.objects[pfx + "manifest.json"].begin(),
                                                 store.objects[pfx + "manifest.json"].end()));
        CHECK(m.group_k == 10, "the manifest says groups of 10");
        CHECK(!fs::exists(cfg.spool_dir + ".objects/" + event_id),
              "the group queue's folder is gone once drained");
    }

    std::printf("== 6. A campus far behind the window verifies and rebuilds ==\n");
    {
        const std::string pfx = "events/" + event_id + "/";
        auto seg_key = [&](uint64_t s) {
            char b[16]; std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)s);
            return pfx + "segments/" + b + ".m4s";
        };
        store.objects.erase(seg_key(3));            // the store lost one
        store.objects[seg_key(13)][100] ^= 0xFF;     // and damaged another
        // In group 2, segment 20 is damaged AND its group-mate 21 is gone, so
        // neither can be rebuilt — but the damaged one must still be refused.
        store.objects[seg_key(20)][50] ^= 0xFF;
        store.objects.erase(seg_key(21));

        DecoderConfig dc;
        dc.room_id = "r";
        dc.cache_dir = (base / "dec").string();
        dc.buffer_minutes = 60;
        dc.max_cached_segments = 100;
        DecoderSession dec(dc, store);
        dec.poll();
        for (int i = 0; i < 6; ++i) dec.pump_downloads(30);
        const auto& st = dec.stats();
        std::printf("      verified by group: %llu, rebuilt: %llu, checksum failures: %llu\n",
                    (unsigned long long)st.verified_by_group, (unsigned long long)st.rebuilt,
                    (unsigned long long)st.checksum_failures);
        CHECK(st.verified_by_group > 0,
              "segments outside the manifest's window were checked against the index");
        auto c3 = dec.cache().load(3), c13 = dec.cache().load(13);
        CHECK(c3 && *c3 == sent[3], "a segment the store lost is rebuilt, byte for byte");
        // Segment 13 is outside the manifest's window. Without the index the
        // campus had no checksum for it and stored the damaged bytes as they
        // came — verified to fail that way with groups switched off.
        CHECK(c13 && *c13 == sent[13], "a segment the store damaged is rebuilt, byte for byte");
        CHECK(st.rebuilt >= 2, "and both are counted as rebuilt");
        CHECK(!dec.cache().load(20),
              "a damaged segment that cannot be rebuilt is refused, not stored");
        bool rest = true;
        for (uint64_t s = 0; s < 25; ++s)
            if (s != 20 && s != 21) { auto c = dec.cache().load(s); if (!c || *c != sent[s]) rest = false; }
        CHECK(rest, "every other segment is cached intact");
    }

    std::printf("== 7. The finisher groups the backlog End left, carrying on End's partial group ==\n");
    {
        // End with the link down in the middle of group 1: 10..12 confirmed,
        // 13..24 still spooled, and End's flush of group 1 (10..12) cannot
        // land, so it waits on disk in the event's object queue.
        Store st;
        const fs::path root = base / "fin";
        SessionConfig cfg;
        cfg.room_id = "r";
        cfg.spool_dir = root.string();
        cfg.parity_group = 10;
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        std::string id;
        std::map<uint64_t, std::vector<uint8_t>> bytes;
        {
            Session ses(cfg, st);
            CHECK(ses.start_new(seg_bytes(999), video, tracks), "start_new");
            id = ses.event_id();
            for (uint64_t i = 0; i < 13; ++i) {
                bytes[i] = seg_bytes(i);
                ses.publish_segment(bytes[i], 6.0, i * 6.0);
            }
            const std::string p0 = "events/" + id + "/";
            for (int i = 0; i < 300 && (ses.status().pending > 0 || !st.index(p0, 0)); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            st.down = true;
            for (uint64_t i = 13; i < 25; ++i) {
                bytes[i] = seg_bytes(i);
                ses.publish_segment(bytes[i], 6.0, i * 6.0);
            }
            ses.end(std::chrono::milliseconds(100));
        }
        const std::string pfx = "events/" + id + "/";
        CHECK(st.index(pfx, 0).has_value() && !st.index(pfx, 1).has_value(),
              "End left group 1's partial index unsent (the link was down)");
        st.down = false;

        FinisherConfig fc;
        fc.spool_root = root.string();
        fc.base_backoff_ms = 1; fc.max_backoff_ms = 2; fc.jitter = 0.0;
        fc.transport = [&]() -> std::shared_ptr<Transport> {
            return std::shared_ptr<Transport>(&st, [](Transport*) {});
        };
        EventFinisher fin(fc);
        fin.run_once();
        CHECK(st.has(seg_key_of(pfx, 24)), "the finisher uploaded the backlog");

        auto g1 = st.index(pfx, 1), g2 = st.index(pfx, 2);
        bool g1_full = g1 && g1->members.size() == 10;
        if (g1_full)
            for (uint64_t s = 10; s < 20; ++s)
                if (!g1->member(s) || g1->member(s)->checksum != sha256_hex(bytes[s])) g1_full = false;
        CHECK(g1_full, "group 1's index now lists all of 10..19: End's three and the finisher's seven");
        CHECK(g2 && g2->members.size() == 5 && g2->member(20) && g2->member(24),
              "the late group 2 has an index listing 20..24");
        CHECK(st.last_index_of(group_parity_key(pfx, 1)) < st.last_index_of(group_index_key(pfx, 1)) &&
              st.last_index_of(group_parity_key(pfx, 2)) < st.last_index_of(group_index_key(pfx, 2)),
              "each group's last parity was stored before its last index");
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.objects.erase(seg_key_of(pfx, 11));   // one End sent
        }
        auto r11 = rebuild_from(st, pfx, 11, 10);
        CHECK(r11 && *r11 == bytes[11],
              "a segment End sent is rebuilt from the finisher's fuller parity");
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.objects[seg_key_of(pfx, 11)] = bytes[11];
            st.objects.erase(seg_key_of(pfx, 16));   // one the finisher sent
        }
        auto r16 = rebuild_from(st, pfx, 16, 10);
        CHECK(r16 && *r16 == bytes[16], "a segment the finisher sent is rebuilt, byte for byte");
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.objects.erase(seg_key_of(pfx, 22));
        }
        auto r22 = rebuild_from(st, pfx, 22, 10);
        CHECK(r22 && *r22 == bytes[22], "a segment of the late group is rebuilt, byte for byte");
        CHECK(!fs::exists(root.string() + ".objects/" + id),
              "the event's object queue is drained and its folder removed");
    }

    std::printf("== 7b. Resume refuses what would not be a superset ==\n");
    {
        ParityAccumulator a(10);
        std::vector<ParityAccumulator::Finished> whole;
        for (uint64_t s = 10; s < 13; ++s) a.add(s, seg_bytes(s), sha256_hex(seg_bytes(s)));
        auto part = a.flush();
        CHECK(part.has_value(), "a partial group to carry on");
        ParityAccumulator b(10);
        CHECK(!b.resume(part->index, part->parity, 12),
              "not when the stored group already names a segment still to come");
        CHECK(!b.resume(part->index, std::vector<uint8_t>(part->parity.begin(), part->parity.end() - 1), 13),
              "not when the parity does not match the index");
        CHECK(!b.resume(part->index, part->parity, 20), "not from another group");
        CHECK(b.resume(part->index, part->parity, 13), "but from the next segment, yes");
        // Carried on to 19, it must equal the group built in one go.
        ParityAccumulator c(10);
        for (uint64_t s = 10; s < 20; ++s) whole = c.add(s, seg_bytes(s), sha256_hex(seg_bytes(s)));
        ParityAccumulator d(10);
        d.resume(part->index, part->parity, 13);
        std::vector<ParityAccumulator::Finished> out;
        for (uint64_t s = 13; s < 20; ++s) { auto f = d.add(s, seg_bytes(s), sha256_hex(seg_bytes(s))); for (auto& x : f) out.push_back(std::move(x)); }
        CHECK(out.size() == 1 && whole.size() == 1 && out[0].parity == whole[0].parity &&
              out[0].index.to_json() == whole[0].index.to_json(),
              "a resumed group closes identical to one built in one go");
    }

    std::printf("== 8. The second bucket gets the groups, and never holds up the first ==\n");
    {
        Store primary;
        StickyStore mirror;
        mirror.down = true;   // the second bucket is out from the start
        const fs::path root = base / "mir";
        SessionConfig cfg;
        cfg.room_id = "r";
        cfg.spool_dir = root.string();
        cfg.parity_group = 10;
        cfg.mirror_transport = &mirror;
        cfg.base_backoff_ms = 1; cfg.max_backoff_ms = 2; cfg.backoff_jitter = 0.0;
        std::string id;
        {
            Session ses(cfg, primary);
            CHECK(ses.start_new(seg_bytes(999), video, tracks), "start_new");
            id = ses.event_id();
            for (uint64_t i = 0; i < 25; ++i) ses.publish_segment(seg_bytes(i), 6.0, i * 6.0);
            const std::string pfx = "events/" + id + "/";
            for (int i = 0; i < 300 && !primary.index(pfx, 1); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(primary.index(pfx, 0) && primary.index(pfx, 1),
                  "the first bucket has groups 0 and 1 while the second is out");
            mirror.down = false;
            for (int i = 0; i < 300 && !mirror.index(pfx, 1); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(mirror.index(pfx, 0) && mirror.index(pfx, 1) &&
                  mirror.has(group_parity_key(pfx, 0)) && mirror.has(group_parity_key(pfx, 1)),
                  "once it is back, the second bucket has them too, parity and index");
            CHECK(mirror.index_of(group_parity_key(pfx, 1)) < mirror.index_of(group_index_key(pfx, 1)),
                  "parity before index there as well");
            ses.end();
        }
        const std::string pfx = "events/" + id + "/";
        CHECK(primary.index(pfx, 2).has_value(), "End publishes the partial group to the first bucket");
        {
            // The next Go Live sends what End left for the second bucket.
            Session next(cfg, primary);
            CHECK(next.start_new(seg_bytes(999), video, tracks), "the next Go Live");
            for (int i = 0; i < 300 && !mirror.index(pfx, 2); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(mirror.index(pfx, 2).has_value() && mirror.has(group_parity_key(pfx, 2)),
                  "and the second bucket gets End's partial group at the next Go Live");
            next.end();
        }
    }

    fs::remove_all(base);
    std::printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
