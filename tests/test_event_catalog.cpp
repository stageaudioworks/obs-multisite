// SPDX-License-Identifier: GPL-3.0-or-later
// test_event_catalog.cpp — turning a bucket into a list an operator can choose
// from.
//
// The classification is the point. A list of event IDs is useless; what matters
// is which one is on air, which are finished recordings, and which were cut off
// mid-event by an encoder that died. That last state is the one the protocol
// previously had no way to express: an interrupted event stays marked "live"
// forever, and before this it could never be played back at all — precisely
// when you would most want to watch it again.
#include "../src/core/event_catalog.h"

#include <cstdio>
#include <map>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// An in-memory store with a listing that behaves like ListObjectsV2: ascending
// key order, delimiter grouping, and a page size the caller does not control.
class MemStore : public Transport {
public:
    std::map<std::string, std::string> objects;
    int  page_size = 1000;
    bool listing_allowed = true;      // simulates a key without s3:ListBucket

    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string,std::string>&) override {
        objects[key] = std::string(body.begin(), body.end());
        PutResult r; r.success = true; r.http_status = 200; return r;
    }

    GetResult get(const std::string& key) override {
        GetResult r;
        auto it = objects.find(key);
        if (it == objects.end()) { r.http_status = 404; r.error = "not found"; return r; }
        r.success = true; r.http_status = 200;
        r.body.assign(it->second.begin(), it->second.end());
        return r;
    }

    ListResult list(const std::string& prefix, const std::string& delimiter,
                    const std::string& token, int) override {
        ListResult r;
        if (!listing_allowed) {
            r.http_status = 403;
            r.error = "AccessDenied — listing requires the s3:ListBucket permission";
            r.retryable = false;
            return r;
        }
        // std::map iterates in ascending key order, exactly as S3 lists.
        size_t start = token.empty() ? 0 : (size_t)std::stoul(token);
        size_t seen = 0, emitted = 0;
        for (const auto& [key, _] : objects) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            if (seen++ < start) continue;
            if ((int)emitted >= page_size) {
                r.truncated = true;
                r.next_continuation_token = std::to_string(seen - 1);
                break;
            }
            if (!delimiter.empty()) {
                size_t d = key.find(delimiter, prefix.size());
                if (d != std::string::npos) {
                    std::string cp = key.substr(0, d + delimiter.size());
                    if (r.common_prefixes.empty() || r.common_prefixes.back() != cp)
                        r.common_prefixes.push_back(cp);
                    ++emitted;
                    continue;
                }
            }
            ListEntry e; e.key = key; e.size = (int64_t)objects.at(key).size();
            r.keys.push_back(e);
            ++emitted;
        }
        r.success = true; r.http_status = 200;
        return r;
    }
};

// Write an event: its descriptor, its room-index entry, and a manifest in the
// given state.
static void make_event(MemStore& s, const std::string& id, const std::string& room,
                       int64_t started_ms, const std::string& status,
                       int64_t updated_ms, bool with_index = true,
                       const std::string& name = "",
                       const std::string& vcodec = "h264") {
    EventInfo ev;
    ev.event_id = id; ev.room_id = room; ev.started_at_ms = started_ms;
    ev.name = name;
    ev.video.codec = vcodec;
    s.objects[event_prefix_for(id) + "event.json"] = ev.to_json();

    if (with_index) {
        RoomEventEntry ix;
        ix.event_id = id; ix.room_id = room; ix.started_at_ms = started_ms;
        ix.name = name;
        s.objects[room_event_key(room, id)] = ix.to_json();
    }

    Manifest m;
    m.event_id = id; m.status = status;
    m.name = name;
    m.video.codec = vcodec;
    m.started_at_ms = started_ms; m.updated_at_ms = updated_ms;
    m.latest_seq = 100; m.first_available_seq = 0;
    ManifestSegment seg;
    seg.seq = 100; seg.duration_s = 6.0; seg.at_ms = started_ms + 600 * 1000;
    m.segments.push_back(seg);
    s.objects[event_prefix_for(id) + "manifest.json"] = m.to_json();
}

static void set_live_pointer(MemStore& s, const std::string& room,
                             const std::string& id, int64_t updated_ms) {
    LivePointer lp;
    lp.room_id = room; lp.event_id = id; lp.status = "live";
    lp.updated_at_ms = updated_ms;
    s.objects[live_pointer_key(room)] = lp.to_json();
}

int main() {
    const int64_t NOW = 1'757'000'000'000LL;   // a fixed "now" for every case
    const int64_t MIN = 60'000;

    // ── The three states ─────────────────────────────────────────────────────
    std::printf("Classifying events\n");
    {
        MemStore s;
        // Oldest: finished cleanly two days ago.
        make_event(s, "01AAA", "main", NOW - 2 * 24 * 60 * MIN, "ended",
                   NOW - 2 * 24 * 60 * MIN + 60 * MIN);
        // Yesterday: the encoder died. Still says "live", nothing since.
        make_event(s, "01BBB", "main", NOW - 24 * 60 * MIN, "live",
                   NOW - 23 * 60 * MIN);
        // Now: on air, manifest updating.
        make_event(s, "01CCC", "main", NOW - 20 * MIN, "live", NOW - 4000);
        set_live_pointer(s, "main", "01CCC", NOW - 4000);

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        CHECK(cat.refresh(NOW), "refresh succeeds");

        auto ev = cat.events();
        CHECK(ev.size() == 3, "all three events listed");
        if (ev.size() == 3) {
            CHECK(ev[0].event_id == "01CCC" && ev[0].state == EventState::Live,
                  "the live event is first and classified live");
            CHECK(ev[0].is_live_pointer, "the live event is the one live.json names");
            CHECK(ev[1].event_id == "01BBB" && ev[1].state == EventState::Interrupted,
                  "a stale 'live' event is INTERRUPTED, not live and not lost");
            CHECK(ev[2].event_id == "01AAA" && ev[2].state == EventState::Recording,
                  "a cleanly-ended event is a recording");
            CHECK(ev[1].started_at_ms > ev[2].started_at_ms,
                  "past events are newest-first");
            CHECK(ev[2].duration_s > 599.0 && ev[2].duration_s < 607.0,
                  "programme length comes from the manifest's own segment times");
        }
        CHECK(!cat.used_fallback_scan(), "the room index is used, not the fallback scan");
        CHECK(cat.skipped() == 0, "nothing skipped");
    }

    // ── The event name surfaces from the manifest ─────────────────────────────
    std::printf("Event names\n");
    {
        MemStore s;
        make_event(s, "01NAMED", "main", NOW - 60 * MIN, "ended", NOW - 30 * MIN,
                   true, "Harvest Festival");
        make_event(s, "01UNNAMED", "main", NOW - 120 * MIN, "ended", NOW - 90 * MIN);

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        CHECK(cat.refresh(NOW), "refresh succeeds");
        auto ev = cat.events();
        CHECK(ev.size() == 2, "both events listed");
        if (ev.size() == 2) {
            CHECK(ev[0].event_id == "01NAMED" && ev[0].name == "Harvest Festival",
                  "a named event surfaces its name");
            CHECK(ev[1].event_id == "01UNNAMED" && ev[1].name.empty(),
                  "an unnamed event has an empty name (UI falls back to the time)");
        }
    }

    // ── The codec does not decide whether a recording lists ─────────────────
    // An AV1 event played live and then never appeared in the list, which is
    // what sent somebody looking in here. Nothing in this file knows one codec
    // from another — the codec lives in the manifest and only the decoder cares
    // — and the point of the case below is that it stays that way. If a codec
    // ever does start deciding, this is where it will show up.
    std::printf("An AV1 recording\n");
    {
        MemStore s;
        make_event(s, "01AV1AAAAA", "main", NOW - 30 * MIN, "ended", NOW - 25 * MIN,
                   true, "AV1 test service", "av1");
        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        CHECK(cat.refresh(NOW), "the listing refreshes with an AV1 event in it");
        auto ev = cat.events();
        CHECK(ev.size() == 1, "an ended AV1 event is listed");
        CHECK(!ev.empty() && ev[0].state == EventState::Recording,
              "and classified as a recording, exactly as another codec would be");
        CHECK(!ev.empty() && ev[0].name == "AV1 test service",
              "with its name, from the manifest the encoder wrote");
        CHECK(cat.skipped() == 0,
              "nothing was quietly skipped for being a codec this build dislikes");
        CHECK(cat.last_error().empty(),
              "and nothing was reported as unplayable");
    }
    {
        // The same event still on air: AV1 must not change which state wins
        // either, or an AV1 room would never show as live.
        MemStore s;
        set_live_pointer(s, "main", "01AV1BBBBB", NOW - 1000);
        make_event(s, "01AV1BBBBB", "main", NOW - 40 * MIN, "live", NOW - 1000,
                   true, "AV1 on air", "av1");
        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        cat.refresh(NOW);
        auto ev = cat.events();
        CHECK(ev.size() == 1 && ev[0].state == EventState::Live,
              "a live AV1 event is still the live one");
        CHECK(ev[0].is_live_pointer, "and is known to be the one the pointer names");
    }

    // ── A room with no live pointer at all ───────────────────────────────────
    std::printf("A room that is not on air\n");
    {
        MemStore s;
        make_event(s, "01AAA", "main", NOW - 60 * MIN, "ended", NOW - 30 * MIN);
        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        CHECK(cat.refresh(NOW), "a room with no live.json still lists its recordings");
        CHECK(cat.events().size() == 1, "the past event is listed");
        CHECK(!cat.events().empty() && !cat.events()[0].is_live_pointer,
              "and is not claimed to be live");
    }

    // ── Rooms ────────────────────────────────────────────────────────────────
    std::printf("Room separation\n");
    {
        MemStore s;
        make_event(s, "01AAA", "main",    NOW - 60 * MIN, "ended", NOW - 30 * MIN);
        make_event(s, "01BBB", "chapel",  NOW - 50 * MIN, "ended", NOW - 20 * MIN);
        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        cat.refresh(NOW);
        CHECK(cat.events().size() == 1, "only this room's events are listed");
        CHECK(!cat.events().empty() && cat.events()[0].event_id == "01AAA",
              "and it is the right one");
    }

    // ── The compatibility path ───────────────────────────────────────────────
    // Everything recorded before the room index existed — which is every
    // recording in the bucket today.
    std::printf("Events recorded before the room index existed\n");
    {
        MemStore s;
        make_event(s, "01AAA", "main",   NOW - 60 * MIN, "ended", NOW - 30 * MIN, false);
        make_event(s, "01BBB", "chapel", NOW - 50 * MIN, "ended", NOW - 20 * MIN, false);
        make_event(s, "01CCC", "main",   NOW - 40 * MIN, "ended", NOW - 10 * MIN, false);

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        CHECK(cat.refresh(NOW), "un-indexed events are still found");
        CHECK(cat.used_fallback_scan(), "and the fallback scan is reported as used");
        auto ev = cat.events();
        CHECK(ev.size() == 2, "both of this room's events found by reading event.json");
        CHECK(ev.size() == 2 && ev[0].event_id == "01CCC" && ev[1].event_id == "01AAA",
              "newest first, other rooms excluded");
    }

    // ── A bucket part-way through the change ─────────────────────────────────
    // The case a real bucket is actually in: events recorded before the room
    // index existed, and one recorded since. Treating a non-empty index as the
    // whole truth hid every older event the moment one indexed event
    // appeared — the operator saw the newest recording and nothing else, while
    // the rest sat in storage, playable, and unlisted.
    std::printf("Indexed and un-indexed events in one bucket\n");
    {
        MemStore s;
        make_event(s, "01AAA", "main",   NOW - 90 * MIN, "ended", NOW - 80 * MIN, false);
        make_event(s, "01BBB", "main",   NOW - 60 * MIN, "ended", NOW - 50 * MIN, false);
        make_event(s, "01CCC", "chapel", NOW - 45 * MIN, "ended", NOW - 40 * MIN, false);
        // The only one with an index entry.
        make_event(s, "01DDD", "main",   NOW - 20 * MIN, "ended", NOW - 10 * MIN, true);

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        CHECK(cat.refresh(NOW), "refresh succeeds");
        auto ev = cat.events();
        CHECK(ev.size() == 3,
              "an indexed event does not hide the un-indexed ones");
        CHECK(ev.size() == 3 && ev[0].event_id == "01DDD" &&
              ev[1].event_id == "01BBB" && ev[2].event_id == "01AAA",
              "all three of this room's events, newest first");
        CHECK(cat.used_fallback_scan(),
              "the scan is reported, since it is what found the older ones");
        CHECK(cat.skipped() == 0, "nothing skipped");
    }

    // An indexed event whose room differs from the one being listed must not
    // leak in: the index is room-scoped by key, the scan by descriptor, and
    // both paths have to agree.
    std::printf("Room separation across both discovery paths\n");
    {
        MemStore s;
        make_event(s, "01AAA", "chapel", NOW - 60 * MIN, "ended", NOW - 50 * MIN, false);
        make_event(s, "01BBB", "chapel", NOW - 40 * MIN, "ended", NOW - 30 * MIN, true);
        make_event(s, "01CCC", "main",   NOW - 20 * MIN, "ended", NOW - 10 * MIN, true);

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        CHECK(cat.refresh(NOW), "refresh succeeds");
        CHECK(cat.events().size() == 1 && cat.events()[0].event_id == "01CCC",
              "another room's events are excluded whether indexed or not");
        CHECK(!cat.used_fallback_scan(),
              "and a scan that found nothing for this room is not reported");
    }

    // ── Pagination ───────────────────────────────────────────────────────────
    std::printf("Pagination\n");
    {
        MemStore s;
        s.page_size = 2;                       // force several pages
        for (int i = 0; i < 7; ++i) {
            char id[16]; std::snprintf(id, sizeof(id), "01E%03d", i);
            make_event(s, id, "main", NOW - (7 - i) * 60 * MIN, "ended",
                       NOW - (7 - i) * 60 * MIN + 30 * MIN);
        }
        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        cat.refresh(NOW);
        CHECK(cat.events().size() == 7,
              "every page is followed, not just the first");
    }

    // ── Failures ─────────────────────────────────────────────────────────────
    std::printf("Failure handling\n");
    {
        MemStore s;
        make_event(s, "01AAA", "main", NOW - 60 * MIN, "ended", NOW - 30 * MIN);
        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        cat.refresh(NOW);
        CHECK(cat.events().size() == 1, "listed once");

        // A key that cannot list must not look like a room with no recordings.
        s.listing_allowed = false;
        CHECK(!cat.refresh(NOW), "a listing failure is reported as a failure");
        CHECK(cat.last_error().find("ListBucket") != std::string::npos,
              "and says the permission is the problem");
        CHECK(cat.events().size() == 1,
              "the list already on screen is kept rather than blanked");
    }
    {
        // Retention can remove an event's objects between the listing and the
        // manifest fetch. Such an event is left out rather than offered as
        // something that cannot play.
        MemStore s;
        make_event(s, "01AAA", "main", NOW - 60 * MIN, "ended", NOW - 30 * MIN);
        make_event(s, "01BBB", "main", NOW - 50 * MIN, "ended", NOW - 20 * MIN);
        s.objects.erase(event_prefix_for("01BBB") + "manifest.json");

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        cat.refresh(NOW);
        CHECK(cat.events().size() == 1, "an unreadable event is not offered");
        CHECK(cat.skipped() == 1, "but it is counted, so the list is not silently short");
    }

    // ── Events that never recorded anything ──────────────────────────────────
    // A go-live that failed immediately leaves a manifest with no start time
    // and no segments. It appeared in the list as "(unknown time) 0 min": a row
    // that cannot be played and says nothing.
    {
        MemStore s;
        make_event(s, "01AAA", "main", NOW - 60 * MIN, "ended", NOW - 30 * MIN);

        Manifest empty;
        empty.event_id = "01BBB"; empty.status = "ended";
        empty.started_at_ms = 0; empty.updated_at_ms = NOW - 40 * MIN;
        empty.latest_seq = 0; empty.first_available_seq = 0;
        s.objects[event_prefix_for("01BBB") + "manifest.json"] = empty.to_json();
        RoomEventEntry ix;
        ix.event_id = "01BBB"; ix.room_id = "main"; ix.started_at_ms = 0;
        s.objects[room_event_key("main", "01BBB")] = ix.to_json();

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        cat.refresh(NOW);
        CHECK(cat.events().size() == 1, "an event that recorded nothing is not listed");
        CHECK(!cat.events().empty() && cat.events()[0].event_id == "01AAA",
              "the real recording is still there");
        CHECK(cat.skipped() == 1, "and it is counted rather than silently dropped");
    }
    {
        // …but an event that has only just gone live has no segments YET, and
        // must not be filtered out on that basis.
        MemStore s;
        Manifest fresh;
        fresh.event_id = "01CCC"; fresh.status = "live";
        fresh.started_at_ms = 0;            // not yet written through
        fresh.updated_at_ms = NOW - 2000;
        s.objects[event_prefix_for("01CCC") + "manifest.json"] = fresh.to_json();
        RoomEventEntry ix;
        ix.event_id = "01CCC"; ix.room_id = "main";
        s.objects[room_event_key("main", "01CCC")] = ix.to_json();
        set_live_pointer(s, "main", "01CCC", NOW - 2000);

        CatalogConfig cfg; cfg.room_id = "main";
        EventCatalog cat(cfg, s);
        cat.refresh(NOW);
        CHECK(cat.events().size() == 1,
              "an event that just went live is kept, empty though it is");
    }

    // ── The stale threshold ──────────────────────────────────────────────────
    std::printf("The live/interrupted boundary\n");
    {
        MemStore s;
        CatalogConfig cfg; cfg.room_id = "main"; cfg.stale_after_ms = 600000;

        // Just inside the threshold: a quiet encoder, not a dead one.
        make_event(s, "01AAA", "main", NOW - 60 * MIN, "live", NOW - 9 * MIN);
        EventCatalog c1(cfg, s); c1.refresh(NOW);
        CHECK(!c1.events().empty() && c1.events()[0].state == EventState::Live,
              "a manifest updated 9 minutes ago is still live");

        // Just past it.
        s.objects.clear();
        make_event(s, "01AAA", "main", NOW - 60 * MIN, "live", NOW - 11 * MIN);
        EventCatalog c2(cfg, s); c2.refresh(NOW);
        CHECK(!c2.events().empty() && c2.events()[0].state == EventState::Interrupted,
              "at 11 minutes it is interrupted");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL CATALOG TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
