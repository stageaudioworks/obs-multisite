// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// ffmpeg_process.h — one ffmpeg child: start it, feed it, watch it, stop it.
//
// Holds no policy. It does not decide when to restart, how long to tolerate
// silence, or what to send — relay_state.h decides all of that. This only
// knows how to run the process and report honestly on whether it is still
// there.
//
// Writes are non-blocking on purpose. A pipe fills when ffmpeg is busy, and
// blocking on it would freeze the supervisor exactly when it most needs to
// respond to a Stop or a timeout. The caller keeps the remainder and offers it
// again, which is also what gives the feed its natural 1x pacing: the pipe
// only drains as fast as the destination takes it.
//
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite {

// Whether the ffmpeg on this machine was built with SRT support.
//
// Asked once and remembered: it cannot change while the relay is running, and
// running ffmpeg on every status poll would be absurd. It exists because the
// failure it prevents is a bad one — an operator saves an SRT destination, it
// validates, it plans, and then every attempt to start it dies inside ffmpeg
// with "Protocol not found" on a Sunday morning. Better said once on the page
// where the address is typed.
//
// Debian's ffmpeg, which is what the container ships, has it. A church that
// has swapped in their own build might not.
bool ffmpeg_supports_srt();

class FfmpegProcess {
public:
    ~FfmpegProcess();

    // Starts ffmpeg with `args` (argv[0] included). Returns false with
    // `error` set if the binary is missing or the process could not be
    // created — worth saying plainly, because on a fresh container the usual
    // cause is that ffmpeg was never installed.
    bool start(const std::vector<std::string>& args, std::string& error);

    // Accepts what it can without blocking. Returns the number of bytes
    // taken, 0 if the pipe is full right now, or -1 if the child has gone.
    long write_some(const uint8_t* data, size_t len);

    // No more content. ffmpeg flushes what it has and exits by itself; this is
    // how the last few seconds of an event reach the destination instead of
    // being cut off.
    void close_input();

    // Reaps the child if it has exited. Cheap; call it as often as you like.
    bool alive();

    // SIGTERM, then SIGKILL if it does not go. Returns once it is gone.
    void stop();

    // The most recent lines ffmpeg wrote to stderr. This is what turns "it
    // failed" into something an operator can act on, so it is surfaced in the
    // UI rather than only written to a log.
    std::string recent_output() const;

    // The last line that looked like a real error, for the status line.
    std::string last_error_line() const;

    int  exit_code() const { return m_exit_code; }
    bool exited_cleanly() const { return m_exited && m_exit_code == 0; }

private:
    void drain_stderr();

    int  m_pid = -1;
    int  m_stdin_fd = -1;
    int  m_stderr_fd = -1;

    std::atomic<bool> m_exited{false};
    std::atomic<int>  m_exit_code{-1};

    std::thread m_stderr_thread;
    std::atomic<bool> m_draining{false};

    mutable std::mutex m_out_mtx;
    std::vector<std::string> m_lines;      // bounded ring of recent output
    std::string m_last_error_line;
    static constexpr size_t kKeepLines = 40;
};

} // namespace multisite
