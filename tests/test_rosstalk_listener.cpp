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
        Rig r(0, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto s = r.listener.state();
        CHECK(r.listener.bound_port() == 0 && !s.enabled && s.error.empty(),
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
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail ? 1 : 0;
}
