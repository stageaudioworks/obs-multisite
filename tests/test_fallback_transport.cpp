// SPDX-License-Identifier: GPL-3.0-or-later
// test_fallback_transport.cpp — the decoder's "prefer LAN, fall back to
// cloud" composite (PROJECT-SCOPE.md §8.7, "Preference and fallback"), tested
// against two small in-memory mocks rather than a real network: the fallback
// decision itself is what's under test, not either transport's own I/O.
#include "../src/core/fallback_transport.h"

#include <cstdio>
#include <map>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {

// Answers a fixed set of keys with fixed bodies; anything else is a 404.
// Counts requests so a test can prove which transport actually answered,
// not just which one COULD have.
class FakeStore : public Transport {
public:
    explicit FakeStore(std::string tag) : m_tag(std::move(tag)) {}

    void put_object(const std::string& key, std::string body) {
        m_objects[key] = std::move(body);
    }

    GetResult get(const std::string& key) override {
        ++requests;
        GetResult r;
        auto it = m_objects.find(key);
        if (it == m_objects.end()) {
            r.success = false;
            r.http_status = 404;
            r.error = m_tag + ": not found";
            return r;
        }
        r.success = true;
        r.http_status = 200;
        r.body.assign(it->second.begin(), it->second.end());
        return r;
    }

    PutResult put(const std::string&, const std::vector<uint8_t>&,
                 const std::string&, const std::map<std::string, std::string>&) override {
        PutResult r; r.success = true; r.http_status = 200; return r;
    }

    int requests = 0;

private:
    std::string m_tag;
    std::map<std::string, std::string> m_objects;
};

} // namespace

int main() {
    std::printf("LAN answers — cloud is never even asked\n");
    {
        FakeStore lan("lan"), cloud("cloud");
        lan.put_object("events/E1/manifest.json", "{\"from\":\"lan\"}");
        cloud.put_object("events/E1/manifest.json", "{\"from\":\"cloud\"}");
        FallbackTransport ft(lan, cloud);

        auto r = ft.get("events/E1/manifest.json");
        CHECK(r.success, "get() succeeds");
        CHECK(r.body.size() > 0 &&
              std::string(r.body.begin(), r.body.end()).find("lan") != std::string::npos,
              "the body came from LAN, not cloud");
        CHECK(cloud.requests == 0, "cloud was never asked — LAN answered first try");
        CHECK(ft.last_get_was_primary(), "the dock's active-path indicator says LAN");
    }

    std::printf("LAN 404s this key (aged out of its retention window) — "
                "cloud answers for THIS request only\n");
    {
        FakeStore lan("lan"), cloud("cloud");
        cloud.put_object("events/E1/segments/00000000.m4s", "old segment bytes");
        FallbackTransport ft(lan, cloud);

        auto r = ft.get("events/E1/segments/00000000.m4s");
        CHECK(r.success, "get() succeeds via the fallback");
        CHECK(std::string(r.body.begin(), r.body.end()) == "old segment bytes",
              "the body came from cloud");
        CHECK(!ft.last_get_was_primary(), "the indicator says cloud for this one");

        // And the very next request, for a key LAN DOES have, goes back to
        // LAN — this is per-request, not a sticky "switched to cloud" state.
        lan.put_object("events/E1/segments/00000005.m4s", "new segment bytes");
        auto r2 = ft.get("events/E1/segments/00000005.m4s");
        CHECK(ft.last_get_was_primary(),
              "back to LAN immediately — no session-wide fallback flag to get stuck");
    }

    std::printf("Neither has it — a clean miss, not a crash\n");
    {
        FakeStore lan("lan"), cloud("cloud");
        FallbackTransport ft(lan, cloud);
        auto r = ft.get("events/E1/segments/00000099.m4s");
        CHECK(!r.success, "reports failure");
        CHECK(r.http_status == 404, "cloud's own 404, surfaced honestly");
    }

    std::printf("Writes, listing and deletes always go to cloud — LAN is "
                "read-only and has no listing concept at all\n");
    {
        FakeStore lan("lan"), cloud("cloud");
        FallbackTransport ft(lan, cloud);
        auto r = ft.put("events/E1/segments/00000000.m4s", {1,2,3}, "video/mp4", {});
        CHECK(r.success, "put() reaches cloud and succeeds");
        CHECK(lan.requests == 0, "LAN's get() was never even touched by a put()");
    }

    std::printf("\n%s\n", g_fail == 0
        ? "ALL FALLBACK-TRANSPORT TESTS PASSED"
        : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
