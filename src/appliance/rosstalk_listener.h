// SPDX-License-Identifier: GPL-3.0-or-later
// rosstalk_listener.h — ProPresenter's RossTalk cues over TCP, driving the Player.
//
// One io thread polls the listening socket and every client, and every second
// compares the Player's config (rosstalk_enabled/bind/port) with what is bound,
// closing or (re)binding as needed. A CUE runs on one cue worker thread; any
// newer command cancels it before acting. Nothing here checks Player::locked():
// a cue from ProPresenter is the operator, and the lock is for the web page.
#pragma once

#include "player.h"
#include "rosstalk.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_player {

struct RossTalkState {
    bool enabled = false; std::string bind; int port = 0;
    std::string error;          // "" | "port in use" | other bind failure text
    std::string last_text; long long last_at_ms = 0;   // Unix ms
    std::string last_result;    // "done" | "no such marker" | "timed out" | "unknown command" | "cueing"
    unsigned long long commands_seen = 0;
};

class RossTalkListener {
public:
    explicit RossTalkListener(Player& player);
    ~RossTalkListener();
    void start();
    void stop();                 // joins everything; safe without start()
    RossTalkState state() const;
    int bound_port() const { return m_bound_port.load(); }   // 0 = not bound

private:
    static constexpr size_t kMaxClients = 8;   // a further accept is closed at once
    struct Client { int fd; std::string buf; bool discarding; };
    void io_loop();
    void reconcile(Config cfg, std::string& bound_bind, int& bound_cfg_port, int& listen_fd,
                   std::vector<Client>& clients);
    void handle_line(const std::string& line);
    void cancel_cue();
    void run_cue(long long target_ms);
    void finish_cue(CueProgress& p);

    Player& m_player;
    mutable std::mutex m_mtx;    // m_state
    RossTalkState m_state;
    std::atomic<int> m_bound_port{0};
    std::atomic<bool> m_stop{false}, m_cue_cancel{false}, m_cue_pending{false};
    std::thread m_io, m_cue;     // m_cue is touched only from the io thread and stop()
};

} // namespace multisite_player
