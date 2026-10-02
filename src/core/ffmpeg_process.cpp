// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffmpeg_process.h"

#include <cctype>
#include <cstdio>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace multisite {

bool ffmpeg_supports_srt() {
    // Computed on first use and then never again. `-protocols` lists what the
    // binary was compiled with, which is the actual question — an ffmpeg
    // without libsrt still accepts an srt:// URL on the command line and only
    // fails once it tries to open it.
    static const bool supported = [] {
        FILE* p = ::popen("ffmpeg -hide_banner -protocols 2>/dev/null", "r");
        if (!p) return false;
        std::string out;
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        ::pclose(p);
        // One protocol per line, indented. Compared as a whole trimmed line
        // rather than searched for, because "srtp" is in almost every build,
        // is a different thing entirely, and contains the name we are looking
        // for.
        size_t i = 0;
        while (i < out.size()) {
            size_t nl = out.find('\n', i);
            if (nl == std::string::npos) nl = out.size();
            size_t a = i, b = nl;
            while (a < b && std::isspace((unsigned char)out[a])) ++a;
            while (b > a && std::isspace((unsigned char)out[b - 1])) --b;
            if (out.compare(a, b - a, "srt") == 0) return true;
            i = nl + 1;
        }
        return false;
    }();
    return supported;
}

namespace {

// ffmpeg says a great deal that is not a problem. Only lines that would help
// somebody fix something are promoted to the status line.
bool looks_like_an_error(const std::string& line) {
    static const char* markers[] = {
        "Connection refused", "Broken pipe", "Server error",
        "Operation not permitted", "Invalid data", "No such file",
        "Unable to open", "Failed to update header", "I/O error",
        "Connection timed out", "Error", "error", "not found",
    };
    // This one is noise: FLV cannot rewrite its header over a socket, and says
    // so twice on every single run.
    if (line.find("Failed to update header") != std::string::npos) return false;
    for (const char* m : markers)
        if (line.find(m) != std::string::npos) return true;
    return false;
}

} // namespace

FfmpegProcess::~FfmpegProcess() { stop(); }

bool FfmpegProcess::start(const std::vector<std::string>& args,
                          std::string& error) {
    if (args.empty()) { error = "no command to run"; return false; }

    int in_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (::pipe(in_pipe) != 0 || ::pipe(err_pipe) != 0) {
        error = std::string("could not create a pipe: ") + std::strerror(errno);
        if (in_pipe[0] >= 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); }
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("could not start a process: ") + std::strerror(errno);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // ── child ────────────────────────────────────────────────────────────
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        // Its own process group, so stopping one destination cannot take the
        // whole container's children with it.
        ::setpgid(0, 0);
        ::execvp(argv[0], argv.data());
        // Only reached if exec failed. The parent sees this on stderr.
        const char* msg = "relay: could not run ffmpeg\n";
        ssize_t ignored = ::write(STDERR_FILENO, msg, std::strlen(msg));
        (void)ignored;
        ::_exit(127);
    }

    // ── parent ───────────────────────────────────────────────────────────────
    ::close(in_pipe[0]);
    ::close(err_pipe[1]);
    m_pid = pid;
    m_stdin_fd = in_pipe[1];
    m_stderr_fd = err_pipe[0];
    m_exited = false;
    m_exit_code = -1;

    ::fcntl(m_stdin_fd, F_SETFL, O_NONBLOCK);
    // A destination that goes away mid-write must give us EPIPE, not a signal
    // that would take the whole relay down with it.
    ::signal(SIGPIPE, SIG_IGN);

    m_draining = true;
    m_stderr_thread = std::thread([this] { drain_stderr(); });
    return true;
}

void FfmpegProcess::drain_stderr() {
    std::string partial;
    char buf[4096];
    while (m_draining) {
        struct pollfd p { m_stderr_fd, POLLIN, 0 };
        const int r = ::poll(&p, 1, 250);
        if (r <= 0) continue;
        const ssize_t n = ::read(m_stderr_fd, buf, sizeof(buf));
        if (n <= 0) break;
        partial.append(buf, (size_t)n);
        size_t nl;
        // ffmpeg's progress lines end in \r, not \n; treating both as line
        // ends keeps the ring buffer from filling with one enormous line.
        while ((nl = partial.find_first_of("\r\n")) != std::string::npos) {
            std::string line = partial.substr(0, nl);
            partial.erase(0, nl + 1);
            if (line.empty()) continue;
            std::lock_guard<std::mutex> lk(m_out_mtx);
            m_lines.push_back(line);
            if (m_lines.size() > kKeepLines)
                m_lines.erase(m_lines.begin(),
                              m_lines.begin() + (long)(m_lines.size() - kKeepLines));
            if (looks_like_an_error(line)) m_last_error_line = line;
        }
    }
}

long FfmpegProcess::write_some(const uint8_t* data, size_t len) {
    if (m_stdin_fd < 0 || len == 0) return -1;
    const ssize_t n = ::write(m_stdin_fd, data, len);
    if (n >= 0) return (long)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;   // full, try later
    return -1;                                               // EPIPE: it's gone
}

void FfmpegProcess::close_input() {
    if (m_stdin_fd >= 0) { ::close(m_stdin_fd); m_stdin_fd = -1; }
}

bool FfmpegProcess::alive() {
    if (m_pid < 0) return false;
    if (m_exited) return false;
    int status = 0;
    const pid_t r = ::waitpid(m_pid, &status, WNOHANG);
    if (r == 0) return true;                 // still going
    if (r < 0 && errno == EINTR) return true;
    m_exited = true;
    m_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
}

void FfmpegProcess::stop() {
    if (m_pid > 0 && !m_exited) {
        ::kill(m_pid, SIGTERM);
        // Give it a moment to flush and go quietly before insisting.
        for (int i = 0; i < 30 && alive(); ++i)
            ::usleep(100 * 1000);
        if (alive()) {
            ::kill(m_pid, SIGKILL);
            int status = 0;
            ::waitpid(m_pid, &status, 0);
            m_exited = true;
            m_exit_code = -1;
        }
    }
    m_draining = false;
    if (m_stderr_thread.joinable()) m_stderr_thread.join();
    close_input();
    if (m_stderr_fd >= 0) { ::close(m_stderr_fd); m_stderr_fd = -1; }
    m_pid = -1;
}

std::string FfmpegProcess::recent_output() const {
    std::lock_guard<std::mutex> lk(m_out_mtx);
    std::string out;
    for (const auto& l : m_lines) { out += l; out += "\n"; }
    return out;
}

std::string FfmpegProcess::last_error_line() const {
    std::lock_guard<std::mutex> lk(m_out_mtx);
    return m_last_error_line;
}

} // namespace multisite
