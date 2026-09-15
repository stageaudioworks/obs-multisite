// SPDX-License-Identifier: GPL-3.0-or-later
// test_storage_manager.cpp — encoder-side storage management.
//
// Proves the dangerous part in isolation: which events are listed, that
// deleting one removes every object under events/{id}/ plus its room-index
// entry while leaving live.json alone, that the live event is refused, and
// that "older than N days" only touches the events it should.
//
// It also pins the two halves apart, because that is what the window depends
// on: the events must list without any size work (an operator should not wait
// minutes for a window to fill), a size that could not be measured must be
// reported as unknown rather than as zero, and both a cancelled listing and a
// prefix that will not finish paging must end rather than run for ever.
#include "../src/core/storage_manager.h"
#include "../src/core/model.h"

#include <cstdio>
#include <cstdint>
#include <atomic>
#include <map>
#include <string>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// An in-memory store that supports put/get/list/remove, enough for the manager.
class MemStore : public Transport {
public:
    std::map<std::string, std::string> objects;

    // Faults the tests ask for: a store that refuses every listing, and one
    // that will never finish paging a particular prefix.
    bool        list_fails = false;
    std::string endless_prefix;

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
        if (list_fails) {
            r.http_status = 403;
            r.error = "HTTP 403";
            r.retryable = false;
            return r;
        }
        if (!endless_prefix.empty() &&
            prefix.compare(0, endless_prefix.size(), endless_prefix) == 0) {
            r.success = true; r.http_status = 200;
            r.truncated = true;                      // and never advances
            r.next_continuation_token = "same-token";
            return r;
        }
        size_t start = token.empty() ? 0 : (size_t)std::stoul(token);
        size_t seen = 0;
        for (const auto& [key, val] : objects) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            if (seen++ < start) continue;
            if (!delimiter.empty()) {
                size_t d = key.find(delimiter, prefix.size());
                if (d != std::string::npos) {
                    std::string cp = key.substr(0, d + delimiter.size());
                    if (r.common_prefixes.empty() || r.common_prefixes.back() != cp)
                        r.common_prefixes.push_back(cp);
                    continue;
                }
            }
            ListEntry e; e.key = key; e.size = (int64_t)val.size();
            r.keys.push_back(e);
        }
        r.success = true; r.http_status = 200;
        return r;
    }
    DeleteResult remove(const std::string& key) override {
        DeleteResult r;
        auto it = objects.find(key);
        if (it == objects.end()) { r.success = true; r.http_status = 404; return r; }
        objects.erase(it);
        r.success = true; r.http_status = 204;
        return r;
    }
};

static void make_event(MemStore& s, const std::string& room, const std::string& id,
                       int64_t started_ms, const std::string& status,
                       int n_segments, const std::string& name) {
    EventInfo ev;
    ev.event_id = id; ev.room_id = room; ev.started_at_ms = started_ms; ev.name = name;
    s.objects[event_prefix_for(id) + "event.json"] = ev.to_json();

    RoomEventEntry ix;
    ix.event_id = id; ix.room_id = room; ix.started_at_ms = started_ms; ix.name = name;
    s.objects[room_event_key(room, id)] = ix.to_json();

    Manifest m;
    m.event_id = id; m.status = status; m.name = name;
    m.started_at_ms = started_ms; m.updated_at_ms = started_ms;
    m.latest_seq = (uint64_t)(n_segments > 0 ? n_segments - 1 : 0);
    ManifestSegment seg; seg.seq = 0; seg.duration_s = 6.0; seg.at_ms = started_ms;
    m.segments.push_back(seg);
    s.objects[event_prefix_for(id) + "manifest.json"] = m.to_json();

    s.objects[event_prefix_for(id) + "init.mp4"] = std::string(1200, 'i');
    for (int i = 0; i < n_segments; ++i) {
        char buf[16]; std::snprintf(buf, sizeof(buf), "%08d", i);
        s.objects[event_prefix_for(id) + "segments/" + buf + ".m4s"] =
            std::string(4096, 's');
    }
}

// A real ULID-shaped id: ten characters of Crockford base32 milliseconds, then
// filler. The short ids used elsewhere in this file ("01AAA") deliberately are
// NOT ULIDs, so they exercise the "cannot be dated from the id" path.
static std::string ulid_for(int64_t ms) {
    static const char kA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string out(10, '0');
    for (int i = 9; i >= 0; --i) { out[i] = kA[ms % 32]; ms /= 32; }
    return out + "ZZZZZZZZZZZZZZZZ";
}

static void set_live(MemStore& s, const std::string& room, const std::string& id) {
    LivePointer lp;
    lp.room_id = room; lp.event_id = id; lp.status = "live"; lp.updated_at_ms = 0;
    s.objects[live_pointer_key(room)] = lp.to_json();
}

int main() {
    const int64_t NOW = 1'757'000'000'000LL;
    const int64_t DAY = 24LL * 60 * 60 * 1000;
    const std::string room = "main";

    std::printf("== 1. Lists the room's events with sizes and live status ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW - 2 * DAY, "ended", 3, "Alpha");
        make_event(s, room, "01BBB", NOW - DAY, "ended", 2, "Beta");
        make_event(s, room, "01CCC", NOW, "live", 1, "Gamma");
        set_live(s, room, "01CCC");

        StorageManager mgr(room, s);
        std::vector<ManagedEvent> evs;
        std::string err;
        CHECK(mgr.list(evs, err), "list succeeds");
        CHECK(evs.size() == 3, "all three events listed");
        if (evs.size() == 3) {
            CHECK(evs[0].event_id == "01CCC" && evs[0].is_live,
                  "the live event is first and marked live");
            CHECK(evs[0].name == "Gamma", "the live event carries its name");
            CHECK(evs[0].objects == 4 && evs[0].bytes > 0,
                  "sizes tallied (descriptor + init + manifest + 1 segment)");
            CHECK(evs[2].event_id == "01AAA" && !evs[2].is_live,
                  "a finished event is listed and not live");
        }
    }

    std::printf("== 2. Delete removes the event and its index entry ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW - 2 * DAY, "ended", 3, "Alpha");
        make_event(s, room, "01CCC", NOW, "live", 1, "Gamma");
        set_live(s, room, "01CCC");

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_event("01AAA");
        CHECK(r.ok, "delete succeeds");
        CHECK(r.objects_deleted == 7, "all 6 media objects plus the index entry removed");
        CHECK(r.confirmed, "verification re-list confirmed the prefix is empty");
        CHECK(s.objects.find("events/01AAA/init.mp4") == s.objects.end(),
              "event media is gone");
        CHECK(s.objects.find("rooms/main/events/01AAA.json") == s.objects.end(),
              "the room index entry is gone");
        CHECK(s.objects.find("rooms/main/live.json") != s.objects.end(),
              "live.json is left alone");
        CHECK(s.objects.find("events/01CCC/init.mp4") != s.objects.end(),
              "the other event is untouched");
    }

    std::printf("== 3. The live event is refused ==\n");
    {
        MemStore s;
        make_event(s, room, "01CCC", NOW, "live", 1, "Gamma");
        set_live(s, room, "01CCC");

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_event("01CCC");
        CHECK(!r.ok, "deleting the live event is refused");
        CHECK(r.error.find("on air") != std::string::npos, "and says why");
        CHECK(s.objects.find("events/01CCC/init.mp4") != s.objects.end(),
              "nothing was actually deleted");
    }

    std::printf("== 4. Older-than-N deletes only the old events ==\n");
    {
        MemStore s;
        make_event(s, room, "01OLD", NOW - 10 * DAY, "ended", 2, "Old");
        make_event(s, room, "01RECENT", NOW - 3 * DAY, "ended", 2, "Recent");
        make_event(s, room, "01LIVE", NOW, "live", 1, "Live");
        set_live(s, room, "01LIVE");

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_older_than(7, NOW);
        CHECK(r.ok, "cleanup succeeds");
        CHECK(r.objects_deleted == 6, "the old event (5 objects + index) was removed");
        CHECK(s.objects.find("events/01OLD/init.mp4") == s.objects.end(),
              "the old event is gone");
        CHECK(s.objects.find("events/01RECENT/init.mp4") != s.objects.end(),
              "a recent event inside the window is kept");
        CHECK(s.objects.find("events/01LIVE/init.mp4") != s.objects.end(),
              "the live event is kept");
    }

    std::printf("== 5. The events list without any size work ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW - 2 * DAY, "ended", 3, "Alpha");
        make_event(s, room, "01CCC", NOW, "live", 1, "Gamma");
        set_live(s, room, "01CCC");

        StorageManager mgr(room, s);
        std::vector<ManagedEvent> evs;
        std::string err;
        ListStats st;
        CHECK(mgr.list_events(evs, err, &st), "list_events succeeds");
        CHECK(evs.size() == 2, "both events listed");
        CHECK(st.events == 2, "and the stats say so");
        if (evs.size() == 2) {
            CHECK(evs[0].event_id == "01CCC" && evs[0].is_live,
                  "the live event is first");
            CHECK(evs[0].name == "Gamma",
                  "names arrive without touching a single segment");
            CHECK(!evs[0].size_known,
                  "and nothing claims to have been measured yet");
        }

        // The expensive half, on demand — and it says so when it is done.
        CHECK(mgr.tally_size(evs[0], err, &st), "tally_size then succeeds");
        CHECK(evs[0].size_known, "and the event is marked as measured");
        CHECK(evs[0].objects > 0 && evs[0].bytes > 0, "with real figures");
        CHECK(st.requests > 0, "the requests it cost are counted for the log");
    }

    std::printf("== 6. A size that could not be measured is not shown as zero ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW - 2 * DAY, "ended", 3, "Alpha");

        StorageManager mgr(room, s);
        std::vector<ManagedEvent> evs;
        std::string err;
        CHECK(mgr.list_events(evs, err), "the event still lists");
        CHECK(evs.size() == 1, "one event");

        s.list_fails = true;                     // the store refuses the paging
        ListStats st;
        CHECK(!mgr.tally_size(evs[0], err, &st), "the tally fails");
        CHECK(!evs[0].size_known,
              "and it does NOT claim a size — this is what stops the window "
              "telling an operator a full event is empty");
        CHECK(!evs[0].size_error.empty(), "the reason is kept for the window");
        CHECK(st.tallies_failed == 1, "and counted as a failure, not a success");
        CHECK(err.find("403") != std::string::npos, "the store's own words survive");
    }

    std::printf("== 7. A cancelled listing stops ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW, "ended", 1, "A");

        StorageManager mgr(room, s);
        std::vector<ManagedEvent> evs;
        std::string err;
        ListStats st;
        std::atomic<bool> cancel{false};
        CHECK(mgr.list_events(evs, err, &st, &cancel), "an open gate lists normally");

        cancel = true;                            // the window was closed
        CHECK(!mgr.tally_size(evs[0], err, &st, &cancel),
              "a cancelled tally returns at once");
        CHECK(err == "cancelled", "saying so rather than blaming the store");
        CHECK(st.tallies_failed == 0,
              "and without counting it as a store failure");

        MemStore s2;
        make_event(s2, room, "01BBB", NOW, "ended", 1, "B");
        StorageManager mgr2(room, s2);
        std::atomic<bool> closed{true};
        ListStats st2;
        CHECK(!mgr2.list(evs, err, &st2, &closed), "a closed gate lists nothing");
        CHECK(st2.cancelled, "and the caller is told it was cancelled");
    }

    std::printf("== 8. A prefix that will not finish paging gives up ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW, "ended", 2, "A");
        s.endless_prefix = event_prefix_for("01AAA");

        StorageManager mgr(room, s);
        std::vector<ManagedEvent> evs;
        std::string err;
        CHECK(mgr.list_events(evs, err), "the event lists (the scan is cheap)");

        ListStats st;
        CHECK(!mgr.tally_size(evs[0], err, &st), "the tally gives up");
        CHECK(err.find("did not finish") != std::string::npos, "and says why");
        CHECK(!evs[0].size_known, "so the size reads as unknown, not as zero");

        // The same guard covers the catalog's own two loops: a store that
        // repeats a token for the room index must not hold the listing for ever.
        MemStore s2;
        make_event(s2, room, "01BBB", NOW, "ended", 1, "B");
        s2.endless_prefix = "rooms/";
        StorageManager mgr2(room, s2);
        CHECK(!mgr2.list_events(evs, err), "an endless room index fails");
        CHECK(err.find("did not finish") != std::string::npos, "and says why");
    }

    std::printf("== 9. A bulk delete reports confirmation when it verified ==\n");
    {
        // The report started its confirmed flag at false and ANDed into it, so
        // a run that deleted anything could never come back confirmed — the
        // window told the operator re-checking had failed after a run in which
        // every re-check passed. Nothing asserted it, so nothing caught it.
        MemStore s;
        make_event(s, room, "01OLD1", NOW - 10 * DAY, "ended", 2, "Old one");
        make_event(s, room, "01OLD2", NOW - 20 * DAY, "ended", 2, "Old two");

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_older_than(7, NOW);
        CHECK(r.ok, "the run succeeds");
        CHECK(r.events_deleted == 2, "both old events went");
        CHECK(r.confirmed, "and the run reports itself confirmed");
        CHECK(r.deleted_ids.size() == 2, "the report names what it removed");
    }

    std::printf("== 10. An event with no start time is dated from its id ==\n");
    {
        // A manifest that never recorded a start time is exactly the debris an
        // operator wants cleared, and requiring the field left those events
        // undeletable from this window for ever.
        MemStore s;
        const std::string old_id = ulid_for(NOW - 30 * DAY);
        make_event(s, room, old_id, NOW - 30 * DAY, "ended", 1, "Undated");
        // Blank the manifest's start time, keeping the id's.
        {
            Manifest m; m.event_id = old_id; m.status = "ended"; m.name = "Undated";
            m.started_at_ms = 0; m.updated_at_ms = NOW - 30 * DAY; m.latest_seq = 1;
            ManifestSegment seg; seg.seq = 0; seg.duration_s = 6.0;
            seg.at_ms = NOW - 30 * DAY;
            m.segments.push_back(seg);
            s.objects[event_prefix_for(old_id) + "manifest.json"] = m.to_json();
        }

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_older_than(7, NOW);
        CHECK(r.ok, "the run succeeds");
        CHECK(r.events_deleted == 1, "the undated-but-ULID event is deleted");
        CHECK(r.events_undated == 0, "and is not counted as undatable");
    }

    std::printf("== 11. An event that cannot be dated at all is left alone ==\n");
    {
        // "01NODATE" is not a ULID and its manifest has no start time. A cutoff
        // cannot be applied to an age nobody knows, so it stays — but the
        // report has to SAY it stayed, or "deleted 0 objects" looks like a
        // broken button.
        MemStore s;
        make_event(s, room, "01NODATE", 0, "ended", 1, "No date");
        {
            Manifest m; m.event_id = "01NODATE"; m.status = "ended";
            m.started_at_ms = 0; m.updated_at_ms = 0; m.latest_seq = 1;
            ManifestSegment seg; seg.seq = 0; seg.duration_s = 6.0; seg.at_ms = 0;
            m.segments.push_back(seg);
            s.objects[event_prefix_for("01NODATE") + "manifest.json"] = m.to_json();
        }

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_older_than(7, NOW);
        CHECK(r.ok, "the run succeeds rather than erroring");
        CHECK(r.events_deleted == 0, "nothing undatable is deleted");
        CHECK(r.events_undated == 1, "and the report says one could not be dated");
        CHECK(s.objects.find(std::string(event_prefix_for("01NODATE")) + "init.mp4")
                  != s.objects.end(), "the event is still there");
    }

    std::printf("== 12. An id decodes to a time only when that time is sane ==\n");
    {
        CHECK(event_id_time_ms(ulid_for(NOW - DAY)) > 0, "a real ULID decodes");
        CHECK(event_id_time_ms("01AAA") == 0, "something too short does not");
        CHECK(event_id_time_ms("0000000000ZZZZZZ") == 0,
              "and neither does one that decodes to 1970 — which would have "
              "read as the oldest event in the bucket and been deleted first");
        CHECK(event_id_time_ms("UUUUUUUUUUZZZZZZ") == 0,
              "a string using letters the alphabet excludes is rejected");
    }

    std::printf("== 13. Progress is reported, and cancelling stops the run ==\n");
    {
        MemStore s;
        make_event(s, room, "01OLDA", NOW - 10 * DAY, "ended", 2, "A");
        make_event(s, room, "01OLDB", NOW - 11 * DAY, "ended", 2, "B");

        StorageManager mgr(room, s);
        int calls = 0;
        uint64_t seen_count = 0;
        DeleteReport r = mgr.delete_older_than(
            7, NOW, [&](const DeleteProgress& p) {
                ++calls;
                seen_count = p.event_count;
                return calls < 3;          // cancel partway through
            });
        CHECK(calls > 0, "progress is actually reported");
        CHECK(seen_count == 2, "and it knows how many events the run covers");
        CHECK(r.cancelled, "the report says it was cancelled");
        CHECK(r.ok, "a cancel is the operator's decision, not a failure");
        CHECK(r.events_deleted < 2, "and it stopped before finishing");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL STORAGE MANAGER TESTS PASSED"
                                       : "SOME STORAGE MANAGER TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}

