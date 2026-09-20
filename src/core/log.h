// Copyright (C) 2026 Stage Audio Works
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// log.h — a way for the core to say something, without knowing who is listening.
//
// The core deliberately has no dependency on OBS, Qt or the appliance, and the
// cost of that until now was silence: `session.cpp` did all of this project's
// uploading in 600 lines without a single log call, because there was nothing
// it was allowed to call. The information was not missing — it was computed and
// thrown away:
//
//     PutResult r = m_cfg.mirror_transport->put(...);
//     if (!r.success) {
//         // put it back for a later pass
//
// An HTTP status and the provider's own error text, discarded, every 200 ms, for
// as long as a link stayed down. The only visible trace was a retry counter
// going up, which tells an operator that something is wrong and nothing about
// what.
//
// So: one function pointer, installed by whoever is hosting the core. The OBS
// plugin routes it into OBS's log, the appliance into its own, and a test can
// install nothing at all and get silence back.
//
// THREAD SAFETY. Every worker in the core may call these, so the sink is held
// behind a shared_ptr that is copied out under a short lock and invoked with
// nothing held. Installing a sink while other threads log is safe; the
// expectation is still that it is installed once, at startup, before any
// Session exists.
#include <functional>
#include <string>

namespace multisite {

enum class LogLevel { Info, Warn, Error };

// (level, already-formatted message). The host adds its own prefix and
// timestamp; the core does not presume to.
using LogSink = std::function<void(LogLevel, const std::string&)>;

// Installs the sink. Pass nullptr to go quiet again (what tests get by default).
void set_log_sink(LogSink sink);

// printf-style, to match the house style in the plugin and the appliance.
void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);

}  // namespace multisite
