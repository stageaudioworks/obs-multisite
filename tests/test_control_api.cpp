// SPDX-License-Identifier: GPL-3.0-or-later
// test_control_api.cpp — the command names, and that they are one list.
//
// §8.3 promises "one API to learn and document, two ways in": the plugin's
// HTTP routes and its obs-websocket vendor requests are the same commands. That
// promise holds only while both are built from this one list, which is why the
// list lives in the portable core — and why it can be checked here, with no
// libobs and no running OBS.
//
// This is a snapshot guard. It does not care that a name was chosen; it cares
// that changing one is a deliberate edit to this file rather than a rename that
// silently orphans a client's button.
#include "../src/core/control_api.h"

#ifdef MULTISITE_WITH_PLAYER
#  include "../src/appliance/api.h"
#  include "../src/appliance/audio_output.h"
#  include "../src/appliance/video_output.h"
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstdio>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {

std::vector<std::string> collect(const char* const* names, std::size_t n) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < n; ++i) out.push_back(names[i]);
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& e : v) if (e == s) return true;
    return false;
}

// A name is a path tail: no leading or trailing slash, no whitespace, and
// nothing that would make an awkward vendor request type.
bool well_formed(const std::string& s) {
    if (s.empty()) return false;
    if (s.front() == '/' || s.back() == '/') return false;
    if (s.find("//") != std::string::npos) return false;
    for (char c : s)
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
    return true;
}

#ifdef MULTISITE_WITH_PLAYER
// GET /api/config over loopback; returns the whole response text.
std::string get_config(int port, const char* method = "GET") {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); return ""; }
    const std::string body = std::string(method) == "PUT" ? "{}" : "";
    const std::string req = std::string(method) + " /api/config HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    ::send(fd, req.data(), req.size(), 0);
    std::string out;
    char buf[4096];
    for (long n; (n = ::recv(fd, buf, sizeof(buf), 0)) > 0;) out.append(buf, (size_t)n);
    ::close(fd);
    return out;
}

// What /api/config says about the box, for a given set of capabilities.
std::string config_with(const multisite_player::ApiCapabilities& caps,
                        const char* method = "GET") {
    multisite_player::NullVideoOutput video;
    multisite_player::NullAudioOutput audio;
    multisite_player::Player player(multisite_player::Config{}, video, audio);
    for (int port = 38400; port < 38440; ++port) {
        HttpServer server("127.0.0.1", port);
        multisite_player::register_api(server, player, "/tmp/test_control_api_config.json", caps);
        std::string err;
        if (!server.start(err)) continue;
        const std::string body = get_config(port, method);
        server.stop();
        return body;
    }
    return "";
}
#endif

} // namespace

int main() {
    const auto enc = collect(kEncoderCommands, std::size(kEncoderCommands));
    const auto dec = collect(kDecoderCommands, std::size(kDecoderCommands));

    std::printf("control api:\n");

    // ── The exact surface, so a rename is a deliberate act ──────────────────
    const std::vector<std::string> want_enc = {
        "encoder/status", "encoder/go-live", "encoder/end",
        "encoder/marker", "encoder/settings",
    };
    const std::vector<std::string> want_dec = {
        "decoder/status", "decoder/play", "decoder/stop", "decoder/hold",
        "decoder/continue", "decoder/catch-up", "decoder/jog", "decoder/seek",
        "decoder/delay", "decoder/marker", "decoder/cue", "decoder/events",
        "decoder/events/refresh", "decoder/load-event",
        "decoder/return-to-live", "decoder/settings",
    };

    CHECK(enc.size() == want_enc.size(), "encoder command count is as expected");
    CHECK(dec.size() == want_dec.size(), "decoder command count is as expected");
    for (const auto& n : want_enc)
        CHECK(contains(enc, n), ("encoder command present: " + n).c_str());
    for (const auto& n : want_dec)
        CHECK(contains(dec, n), ("decoder command present: " + n).c_str());

    // ── No duplicates, and no name on both sides ────────────────────────────
    // Both halves sit under one obs-websocket vendor, so a name used twice
    // would mean one request silently standing in for two.
    std::set<std::string> seen;
    bool dup = false;
    for (const auto& n : enc) if (!seen.insert(n).second) dup = true;
    for (const auto& n : dec) if (!seen.insert(n).second) dup = true;
    CHECK(!dup, "no command name is used twice across the whole surface");

    // ── Every name is a path tail the router can take ───────────────────────
    bool shaped = true;
    for (const auto& n : enc) if (!well_formed(n)) shaped = false;
    for (const auto& n : dec) if (!well_formed(n)) shaped = false;
    CHECK(shaped, "every name is a well-formed path tail");

    // ── The one path builder, so the HTTP route and the vendor request agree ─
    CHECK(api_path("decoder/hold") == "/api/decoder/hold",
          "api_path prefixes exactly once");
    CHECK(api_path("") == "/api/",
          "api_path of nothing is still a path");
    CHECK(api_path("encoder/go-live") == "/api/encoder/go-live",
          "api_path leaves the name intact");

#ifdef MULTISITE_WITH_PLAYER
    // ── The settings page learns what this box has ──────────────────────────
    const std::string all = config_with({});
    CHECK(all.find("\"capabilities\":{\"aes67\":true,\"all_tracks\":false,\"auto_screen\":true,"
                   "\"clock\":true,\"power\":true,\"remote_access\":true,"
                   "\"sd_card\":true}") != std::string::npos,
          "default capabilities are all true (the Pi, unchanged)");
    const std::string none = config_with({false, false, false, false, false, false});
    CHECK(none.find("\"capabilities\":{\"aes67\":false,\"all_tracks\":false,\"auto_screen\":false,"
                    "\"clock\":false,\"power\":false,\"remote_access\":false,"
                    "\"sd_card\":false}") != std::string::npos,
          "cleared capabilities are all false");
    CHECK(all.find("\"all_tracks\":false") != std::string::npos,
          "all_tracks is off by default (the Pi has no wide output)");
    multisite_player::ApiCapabilities wide;
    wide.all_tracks = true;
    CHECK(config_with(wide).find("\"all_tracks\":true") != std::string::npos,
          "all_tracks can be turned on");
    const std::string saved = config_with(wide, "PUT");
    CHECK(saved.find("200 OK") != std::string::npos &&
          saved.find("\"capabilities\":{") != std::string::npos &&
          saved.find("\"all_tracks\":true") != std::string::npos,
          "saving settings answers with the capabilities too");
#endif

    if (g_fail) {
        std::printf("control api: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("control api: OK\n");
    return 0;
}
