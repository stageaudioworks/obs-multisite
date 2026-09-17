// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// relay_session.h — one destination, supervised.
//
// Owns the only ffmpeg child that will ever push to this destination, and the
// only thread that drives it. That ownership IS the idempotency guarantee the
// brief asks for: there is no code path that can produce a second process for
// the same destination, because starting one means setting a flag on the
// session that already exists rather than creating anything.
//
// All policy lives in RelayMachine; all process handling lives in
// FfmpegProcess. This is the loop that connects them to the bucket, plus the
// bookkeeping the operator's screen needs.
//
#include "destination.h"
#include "ffmpeg_process.h"
#include "relay_state.h"
#include "room_feeder.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_relay {

// What the UI shows. Deliberately in plain language and finished units: the
// browser renders these, it does not compute them.
struct RelayStatus {
    int64_t     id = 0;
    std::string name;
    std::string state;          // machine-readable, for styling
    std::string state_text;     // "Sending", "Reconnecting"
    std::string detail;         // "sending 1920x1080 H.264 with 'Main Mix'"
    std::string error;          // shown on screen, not just logged
    bool        enabled = false;
    bool        live = false;   // actually pushing right now
    int64_t     uptime_s = 0;
    double      behind_live_s = -1;
    double      bitrate_kbps = 0;
    int         restarts = 0;
    int64_t     sent_bytes = 0;
    std::string audio_label;
};

class RelaySession {
public:
    // `from_beginning` makes this a rebroadcast of a finished event rather
    // than a relay of a live one. The feeder it is given must be pinned to
    // that event. `start_seq`/`end_seq` are an optional excerpt — two cues
    // chosen as in and out points; 0 means "no bound".
    RelaySession(Destination dest, RoomFeeder& feeder,
                 bool from_beginning = false,
                 uint64_t start_seq = 0, uint64_t end_seq = 0);
    ~RelaySession();

    void start_thread();
    void stop_thread();

    // The operator's switch. Takes effect on the next pass of the loop; it
    // does not spawn or kill anything itself, so there is one place that does.
    void set_enabled(bool on);
    bool enabled() const { return m_enabled; }

    // Replaces the configuration. A change to what is actually being sent —
    // where it goes, which sound, how far behind — rebuilds the stream,
    // because none of those can be switched mid-flight. A change to anything
    // else (the name) leaves a stream that is on air completely alone.
    void update(const Destination& d);

    Destination destination() const;
    RelayStatus status() const;

    // Where this destination has read up to, so the feeder knows what it must
    // keep and what it may prune.
    uint64_t head() const { return m_machine.head(); }
    bool     has_position() const { return m_machine.has_position(); }
    bool     wants_content() const { return m_enabled; }

private:
    void run();
    void pump_writes();
    // Separate on purpose: the init segment goes in at spawn, and the media
    // fragments come only from the machine's FeedSegment decisions. Queueing
    // a fragment at spawn as well sent the first one twice.
    bool queue_init();
    bool queue_segment(uint64_t seq);

    Destination  m_dest;
    RoomFeeder&  m_feeder;
    const bool   m_from_beginning = false;
    // An excerpt bounded by two cues: 0 means "no bound", so the default is a
    // whole-event replay.
    const uint64_t m_start_seq = 0;
    const uint64_t m_end_seq = 0;
    RelayMachine m_machine;
    std::unique_ptr<FfmpegProcess> m_child;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_enabled{false};
    // Set by update(), acted on by the loop. The child is never touched from
    // the thread that edits the configuration: only the loop owns it.
    std::atomic<bool> m_reconfigured{false};

    // Content waiting to go into the pipe. A fragment is several megabytes and
    // the pipe takes it in 64 KB bites, so this is normal, not a backlog.
    std::vector<uint8_t> m_pending;
    size_t m_pending_offset = 0;
    // When the pipe last took anything, and whether it ever has on this
    // connection. A backlog that stops moving is the only visible sign that
    // the far end has stopped reading — or, for an SRT listener, has not
    // attached yet. RelayMachine decides which of those it is.
    int64_t m_last_accept_ms = 0;
    bool    m_ever_accepted = false;

    mutable std::mutex m_mtx;
    std::string m_detail;
    std::string m_error;
    std::string m_audio_label;
    int64_t m_started_ms = 0;
    int64_t m_sent_bytes = 0;
    double  m_bitrate_kbps = 0;
};

} // namespace multisite_relay
