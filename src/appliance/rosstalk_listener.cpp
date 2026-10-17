// SPDX-License-Identifier: GPL-3.0-or-later
#include "rosstalk_listener.h"
#include "rosstalk.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace multisite_player {
namespace {
using Clock = std::chrono::steady_clock;
long long unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

RossTalkListener::RossTalkListener(Player& player) : m_player(player) {}
RossTalkListener::~RossTalkListener() { stop(); }

void RossTalkListener::start() {
    if (m_io.joinable()) return;
    m_stop = false;
    m_io = std::thread([this] { io_loop(); });
}

void RossTalkListener::stop() {
    m_stop = true;
    if (m_io.joinable()) m_io.join();      // io_loop closes its sockets on the way out
    cancel_cue();
}

RossTalkState RossTalkListener::state() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state;
}

void RossTalkListener::cancel_cue() {
    m_cue_cancel = true;
    if (m_cue.joinable()) m_cue.join();
}

// (Re)bind to match cfg. A failed bind is retried on the next call (1 s).
void RossTalkListener::reconcile(Config cfg, std::string& bound_bind, int& bound_cfg_port,
                                 int& listen_fd, std::vector<Client>& clients) {
    const bool want = cfg.rosstalk_enabled;
    if (listen_fd >= 0 && (!want || bound_bind != cfg.rosstalk_bind ||
                           bound_cfg_port != cfg.rosstalk_port)) {
        ::close(listen_fd); listen_fd = -1; m_bound_port = 0;
        for (auto& c : clients) ::close(c.fd);
        clients.clear();
    }
    std::string err;
    if (want && listen_fd < 0) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)cfg.rosstalk_port);
        int one = 1;
        if (fd < 0) err = std::strerror(errno);
        else {
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (::inet_pton(AF_INET, cfg.rosstalk_bind.c_str(), &a.sin_addr) != 1)
                err = "bad bind address";
            else if (::bind(fd, (sockaddr*)&a, sizeof a) != 0)
                err = errno == EADDRINUSE ? "port in use" : std::strerror(errno);
            else if (::listen(fd, 8) != 0) err = std::strerror(errno);
        }
        if (err.empty()) {
            socklen_t l = sizeof a; ::getsockname(fd, (sockaddr*)&a, &l);
            listen_fd = fd; bound_bind = cfg.rosstalk_bind; bound_cfg_port = cfg.rosstalk_port;
            m_bound_port = ntohs(a.sin_port);
        } else if (fd >= 0) ::close(fd);
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    m_state.enabled = want; m_state.bind = cfg.rosstalk_bind; m_state.port = cfg.rosstalk_port;
    m_state.error = err;
}

void RossTalkListener::io_loop() {
    int listen_fd = -1, bound_cfg_port = 0;
    std::string bound_bind;
    std::vector<Client> clients;
    auto last_reconcile = Clock::now() - std::chrono::seconds(2);
    while (!m_stop) {
        if (Clock::now() - last_reconcile >= std::chrono::seconds(1)) {
            reconcile(m_player.config(), bound_bind, bound_cfg_port, listen_fd, clients);
            last_reconcile = Clock::now();
        }
        std::vector<pollfd> fds;
        if (listen_fd >= 0) fds.push_back({listen_fd, POLLIN, 0});
        for (auto& c : clients) fds.push_back({c.fd, POLLIN, 0});
        if (fds.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        if (::poll(fds.data(), fds.size(), 100) <= 0) continue;

        size_t i = 0;
        if (listen_fd >= 0) {
            if (fds[i++].revents & POLLIN) {
                int c = ::accept(listen_fd, nullptr, nullptr);
                if (c >= 0) clients.push_back({c, {}});
            }
        }
        // The new client (if any) is past the end of fds; it is polled next time round.
        for (size_t k = 0; i < fds.size(); ++i, ++k) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            Client& c = clients[k];
            char tmp[1024];
            ssize_t n = ::recv(c.fd, tmp, sizeof tmp, 0);
            if (n > 0) {
                c.buf.append(tmp, (size_t)n);
                size_t nl;
                while ((nl = c.buf.find('\n')) != std::string::npos) {
                    handle_line(c.buf.substr(0, nl));
                    c.buf.erase(0, nl + 1);
                }
                if (c.buf.size() > 4096) c.buf.clear();   // not RossTalk; do not grow without end
            } else {
                if (!c.buf.empty()) handle_line(c.buf);    // final unterminated line
                ::close(c.fd); c.fd = -1;
            }
        }
        for (size_t k = clients.size(); k-- > 0;)
            if (clients[k].fd < 0) clients.erase(clients.begin() + (long)k);
    }
    if (listen_fd >= 0) ::close(listen_fd);
    for (auto& c : clients) ::close(c.fd);
    m_bound_port = 0;
}

void RossTalkListener::handle_line(const std::string& line) {
    RossCommand cmd = parse_rosstalk_line(line);
    if (cmd.text.find_first_not_of(" \t") == std::string::npos) return;   // blank line
    // A newer CUE/PLAY/HOLD/LIVE cancels a running cue first; a stray line does not.
    if (cmd.kind != RossCmd::Unknown) { cancel_cue(); m_cue_pending = false; }
    std::string result = "done";
    long long target = -1;
    if (cmd.kind == RossCmd::Unknown) result = "unknown command";
    else if (cmd.kind == RossCmd::Cue) {
        Status st; m_player.status(st);
        target = find_cue_marker_ms(st.markers, cmd.arg, st.started_ms);   // once, not per tick
        result = "cueing";
    } else if (cmd.kind == RossCmd::Play) {
        Status st; m_player.status(st);
        if (st.paused) m_player.resume();
    } else if (cmd.kind == RossCmd::Hold) m_player.pause();
    else if (cmd.kind == RossCmd::Live) m_player.jump_to_live();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_state.commands_seen;
        m_state.last_text = cmd.text; m_state.last_at_ms = unix_ms();
        // An unknown line leaves a running cue's "cueing" (and later its outcome) alone.
        if (cmd.kind != RossCmd::Unknown || !m_cue_pending) m_state.last_result = result;
    }
    if (cmd.kind == RossCmd::Cue) {
        m_cue_cancel = false;
        m_cue_pending = true;
        m_cue = std::thread([this, target] { run_cue(target); });
    }
}

void RossTalkListener::run_cue(long long target_ms) {
    CueProgress p;
    p.started_wall_ms = unix_ms();
    while (!m_cue_cancel && !m_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (m_cue_cancel || m_stop) return;
        Status st; m_player.status(st);
        switch (next_cue_step(target_ms, st, p, unix_ms())) {
        case CueStep::Start:
            if (st.paused) m_player.resume(); else m_player.play();
            break;
        case CueStep::Seek: m_player.seek_to_media(seek_ms_for(target_ms)); break;
        case CueStep::Hold:
            m_player.pause();
            p.hold_sent = true;
            if (!m_cue_cancel) {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_state.last_result = p.outcome;
                m_cue_pending = false;
            }
            break;
        case CueStep::Done: return;
        case CueStep::Wait: break;
        }
    }
}

} // namespace multisite_player
