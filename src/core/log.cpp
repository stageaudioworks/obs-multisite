// Copyright (C) 2026 Stage Audio Works
// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace multisite {
namespace {

std::mutex                   g_mtx;
std::shared_ptr<LogSink>     g_sink;

std::shared_ptr<LogSink> current_sink() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_sink;
}

// Formatted once, into a buffer sized from the message itself: a provider's
// error text can be long, and a truncated reason is the kind of log line that
// wastes an afternoon.
std::string format(const char* fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n <= 0) return std::string();
    std::vector<char> buf((size_t)n + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, ap);
    return std::string(buf.data(), (size_t)n);
}

void emit(LogLevel lvl, const char* fmt, va_list ap) {
    // Checked BEFORE formatting: with no sink installed — every test, and any
    // host that has not wired one up — this costs one lock and a null test
    // rather than a vsnprintf.
    std::shared_ptr<LogSink> sink = current_sink();
    if (!sink || !*sink) return;
    const std::string msg = format(fmt, ap);
    // Invoked with NOTHING held. The sink belongs to the host and may take its
    // own locks, or in OBS's case the log mutex; holding ours across that is
    // how a logging call becomes a deadlock.
    (*sink)(lvl, msg);
}

}  // namespace

void set_log_sink(LogSink sink) {
    auto p = std::make_shared<LogSink>(std::move(sink));
    std::lock_guard<std::mutex> lk(g_mtx);
    g_sink = std::move(p);
}

void log_info(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); emit(LogLevel::Info, fmt, ap); va_end(ap);
}
void log_warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); emit(LogLevel::Warn, fmt, ap); va_end(ap);
}
void log_error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); emit(LogLevel::Error, fmt, ap); va_end(ap);
}

}  // namespace multisite
