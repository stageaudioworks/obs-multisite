// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>

namespace multisite_player {

namespace {

std::mutex           g_mtx;
std::deque<LogEntry> g_ring;
bool                 g_debug = false;
std::function<void(int, const char*)> g_hook;   // under g_mtx

// Enough to cover an event's worth of interesting events without letting a
// chatty failure grow without bound on a box that runs for months.
constexpr size_t kMaxLines = 500;

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

} // namespace

const char* to_string(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:  return "info";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Error: return "error";
    }
    return "info";
}

void set_debug_logging(bool on) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_debug = on;
}

void set_log_hook(std::function<void(int, const char*)> hook) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_hook = std::move(hook);
}

void log_line(LogLevel level, const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (level == LogLevel::Debug && !g_debug) return;
    }

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    LogEntry e;
    e.at_ms = now_ms();
    e.level = level;
    e.text  = buf;

    // systemd reads a level prefix off stderr, so the journal colours warnings
    // and errors the way an operator expects rather than showing a wall of
    // undifferentiated notices.
    const char* sd = level == LogLevel::Error ? "<3>"
                   : level == LogLevel::Warn  ? "<4>"
                   : level == LogLevel::Debug ? "<7>" : "<6>";

    const std::time_t t = (std::time_t)(e.at_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    std::fprintf(stderr, "%s[%s] %s\n", sd, stamp, buf);
    std::fflush(stderr);

    std::function<void(int, const char*)> hook;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_ring.push_back(std::move(e));
        while (g_ring.size() > kMaxLines) g_ring.pop_front();
        hook = g_hook;
    }
    // A hook that logs would recurse; one that throws must not take the caller down.
    thread_local bool in_hook = false;
    if (hook && !in_hook) {
        in_hook = true;
        try {
            hook(level == LogLevel::Error ? 3 : level == LogLevel::Warn ? 4
                 : level == LogLevel::Debug ? 7 : 6, buf);
        } catch (...) {}
        in_hook = false;
    }
}

std::vector<LogEntry> recent_log(size_t max_lines) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::vector<LogEntry> out;
    const size_t n = g_ring.size();
    const size_t start = max_lines >= n ? 0 : n - max_lines;
    out.reserve(n - start);
    for (size_t i = start; i < n; ++i) out.push_back(g_ring[i]);
    return out;
}

} // namespace multisite_player
