// SPDX-License-Identifier: GPL-3.0-or-later
// test_mirror_read.cpp — the decoder's two-cloud read path: prefer the primary,
// reach for the second bucket only for an object the primary could not serve
// (PROJECT-SCOPE.md §10 Phase 9).
//
// Against two in-memory mocks, because the decision is what is under test, not
// either transport's I/O.
#include "../src/core/mirror_read_transport.h"

#include <cstdio>
#include <map>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {

class FakeStore : public Transport {
public:
    explicit FakeStore(std::string tag) : m_tag(std::move(tag)) {}

    void put_object(const std::string& key, const std::string& body) {
        m_objects[key] = body;
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
                  const std::string&,
                  const std::map<std::string, std::string>&) override {
        PutResult r; r.success = true; r.http_status = 200; return r;
    }

    ListResult list(const std::string&, const std::string&, const std::string&,
                    int) override {
        ++list_calls;
        ListResult r;
        r.success = reachable;
        if (!r.success) r.error = m_tag + ": unreachable";
        return r;
    }

    int64_t object_size(const std::string& key) override {
        ++size_calls;
        return has(key) ? 100 : -1;
    }

    bool has(const std::string& key) const {
        return m_objects.find(key) != m_objects.end();
    }

    // Distinguishes a genuine connection failure from an ordinary 404, which is
    // the distinction the whole class turns on.
    bool last_request_reached_server() const override { return reachable; }
    bool reachable = true;

    int requests = 0;
    int list_calls = 0;
    int size_calls = 0;

private:
    std::string m_tag;
    std::map<std::string, std::string> m_objects;
};

} // namespace

int main() {
    std::printf("== 1. The primary serves it: the second bucket is not touched ==\n");
    {
        FakeStore primary("primary"), second("second");
        primary.put_object("k", "from-primary");
        second.put_object("k", "from-second");
        MirrorReadTransport tx(primary, second);

        GetResult r = tx.get("k");
        CHECK(r.success && std::string(r.body.begin(), r.body.end()) == "from-primary",
              "the primary's copy is what comes back");
        CHECK(second.requests == 0,
              "and the second bucket was never asked — its egress costs money");
        CHECK(!tx.last_read_was_secondary(), "reported as a primary read");
    }

    std::printf("== 2. The primary is unreachable: the second serves it ==\n");
    {
        FakeStore primary("primary"), second("second");
        primary.reachable = false;
        second.put_object("k", "from-second");
        MirrorReadTransport tx(primary, second);

        GetResult r = tx.get("k");
        CHECK(r.success && std::string(r.body.begin(), r.body.end()) == "from-second",
              "the second bucket answers when the first cannot");
        CHECK(tx.last_read_was_secondary(), "and the dock can say so");
        CHECK(!tx.primary_reachable(), "the primary is reported as not answering");
    }

    std::printf("== 3. An ordinary miss is NOT the primary being down ==\n");
    {
        FakeStore primary("primary"), second("second");
        // The shape this exists for: the encoder failed over, so an event's
        // later segments only ever reached the second bucket.
        second.put_object("events/E/segments/00000005.m4s", "late-segment");
        MirrorReadTransport tx(primary, second);

        GetResult r = tx.get("events/E/segments/00000005.m4s");
        CHECK(r.success, "a segment only the second bucket has is still readable");
        CHECK(tx.last_read_was_secondary(), "and is reported as coming from there");
        CHECK(tx.primary_reachable(),
              "but the primary is NOT reported as down — it answered, it just "
              "did not have this object");
    }

    std::printf("== 4. Neither has it: the failure is the primary's ==\n");
    {
        FakeStore primary("primary"), second("second");
        MirrorReadTransport tx(primary, second);
        GetResult r = tx.get("missing");
        CHECK(!r.success, "the read fails");
        CHECK(second.requests == 1, "after both were genuinely tried");
    }

    std::printf("== 5. Listing and sizing fall back the same way ==\n");
    {
        FakeStore primary("primary"), second("second");
        primary.reachable = false;
        MirrorReadTransport tx(primary, second);

        CHECK(tx.list("events/").success, "a failed list falls back");
        CHECK(primary.list_calls == 1 && second.list_calls == 1,
              "the second was asked only after the first failed");

        second.put_object("only-here", "x");
        CHECK(tx.object_size("only-here") == 100,
              "an object only the second bucket has still reports its size");
    }

    std::printf("== 6. A reachable primary is listed first even when it misses ==\n");
    {
        FakeStore primary("primary"), second("second");
        primary.put_object("a", "1");
        MirrorReadTransport tx(primary, second);
        tx.get("a");
        CHECK(tx.primary_reachable(), "healthy after a hit");
        tx.get("nope");   // 404 from a reachable primary, then the second's 404
        CHECK(tx.primary_reachable(), "and still healthy after a 404");
    }

    std::printf("== 7. Told to prefer the second bucket, it goes first ==\n");
    {
        FakeStore primary("primary"), second("second");
        primary.put_object("k", "from-primary");
        second.put_object("k", "from-second");
        MirrorReadTransport tx(primary, second);

        CHECK(!tx.preferring_secondary(), "it starts on the primary");
        tx.prefer_secondary(true);
        CHECK(tx.preferring_secondary(), "and can be moved to the other end");

        GetResult r = tx.get("k");
        CHECK(r.success && std::string(r.body.begin(), r.body.end()) == "from-second",
              "the second bucket is now asked first");
        CHECK(tx.last_read_was_secondary(), "reported as a secondary read");

        // A failover that turns out to be the wrong call must degrade into the
        // old behaviour, not into an unreadable event.
        FakeStore p3("primary"), s3("second");
        p3.put_object("only-primary", "x");
        MirrorReadTransport tx3(p3, s3);
        tx3.prefer_secondary(true);
        GetResult r3 = tx3.get("only-primary");
        CHECK(r3.success,
              "and an object the second bucket lacks still comes from the primary");
        CHECK(!tx3.last_read_was_secondary(), "reported as a primary read");
    }

    std::printf("\n%s\n", g_fail == 0 ? "MIRROR READ TESTS PASSED"
                                      : "MIRROR READ TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
