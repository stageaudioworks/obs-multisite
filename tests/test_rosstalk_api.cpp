// SPDX-License-Identifier: GPL-3.0-or-later
// test_rosstalk_api.cpp — the RossTalk parts of the player's JSON API, over a
// real loopback socket: the status document's "rosstalk" object and the
// bind/port rules PUT /api/config applies.
#include "../src/appliance/api.h"
#include "../src/appliance/audio_output.h"
#include "../src/appliance/video_output.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::string request(int port, const std::string& method, const std::string& path,
                           const std::string& body = "") {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return ""; }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    ::send(fd, req.data(), req.size(), 0);
    std::string out; char buf[4096]; ssize_t n;
    while ((n = ::recv(fd, buf, sizeof buf, 0)) > 0) out.append(buf, (size_t)n);
    ::close(fd);
    auto p = out.find("\r\n\r\n");
    return p == std::string::npos ? out : out.substr(p + 4);
}

static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    NullVideoOutput vout; NullAudioOutput aout;
    Player player(Config(), vout, aout);
    RossTalkState rt;
    rt.enabled = true; rt.bind = "127.0.0.1"; rt.port = 7790; rt.error = "port in use";
    rt.last_text = "CC CUE Sermon"; rt.last_at_ms = 1234; rt.last_result = "no such marker";

    std::unique_ptr<multisite::HttpServer> server;
    int port = 0;
    for (int candidate = 18861; candidate < 18881 && !server; ++candidate) {
        auto s = std::make_unique<multisite::HttpServer>("127.0.0.1", candidate);
        register_api(*s, player, "", {}, [&rt] { return rt; });
        std::string err;
        if (s->start(err)) { server = std::move(s); port = candidate; }
    }
    CHECK(server != nullptr, "the API server bound a loopback port");
    if (!server) return 1;

    {
        const std::string b = request(port, "GET", "/api/status");
        CHECK(has(b, "\"rosstalk\":{"), "status has a rosstalk object");
        CHECK(has(b, "\"enabled\":true") && has(b, "\"bind\":\"127.0.0.1\"") &&
              has(b, "\"port\":7790") && has(b, "\"error\":\"port in use\""),
              "rosstalk: enabled, bind, port, error");
        CHECK(has(b, "\"text\":\"CC CUE Sermon\"") && has(b, "\"at_ms\":1234") &&
              has(b, "\"result\":\"no such marker\""), "rosstalk.last: text, at_ms, result");
    }
    {
        const std::string b = request(port, "PUT", "/api/config",
            "{\"rosstalk_enabled\":true,\"rosstalk_bind\":\"evil.example\",\"rosstalk_port\":70000}");
        CHECK(has(b, "\"rosstalk_bind\":\"127.0.0.1\"") && has(b, "\"rosstalk_port\":7790") &&
              has(b, "\"rosstalk_enabled\":true"),
              "PUT config: a bad bind and port fall back; enabled is kept");
    }
    {
        const std::string b = request(port, "PUT", "/api/config",
            "{\"rosstalk_bind\":\"0.0.0.0\",\"rosstalk_port\":7791}");
        CHECK(has(b, "\"rosstalk_bind\":\"0.0.0.0\"") && has(b, "\"rosstalk_port\":7791"),
              "PUT config: 0.0.0.0 and a good port are accepted");
        const std::string z = request(port, "PUT", "/api/config", "{\"rosstalk_port\":0}");
        CHECK(has(z, "\"rosstalk_port\":7790"), "PUT config: port 0 falls back to 7790");
    }
    server->stop();
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail ? 1 : 0;
}
