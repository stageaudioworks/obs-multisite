// SPDX-License-Identifier: GPL-3.0-or-later
// test_mirror_verify.cpp — is the second copy really a copy?
//
// Driven entirely through the two targets' manifests, so it is offline and
// cheap: two requests, not two buckets.
#include "../src/core/mirror_verify.h"
#include "../src/core/model.h"   // Manifest, event_prefix_for

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {

class FakeStore : public Transport {
public:
    void put_manifest(const std::string& event_id,
                      std::vector<std::pair<uint64_t, std::string>> segs) {
        Manifest m;
        m.event_id = event_id;
        m.init = "init.mp4";
        m.status = "ended";
        for (auto& [seq, sum] : segs) {
            ManifestSegment s;
            s.seq = seq;
            s.duration_s = 6.0;
            s.checksum = sum;
            m.push(s, 50);
        }
        const std::string body = m.to_json();
        m_objects[event_prefix_for(event_id) + "manifest.json"] =
            std::vector<uint8_t>(body.begin(), body.end());
    }

    PutResult put(const std::string&, const std::vector<uint8_t>&,
                  const std::string&,
                  const std::map<std::string, std::string>&) override {
        PutResult r; r.success = true; r.http_status = 200; return r;
    }

    GetResult get(const std::string& key) override {
        GetResult r;
        auto it = m_objects.find(key);
        if (it == m_objects.end()) {
            r.http_status = 404;
            r.error = "NoSuchKey";
            return r;
        }
        r.success = true;
        r.http_status = 200;
        r.body = it->second;
        return r;
    }

private:
    std::map<std::string, std::vector<uint8_t>> m_objects;
};

const char* EV = "01EVENTAAAA";

} // namespace

int main() {
    std::printf("== 1. Identical targets come back complete ==\n");
    {
        FakeStore a, b;
        a.put_manifest(EV, {{1, "aa"}, {2, "bb"}, {3, "cc"}});
        b.put_manifest(EV, {{1, "aa"}, {2, "bb"}, {3, "cc"}});
        MirrorDiff d = compare_targets(a, b, EV);
        CHECK(d.ok() && d.complete, "no differences, and said so");
        CHECK(d.total() == 0, "with nothing listed");
    }

    std::printf("== 2. A segment the second bucket never got ==\n");
    {
        FakeStore a, b;
        a.put_manifest(EV, {{1, "aa"}, {2, "bb"}, {3, "cc"}});
        b.put_manifest(EV, {{1, "aa"}, {2, "bb"}});
        MirrorDiff d = compare_targets(a, b, EV);
        CHECK(d.ok(), "both manifests were readable");
        CHECK(!d.complete, "so this is NOT a complete copy");
        CHECK(d.only_primary.size() == 1 && d.only_primary[0] == 3,
              "and the missing segment is named, not merely counted");
    }

    std::printf("== 3. A segment only the second bucket has ==\n");
    {
        // The failover case: after the encoder moved its writes, the second
        // bucket is the one that carried on.
        FakeStore a, b;
        a.put_manifest(EV, {{1, "aa"}, {2, "bb"}});
        b.put_manifest(EV, {{1, "aa"}, {2, "bb"}, {3, "cc"}, {4, "dd"}});
        MirrorDiff d = compare_targets(a, b, EV);
        CHECK(d.only_second.size() == 2, "both later segments are named");
        CHECK(!d.complete, "and the copies are not the same");
    }

    std::printf("== 4. Present in both, but different — the one that matters most ==\n");
    {
        FakeStore a, b;
        a.put_manifest(EV, {{1, "aa"}, {2, "bb"}});
        b.put_manifest(EV, {{1, "aa"}, {2, "CORRUPTED"}});
        MirrorDiff d = compare_targets(a, b, EV);
        CHECK(d.checksum_mismatch.size() == 1 && d.checksum_mismatch[0] == 2,
              "a differing checksum is reported as a mismatch, not as a miss");
        CHECK(!d.complete, "which is not a complete copy either");
    }

    std::printf("== 5. An unreadable manifest is itself, not a content difference ==\n");
    {
        FakeStore a, b;
        a.put_manifest(EV, {{1, "aa"}});
        // b has no manifest at all.
        MirrorDiff d = compare_targets(a, b, EV);
        CHECK(!d.ok(), "this is an error, not a difference");
        CHECK(d.error.rfind("second:", 0) == 0,
              "and it names which side failed");
        CHECK(d.total() == 0, "with no invented differences");
    }

    std::printf("== 6. Records with no checksum are not claimed as verified ==\n");
    {
        FakeStore a, b;
        a.put_manifest(EV, {{1, ""}});
        b.put_manifest(EV, {{1, ""}});
        MirrorDiff d = compare_targets(a, b, EV);
        CHECK(d.ok(), "both readable");
        CHECK(d.checksum_mismatch.empty(),
              "empty-vs-empty is not reported as a mismatch — there is nothing "
              "to compare, so nothing is claimed");
    }

    std::printf("\n%s\n", g_fail == 0 ? "MIRROR VERIFY TESTS PASSED"
                                      : "MIRROR VERIFY TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
