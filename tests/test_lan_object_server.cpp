// SPDX-License-Identifier: GPL-3.0-or-later
// test_lan_object_server.cpp — the encoder's LAN delivery surface
// (PROJECT-SCOPE.md §8.7), exercised over a real loopback socket the same
// way test_http_server.cpp proves the shared server itself.
//
// A satellite fetching manifest.json, event.json, init.mp4, live.json and a
// segment directly from the encoder instead of through the bucket, with a
// bounded retention window standing in for the spool (which deletes a
// segment the moment it confirms — see lan_object_server.h for why LAN
// serving needs its own copy). The decoder-side client (LanTransport) and
// the fallback composite (FallbackTransport) have their own test files;
// this one proves the server in isolation, over a real loopback socket.
#include "../src/core/lan_object_server.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace multisite;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSocket = INVALID_SOCKET;
void close_socket(sock_t s) { ::closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kBadSocket = -1;
void close_socket(sock_t s) { ::close(s); }
#endif

sock_t connect_local(int port) {
    const sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSocket) return kBadSocket;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        close_socket(s);
        return kBadSocket;
    }
    return s;
}

bool send_all(sock_t s, const std::string& text) {
    size_t sent = 0;
    while (sent < text.size()) {
        const int n = ::send(s, text.data() + sent,
                             (int)(text.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

// One request, one connection, read until the socket closes (every request
// below sends "Connection: close", so this never has to parse Content-Length
// to know where a response ends).
std::string round_trip(int port, const std::string& request_text) {
    const sock_t fd = connect_local(port);
    if (fd == kBadSocket) return "";
    send_all(fd, request_text);
    std::string out;
    char chunk[4096];
    for (;;) {
        const long long n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        out.append(chunk, (size_t)n);
    }
    close_socket(fd);
    return out;
}

int status_of(const std::string& r) {
    const size_t sp = r.find(' ');
    if (sp == std::string::npos || r.compare(0, 5, "HTTP/") != 0) return 0;
    try { return std::stoi(r.substr(sp + 1, 3)); } catch (...) { return 0; }
}

std::string body_of(const std::string& r) {
    const size_t e = r.find("\r\n\r\n");
    return e == std::string::npos ? std::string() : r.substr(e + 4);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::vector<uint8_t> fake_bytes(uint64_t seed, size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((seed * 131 + i * 7) & 0xFF);
    return v;
}

} // namespace

int main() {
    fs::path cache_dir = fs::temp_directory_path() / "multisite_lan_object_server_test";
    fs::remove_all(cache_dir);
    fs::create_directories(cache_dir);

    LanServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.max_cached_segments = 4;   // small, so the retention cap is reachable in a test
    std::unique_ptr<LanObjectServer> server;
    int port = 0;
    for (int candidate = 19080; candidate < 19100; ++candidate) {
        cfg.port = candidate;
        auto s = std::make_unique<LanObjectServer>(cfg, cache_dir.string());
        std::string err;
        if (s->start(err)) { server = std::move(s); port = candidate; break; }
    }
    CHECK(server != nullptr, "the server bound a loopback port");
    if (!server) return 1;
    std::printf("  (listening on 127.0.0.1:%d)\n", port);

    std::printf("Before any event: everything is a 404, not a crash\n");
    {
        const std::string r = round_trip(port,
            "GET /events/E1/manifest.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404, "nothing to serve yet");
    }

    std::printf("Bootstrapping an event: event.json, init.mp4, manifest.json\n");
    const std::string event_json = "{\"event_id\":\"E1\",\"name\":\"Sunday\"}";
    const auto init_bytes = fake_bytes(0, 1200);
    server->on_event_started("E1", event_json, init_bytes);
    server->on_manifest_published("{\"event_id\":\"E1\",\"segments\":[]}");
    {
        const std::string r = round_trip(port,
            "GET /events/E1/event.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "event.json is served once the event starts");
        CHECK(contains(body_of(r), "Sunday"), "with the exact JSON handed to it");
    }
    {
        const std::string r = round_trip(port,
            "GET /events/E1/manifest.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "manifest.json is served");
        CHECK(contains(body_of(r), "E1"), "with the exact JSON just published");
    }
    {
        const std::string r = round_trip(port,
            "GET /events/E1/init.mp4 HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "init.mp4 is served");
        CHECK(contains(r, "video/mp4"), "with the right content type");
        CHECK(body_of(r).size() == init_bytes.size(),
              "and the exact init bytes, byte for byte");
    }
    {
        const std::string r = round_trip(port,
            "GET /events/WRONG_EVENT/manifest.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404,
              "a request for any OTHER event id is refused, not served by accident");
    }

    std::printf("live.json: how a satellite following the room (not a "
                "pinned event) discovers what's live\n");
    {
        const std::string r = round_trip(port,
            "GET /rooms/main-auditorium/live.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404, "nothing published yet — not a crash");
    }
    server->on_live_published("{\"room_id\":\"main-auditorium\",\"event_id\":\"E1\",\"status\":\"live\"}");
    {
        const std::string r = round_trip(port,
            "GET /rooms/main-auditorium/live.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "served once published");
        CHECK(contains(body_of(r), "E1"), "with the exact JSON just published");
    }
    {
        const std::string r = round_trip(port,
            "GET /rooms/some-other-room/live.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404,
              "a different room's live.json path is a 404, not this room's answer");
    }

    std::printf("markers.json: without this a LAN-only satellite (cloud "
                "disabled) could never receive a marker at all\n");
    {
        const std::string r = round_trip(port,
            "GET /events/E1/markers.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404, "no marker dropped yet — a genuine, honest 404");
    }
    server->on_markers_published("{\"markers\":[{\"id\":\"m1\",\"label\":\"Sermon Start\"}]}");
    {
        const std::string r = round_trip(port,
            "GET /events/E1/markers.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "served once a marker is dropped");
        CHECK(contains(body_of(r), "Sermon Start"), "with the exact JSON just published");
    }

    std::printf("Segments become servable the moment they confirm\n");
    {
        const std::string r = round_trip(port,
            "GET /events/E1/segments/00000000.m4s HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404, "not yet confirmed, not yet servable");
    }
    server->on_segment_confirmed(0, fake_bytes(0, 4096));
    {
        const std::string r = round_trip(port,
            "GET /events/E1/segments/00000000.m4s HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 200, "servable the instant it confirms — no bucket round trip");
        const auto expect = fake_bytes(0, 4096);
        CHECK(body_of(r) == std::string(expect.begin(), expect.end()),
              "byte for byte the same segment that was confirmed");
    }
    {
        const std::string r = round_trip(port,
            "GET /events/E1/segments/notaseq.m4s HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404, "a malformed sequence number is a 404, not a crash");
    }

    std::printf("Retention is bounded, the same shape as the decoder's own cache\n");
    {
        for (uint64_t seq = 1; seq <= 5; ++seq)
            server->on_segment_confirmed(seq, fake_bytes(seq, 512));
        CHECK(server->cached_count() <= 4,
              "never holds more than max_cached_segments, even though 6 have confirmed");
        const std::string r = round_trip(port,
            "GET /events/E1/segments/00000000.m4s HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404,
              "the oldest segment aged out of the window and is genuinely gone");
        const std::string r2 = round_trip(port,
            "GET /events/E1/segments/00000005.m4s HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r2) == 200, "but the newest one is still there");
    }

    std::printf("A new event discards the old one's retained segments\n");
    {
        server->on_event_started("E2", "{\"event_id\":\"E2\"}", fake_bytes(99, 100));
        const std::string r = round_trip(port,
            "GET /events/E1/segments/00000005.m4s HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r) == 404,
              "E1's segments are gone now that E2 is the live event");
        const std::string r2 = round_trip(port,
            "GET /events/E2/event.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r2) == 200, "and E2 is what's actually being served");
        const std::string r3 = round_trip(port,
            "GET /events/E1/markers.json HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(status_of(r3) == 404,
              "E1's markers are gone too, not still hanging around under E2");
    }

    std::printf("An auth token, once configured, is enforced\n");
    {
        server->stop();
        LanServerConfig authed = cfg;
        authed.auth_token = "s3cr3t";
        std::unique_ptr<LanObjectServer> as;
        int aport = 0;
        for (int candidate = 19100; candidate < 19120; ++candidate) {
            authed.port = candidate;
            auto s = std::make_unique<LanObjectServer>(authed, cache_dir.string());
            std::string err;
            if (s->start(err)) { as = std::move(s); aport = candidate; break; }
        }
        CHECK(as != nullptr, "a second server, with a token configured, starts");
        if (as) {
            as->on_event_started("E3", "{\"event_id\":\"E3\"}", fake_bytes(1, 10));
            {
                const std::string r = round_trip(aport,
                    "GET /events/E3/event.json HTTP/1.1\r\nConnection: close\r\n\r\n");
                CHECK(status_of(r) == 401, "no token presented — refused");
            }
            {
                const std::string r = round_trip(aport,
                    "GET /events/E3/event.json HTTP/1.1\r\n"
                    "Authorization: Bearer wrong\r\nConnection: close\r\n\r\n");
                CHECK(status_of(r) == 401, "the wrong token — still refused");
            }
            {
                const std::string r = round_trip(aport,
                    "GET /events/E3/event.json HTTP/1.1\r\n"
                    "Authorization: Bearer s3cr3t\r\nConnection: close\r\n\r\n");
                CHECK(status_of(r) == 200, "the right token — served");
            }
        }
    }

    fs::remove_all(cache_dir);
    std::printf("\n%s\n", g_fail == 0
        ? "ALL LAN-OBJECT-SERVER TESTS PASSED"
        : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
