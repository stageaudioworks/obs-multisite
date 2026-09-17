// SPDX-License-Identifier: GPL-3.0-or-later
// test_cues.cpp — the pieces a shared cue list rests on: one cue object per
// author, a stable token for a site name, and a deterministic merge. All pure
// functions, so no OBS, no Qt, no bucket and no network.
#include "../src/core/model.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    // ── Site name -> object key ─────────────────────────────────────────────
    CHECK(cue_author_token("Campus B") == "campus-b",
          "a plain name lowercases and hyphenates");
    CHECK(cue_author_token("Main site") == "main-site",
          "spaces become single hyphens");
    CHECK(cue_author_token("Campus B (north)") == "campus-b-north",
          "brackets are dropped, not turned into dashes");
    CHECK(cue_author_token("St. Mary's") == "st-marys",
          "punctuation is dropped");
    CHECK(cue_author_token("North/South") == "north-south",
          "a slash is a separator, not a path");
    CHECK(cue_author_token("") == "site",
          "an empty name still yields a key");
    CHECK(cue_author_token("   ---  ") == "site",
          "a name of only separators yields the fallback");

    CHECK(cues_prefix_for("E1") == "events/E1/cues/",
          "cues live under the event prefix");
    CHECK(cue_object_key("E1", "Campus B") == "events/E1/cues/campus-b.json",
          "one cue object per author");

    // ── Author survives the round trip ──────────────────────────────────────
    MarkerList a;
    a.markers.push_back(Marker{100, 1000, "cue", "Sermon", "id-a1", "Main site"});
    auto a2 = MarkerList::from_json(a.to_json());
    CHECK(a2.markers.size() == 1 && a2.markers[0].author == "Main site",
          "a cue's author survives serialization");

    // A document written before authors existed carries none, and must read.
    auto old = MarkerList::from_json(
        "{\"markers\":[{\"seq\":1,\"at_ms\":10,\"type\":\"cue\","
        "\"label\":\"x\",\"id\":\"o1\"}]}");
    CHECK(old.markers.size() == 1 && old.markers[0].author.empty(),
          "a cue with no author reads as the main site");

    // ── Merge: one list, oldest first, no duplicates ────────────────────────
    MarkerList enc;   // the encoder: two cues
    enc.markers.push_back(Marker{100, 1000, "cue", "Sermon", "id-e1", ""});
    enc.markers.push_back(Marker{200, 2000, "cue", "Offering", "id-e2", ""});
    MarkerList sat;   // a satellite: one cue in between, plus a duplicate
    sat.markers.push_back(Marker{150, 1500, "cue", "Notice", "id-s1", "Campus B"});
    sat.markers.push_back(enc.markers[0]);   // the same cue seen twice

    auto merged = merge_markers({ sat, enc });
    CHECK(merged.markers.size() == 3, "a cue seen in two lists is kept once");
    const bool ordered = merged.markers.size() == 3 &&
                         merged.markers[0].id == "id-e1" &&
                         merged.markers[1].id == "id-s1" &&
                         merged.markers[2].id == "id-e2";
    CHECK(ordered, "merged cues are oldest first regardless of source order");
    CHECK(merged.markers.size() == 3 && merged.markers[1].author == "Campus B",
          "the satellite's cue keeps its author through the merge");

    // Same millisecond: order by seq, then id, so it is stable between reads.
    MarkerList t1;
    t1.markers.push_back(Marker{2, 500, "cue", "b", "id-b", ""});
    t1.markers.push_back(Marker{1, 500, "cue", "a", "id-a", ""});
    MarkerList t2;
    t2.markers.push_back(Marker{1, 500, "cue", "a", "id-a", ""});
    auto tm = merge_markers({ t1, t2 });
    CHECK(tm.markers.size() == 2 &&
          tm.markers[0].id == "id-a" && tm.markers[1].id == "id-b",
          "cues sharing a millisecond order by seq, once each");

    // A box whose clock is badly out must not drag its cue to the wrong place:
    // ordering follows the EVENT's position (seq), not anybody's wall clock.
    MarkerList on_time;   // dropped later in the event, correct clock
    on_time.markers.push_back(Marker{ 200, 200000, "cue", "second", "id-2", "" });
    MarkerList skewed;    // dropped earlier in the event, clock a day fast
    skewed.markers.push_back(Marker{ 150, 86400000 + 150000, "cue", "first",
                                     "id-1", "Campus B" });
    auto fixed = merge_markers({ on_time, skewed });
    CHECK(fixed.markers.size() == 2 && fixed.markers[0].id == "id-1",
          "a skewed clock cannot reorder a cue: seq decides, not at_ms");

    std::printf("\n%s\n", g_fail == 0 ? "CUE TESTS PASSED" : "CUE TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
