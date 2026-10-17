// SPDX-License-Identifier: GPL-3.0-or-later
// test_rosstalk_listener.cpp — the RossTalk listener over a real socket.
#include "../src/appliance/rosstalk_listener.h"
#include "../src/appliance/audio_output.h"
#include "../src/appliance/video_output.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

using namespace multisite_player;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

static int connect_to(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return -1; }
    return fd;
}

// A Player and a listener on an OS-chosen port; connect() once it is bound.
struct Rig {
    NullVideoOutput vout; NullAudioOutput aout;
    Player player;
    RossTalkListener listener;
    explicit Rig(int port = 0, bool enabled = true)
        : player(make_cfg(port, enabled), vout, aout), listener(player) {
        listener.start();
    }
    static Config make_cfg(int port, bool enabled) {
        Config c; c.rosstalk_enabled = enabled; c.rosstalk_port = port; return c;
    }
    int connect() {
        if (!wait_for([&] { return listener.bound_port() != 0; })) return -1;
        return connect_to(listener.bound_port());
    }
    void send(const char* s, bool close_after = false) {
        int fd = connect();
        if (fd < 0) return;
        ::send(fd, s, std::char_traits<char>::length(s), 0);
        if (close_after) ::close(fd); else leaked = fd;
    }
    int leaked = -1;
    ~Rig() { if (leaked >= 0) ::close(leaked); }
};

int main() {
    {
        Rig r;
        r.send("CC PLAY\r\nCC HOLD", true);
        CHECK(wait_for([&] { return r.listener.state().commands_seen == 2; }),
              "framing: two commands, the unterminated one handled on close");
        CHECK(r.listener.state().last_text == "CC HOLD", "framing: last_text is the final line");
    }
    {
        Rig r;
        r.send("CC JUMP\n");
        CHECK(wait_for([&] { return r.listener.state().last_result == "unknown command"; }),
              "unknown command is reported and nothing else happens");
    }
    {
        // A stray line must not cancel a cue in flight, nor hide its outcome.
        Rig r;
        r.send("CC CUE x\nCC JUMP\n");
        CHECK(wait_for([&] { return r.listener.state().commands_seen == 2; }),
              "unknown during a cue: both lines counted");
        CHECK(wait_for([&] { return r.listener.state().last_result == "no such marker"; }, 1000),
              "unknown during a cue: the cue still finishes and its outcome stands");
        CHECK(r.listener.state().last_text == "CC JUMP", "unknown during a cue: last_text is the stray line");
    }
    {
        Rig r;
        r.send("CC CUE Sermon\n");
        CHECK(wait_for([&] { return r.listener.state().last_result == "no such marker"; }, 1000),
              "CUE with no event: no such marker within 1 s");
    }
    {
        Rig r;
        r.send("CC CUE x\nCC HOLD\n");
        CHECK(wait_for([&] { return r.listener.state().last_text == "CC HOLD"; }),
              "newest wins: HOLD is the last command");
        // The cue worker was cancelled: a stale cue must not overwrite the result.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(r.listener.state().last_text == "CC HOLD" &&
              r.listener.state().last_result == "done",
              "newest wins: the cancelled cue leaves the HOLD result alone");
    }
    {
        int blocker = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; ::setsockopt(blocker, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        ::bind(blocker, (sockaddr*)&a, sizeof a); ::listen(blocker, 1);
        socklen_t l = sizeof a; ::getsockname(blocker, (sockaddr*)&a, &l);
        Rig r(ntohs(a.sin_port));
        CHECK(wait_for([&] { return r.listener.state().error == "port in use"; }),
              "port busy: state().error is \"port in use\"");
        CHECK(r.listener.bound_port() == 0, "port busy: nothing bound, engine still running");
        ::close(blocker);
    }
    {
        Config d;
        CHECK(!d.rosstalk_enabled && d.rosstalk_bind == "127.0.0.1" && d.rosstalk_port == 7790,
              "defaults: disabled, 127.0.0.1, 7790");
        NullVideoOutput vout; NullAudioOutput aout;
        Player player(Config(), vout, aout);   // no overrides: the real default
        RossTalkListener l(player);
        l.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto s = l.state();
        CHECK(l.bound_port() == 0 && !s.enabled && s.error.empty(),
              "off by default: nothing bound");
    }
    {
        NullVideoOutput vout; NullAudioOutput aout;
        Player player(Config(), vout, aout);
        RossTalkListener never(player);
        never.stop();
        CHECK(true, "stop() without start() returns");
        Rig r;
        r.send("CC CUE x\n");
        auto t0 = Clock::now();
        r.listener.stop();
        CHECK(Clock::now() - t0 < std::chrono::seconds(2), "stop() after start() joins within 2 s");
    }
    {
        const std::string path = "/tmp/rosstalk_cfg_test.json";
        std::string err;
        Config c;
        c.rosstalk_enabled = true; c.rosstalk_bind = "0.0.0.0"; c.rosstalk_port = 7791;
        CHECK(c.save(path, err), "config save");
        Config r;
        CHECK(r.load(path, err), "config load");
        CHECK(r.rosstalk_enabled && r.rosstalk_bind == "0.0.0.0" && r.rosstalk_port == 7791,
              "config round trip keeps enabled/bind/port");
        c.rosstalk_bind = "evil.example"; c.rosstalk_port = 70000;
        c.save(path, err);
        Config b;
        b.load(path, err);
        CHECK(b.rosstalk_bind == "127.0.0.1" && b.rosstalk_port == 7790,
              "config load: bad bind and port fall back to the defaults");
        ::unlink(path.c_str());
    }
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail ? 1 : 0;
}
