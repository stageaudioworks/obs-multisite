// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// log.h — the appliance's log.
//
// Goes to stderr, which systemd captures into the journal, so `journalctl -u
// multisite-player` is the whole diagnostic story on a box with no screen
// attached. The last few hundred lines are also kept in memory and served to
// the web UI: an operator with a phone and no SSH should still be able to see
// why nothing is playing.
//
#include <functional>
#include <string>
#include <vector>

namespace multisite_player {

enum class LogLevel { Debug, Info, Warn, Error };

void log_line(LogLevel level, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define plog_debug(...) ::multisite_player::log_line(::multisite_player::LogLevel::Debug, __VA_ARGS__)
#define plog_info(...)  ::multisite_player::log_line(::multisite_player::LogLevel::Info,  __VA_ARGS__)
#define plog_warn(...)  ::multisite_player::log_line(::multisite_player::LogLevel::Warn,  __VA_ARGS__)
#define plog_error(...) ::multisite_player::log_line(::multisite_player::LogLevel::Error, __VA_ARGS__)

void set_debug_logging(bool on);

// A host can receive every line too (the Mac player sends them to os_log).
// `level` is the syslog number the stderr prefix uses (3 error, 4 warn, 6 info,
// 7 debug); `message` is the formatted text without the "<N>[time]" prefix.
// Called after the stderr and ring writes, on whatever thread logged, with no
// lock held: the hook must be thread-safe (a log line from inside the hook is
// dropped from it, and an exception out of it is swallowed). Safe to set or
// clear at any time; an empty function clears it, though a call already in
// flight on another thread may still run the old hook once.
void set_log_hook(std::function<void(int level, const char* message)> hook);

struct LogEntry {
    long long   at_ms = 0;
    LogLevel    level = LogLevel::Info;
    std::string text;
};

// Newest last. Copied out under the lock.
std::vector<LogEntry> recent_log(size_t max_lines = 200);

const char* to_string(LogLevel l);

} // namespace multisite_player
