// SPDX-License-Identifier: GPL-3.0-or-later
// test_lan_transport.cpp — the decoder's LAN client (PROJECT-SCOPE.md §8.7),
// proven against a REAL LanObjectServer over a real loopback socket rather
// than a mock: what's under test is that LanTransport's key-to-URL mapping
// actually lines up with what LanObjectServer actually serves, which a mock
// on either side could get wrong in a way that still passed.
//
// cancel_pending()'s "abort a stalled transfer promptly" guarantee is not
// re-proven here: it is the identical curl technique test_s3_cancel.cpp
// already proves against a deliberately-stalling raw listener, just applied
// to this class instead of S3Transport.
#include "../src/core/lan_transport.h"
#include "../src/core/lan_object_server.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

using namespace multisite;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    fs::path cache_dir = fs::temp_directory_path() / "multisite_lan_transport_test";
    fs::remove_all(cache_dir);
    fs::create_directories(cache_dir);

    LanServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.room_id = "main-auditorium";
    std::unique_ptr<LanObjectServer> server;
    int port = 0;
    for (int candidate = 19200; candidate < 19220; ++candidate) {
        cfg.port = candidate;
        auto s = std::make_unique<LanObjectServer>(cfg, cache_dir.string());
        std::string err;
        if (s->start(err)) { server = std::move(s); port = candidate; break; }
    }
    CHECK(server != nullptr, "the server bound a loopback port");
    if (!server) return 1;

    server->on_live_published(
        "{\"room_id\":\"main-auditorium\",\"event_id\":\"E1\",\"status\":\"live\"}");
    server->on_event_started("E1", "{\"event_id\":\"E1\",\"name\":\"Sunday\"}",
                             { 'i','n','i','t' });
    server->on_manifest_published("{\"event_id\":\"E1\",\"segments\":[]}");
    server->on_segment_confirmed(0, { 'a','b','c','d' });
    server->on_markers_published("{\"markers\":[{\"id\":\"m1\",\"label\":\"Sermon Start\"}]}");

    LanTransportConfig tcfg;
    tcfg.host = "127.0.0.1";
    tcfg.port = port;
    LanTransport tx(tcfg);

    std::printf("The key shapes DecoderSession actually asks for all resolve "
                "correctly against a real server\n");
    {
        auto r = tx.get("rooms/main-auditorium/live.json");
        CHECK(r.success, "live.json — how a satellite discovers what's live");
        CHECK(tx.last_request_reached_server(), "the LAN path itself is alive");
    }
    {
        auto r = tx.get("events/E1/event.json");
        CHECK(r.success, "event.json");
    }
    {
        auto r = tx.get("events/E1/manifest.json");
        CHECK(r.success, "manifest.json");
    }
    {
        auto r = tx.get("events/E1/init.mp4");
        CHECK(r.success, "init.mp4");
        CHECK(std::string(r.body.begin(), r.body.end()) == "init",
              "the exact init bytes");
    }
    {
        auto r = tx.get("events/E1/segments/00000000.m4s");
        CHECK(r.success, "a confirmed segment");
        CHECK(std::string(r.body.begin(), r.body.end()) == "abcd",
              "byte for byte");
    }
    {
        auto r = tx.get("events/E1/markers.json");
        CHECK(r.success, "markers.json — without this a LAN-only satellite "
                        "(cloud disabled) could never receive a marker at all");
        CHECK(std::string(r.body.begin(), r.body.end()).find("Sermon Start")
                  != std::string::npos, "the exact marker just published");
    }

    std::printf("A genuine miss is a plain 404, and the LAN path is still "
                "reported reachable\n");
    {
        auto r = tx.get("events/E1/segments/00000099.m4s");
        CHECK(!r.success, "not confirmed — not found");
        CHECK(r.http_status == 404, "a real 404, not a connection failure");
        CHECK(tx.last_request_reached_server(),
              "the server answered — this is a miss, not the LAN path being down");
    }

    std::printf("Event listing is the one thing genuinely not a LAN "
                "concept — a plain 404, which is exactly what makes "
                "FallbackTransport fall through to cloud for it\n");
    {
        auto r = tx.get("rooms/main-auditorium/events/some-listing-key");
        CHECK(!r.success && r.http_status == 404,
              "event browsing (§7.5) is inherently cloud-only, by design");
    }

    std::printf("An unreachable host reports a connection failure, not a "
                "false success — this is what tells a satellite the LAN "
                "path itself is down\n");
    {
        LanTransportConfig deadcfg;
        deadcfg.host = "127.0.0.1";
        deadcfg.port = port + 500;   // nothing listening here
        deadcfg.connect_timeout_ms = 300;
        LanTransport dead(deadcfg);
        auto r = dead.get("events/E1/manifest.json");
        CHECK(!r.success, "fails");
        CHECK(!dead.last_request_reached_server(),
              "correctly distinguished from a 404 — nothing answered at all");
    }

    std::printf("An auth token, once configured, is required and honoured\n");
    {
        server->stop();
        LanServerConfig authed = cfg;
        authed.auth_token = "s3cr3t";
        std::unique_ptr<LanObjectServer> as;
        int aport = 0;
        for (int candidate = 19220; candidate < 19240; ++candidate) {
            authed.port = candidate;
            auto s = std::make_unique<LanObjectServer>(authed, cache_dir.string());
            std::string err;
            if (s->start(err)) { as = std::move(s); aport = candidate; break; }
        }
        CHECK(as != nullptr, "a second server, with a token configured, starts");
        if (as) {
            as->on_event_started("E2", "{\"event_id\":\"E2\"}", { 'x' });
            LanTransportConfig noauth;
            noauth.host = "127.0.0.1"; noauth.port = aport;
            LanTransport notoken(noauth);
            auto r = notoken.get("events/E2/event.json");
            CHECK(!r.success && r.http_status == 401, "refused with no token");

            LanTransportConfig withauth = noauth;
            withauth.auth_token = "s3cr3t";
            LanTransport withtoken(withauth);
            auto r2 = withtoken.get("events/E2/event.json");
            CHECK(r2.success, "served once the right token is presented");
        }
    }

    fs::remove_all(cache_dir);
    std::printf("\n%s\n", g_fail == 0
        ? "ALL LAN-TRANSPORT TESTS PASSED"
        : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
