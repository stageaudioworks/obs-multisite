// SPDX-License-Identifier: GPL-3.0-or-later
#include "relay_state.h"

#include <algorithm>
#include <cstdint>

namespace multisite_relay {

using multisite::Manifest;
using multisite::RoomState;

namespace {
constexpr int kBackoffFirstMs = 1000;
constexpr int kBackoffMaxMs   = 15000;
// A child that streamed for this long before dying is treated as a fresh
// problem rather than a continuing one, so an outage hours ago does not leave
// a destination reconnecting on a fifteen-second backoff for the rest of the
// event.
constexpr int64_t kBackoffResetAfterMs = 120000;
// A segment that is a moment late is not a stall. Running close to the live
// edge, the next fragment is routinely a second or two behind its due time,
// and reporting each of those as "nothing coming from the main site" flickers
// the operator's screen and fills the log with a problem that is not
// happening. The grace period is still measured from the moment content
// actually stopped, so this delays only what is SAID, never when we act.
constexpr int64_t kStallAfterMs = 3000;
} // namespace

const char* to_string(RelayState s) {
    switch (s) {
        case RelayState::Idle:         return "idle";
        case RelayState::Waiting:      return "waiting";
        case RelayState::Awaiting:     return "awaiting";
        case RelayState::Streaming:    return "streaming";
        case RelayState::Stalled:      return "stalled";
        case RelayState::Reconnecting: return "reconnecting";
        case RelayState::Ending:       return "ending";
        case RelayState::Stopped:      return "stopped";
        case RelayState::Blocked:      return "blocked";
    }
    return "?";
}

std::string describe(RelayState s) {
    switch (s) {
        case RelayState::Idle:         return "Not sending";
        case RelayState::Waiting:      return "Waiting for the event to start";
        case RelayState::Awaiting:     return "Ready — waiting to be connected to";
        case RelayState::Streaming:    return "Sending";
        case RelayState::Stalled:      return "Nothing coming from the main site";
        case RelayState::Reconnecting: return "Reconnecting";
        case RelayState::Ending:       return "Finishing off";
        case RelayState::Stopped:      return "Finished";
        case RelayState::Blocked:      return "Cannot send this event";
    }
    return {};
}

uint64_t seq_behind_live(uint64_t latest_seq, uint64_t first_available_seq,
                         double seg_s, int delay_s) {
    const double seg = seg_s > 0.1 ? seg_s : 6.0;
    const uint64_t back = (uint64_t)std::max(0.0, (double)delay_s / seg);
    uint64_t want = (latest_seq > back) ? (latest_seq - back) : 0;
    // Never before what storage still holds, and never past the live edge.
    want = std::max(want, first_available_seq);
    want = std::min(want, latest_seq);
    return want;
}

uint64_t start_seq_for_delay(const Manifest& m, int delay_s) {
    return seq_behind_live(m.latest_seq, m.first_available_seq,
                           m.stream_duration_hint(), delay_s);
}

void RelayMachine::enter(RelayState s, int64_t now_ms, const std::string& why) {
    m_state = s;
    if (s == RelayState::Streaming) m_streaming_since_ms = now_ms;
    if (s != RelayState::Stalled) m_stalled_since_ms = 0;
    if (s != RelayState::Awaiting) m_backed_up_since_ms = 0;
    if (!why.empty()) m_last_error = why;
}

double RelayMachine::behind_live_s(const RelayInput& in) const {
    if (!m_head_set) return -1.0;
    if (m_state != RelayState::Streaming && m_state != RelayState::Stalled &&
        m_state != RelayState::Ending)
        return -1.0;
    if (in.latest_seq < m_head) return 0.0;
    const double seg = in.segment_duration_s > 0.1 ? in.segment_duration_s : 6.0;
    return (double)(in.latest_seq - m_head) * seg;
}

void RelayMachine::note_child_exited(int64_t now_ms, bool expected) {
    if (expected) return;
    ++m_restarts;
    // Reset the backoff if the last run was healthy for a good while: this is
    // a new problem, not the same one still failing.
    if (m_streaming_since_ms > 0 &&
        now_ms - m_streaming_since_ms > kBackoffResetAfterMs)
        m_backoff_ms = 0;
    m_backoff_ms = m_backoff_ms ? std::min(m_backoff_ms * 2, kBackoffMaxMs)
                                : kBackoffFirstMs;
    m_retry_at_ms = now_ms + m_backoff_ms;
}

void RelayMachine::reconfigured(int64_t now_ms) {
    m_state = RelayState::Waiting;
    m_head_set = false;          // the delay may be what changed
    m_retry_at_ms = 0;           // an edit is not a failure to back off from
    m_stalled_since_ms = 0;
    m_overdue_since_ms = 0;
    m_backed_up_since_ms = 0;
    m_streaming_since_ms = now_ms;
    m_last_error.clear();
}

RelayDecision RelayMachine::step(const RelayInput& in) {
    RelayDecision d;
    const int64_t now = in.now_ms;
    const double seg_s = in.segment_duration_s > 0.1 ? in.segment_duration_s : 6.0;
    const int64_t seg_ms = (int64_t)(seg_s * 1000.0);

    // ── The operator's switch wins over everything ───────────────────────────
    if (!in.operator_wants_running) {
        if (in.child_alive) {
            d.action = RelayAction::Kill;
            d.note = "stopped by the operator";
            enter(RelayState::Idle, now, {});
            m_head_set = false;
            return d;
        }
        if (m_state != RelayState::Idle) {
            enter(RelayState::Idle, now, {});
            m_head_set = false;
            d.note = "stopped";
        }
        return d;
    }

    // ── Something about this feed makes it unsendable ────────────────────────
    // Checked before anything else, because the whole point is that a feed we
    // cannot serve correctly must not be served at all. ffmpeg would not
    // object, so this is the only place it can be caught.
    if (!in.plan_ok) {
        if (in.child_alive) {
            d.action = RelayAction::Kill;
            d.note = in.plan_problem;
            enter(RelayState::Blocked, now, in.plan_problem);
            return d;
        }
        if (m_state != RelayState::Blocked) {
            enter(RelayState::Blocked, now, in.plan_problem);
            d.note = in.plan_problem;
        } else {
            m_last_error = in.plan_problem;
        }
        return d;
    }
    if (m_state == RelayState::Blocked) {
        // Whatever it was has been fixed — a new event with the right codec,
        // or the operator picking a track that exists.
        enter(RelayState::Waiting, now, {});
        m_last_error.clear();
        d.note = "this event can be sent now";
    }

    // ── An unexpected exit ───────────────────────────────────────────────────
    if (!in.child_alive && (m_state == RelayState::Streaming ||
                            m_state == RelayState::Stalled ||
                            m_state == RelayState::Awaiting)) {
        enter(RelayState::Reconnecting, now,
              "the connection to the destination dropped");
        d.note = "connection lost — reconnecting";
        return d;
    }

    // ── Nothing to send ──────────────────────────────────────────────────────
    const bool room_has_content =
        (in.room == RoomState::Live || in.room == RoomState::Ended ||
         in.room == RoomState::Interrupted);

    if (!room_has_content) {
        if (in.child_alive) {
            // The room went away underneath us. Let whatever is buffered
            // finish rather than cutting it off mid-sentence.
            d.action = RelayAction::CloseInput;
            enter(RelayState::Ending, now, {});
            d.note = "the main site is no longer broadcasting";
            return d;
        }
        if (m_state != RelayState::Waiting && m_state != RelayState::Stopped) {
            enter(RelayState::Waiting, now, {});
            d.note = "waiting for the event to start";
        }
        return d;
    }

    // ── Start, or restart ────────────────────────────────────────────────────
    if (!in.child_alive) {
        if (m_state == RelayState::Ending) {
            // The drain finished and ffmpeg exited on its own.
            enter(RelayState::Stopped, now, {});
            d.note = "everything has been sent";
            return d;
        }
        if (m_state == RelayState::Stopped) return d;

        // Take up position FIRST, before waiting on anything. The downloader
        // fetches around wherever the destinations are reading, so until this
        // is set nothing knows which part of the event to bring down — and
        // a relay starting three minutes back would sit waiting for segments
        // that were never going to be fetched.
        if (!m_head_set) {
            if (in.from_beginning) {
                // A rebroadcast starts at its in-point when one was chosen,
                // and never before what storage still holds — a cue whose
                // segment has aged out cannot be honoured.
                m_head = in.start_seq > in.first_available_seq
                           ? in.start_seq
                           : in.first_available_seq;
            } else {
                m_head = seq_behind_live(in.latest_seq, in.first_available_seq,
                                         seg_s, in.delay_s);
            }
            m_head_set = true;
        } else {
            // A restart resumes from where the feed got to, so nothing is
            // skipped. It costs whatever the restart took, which is added to
            // the delay rather than sprinted off.
            m_head = std::max(m_head, in.first_available_seq);
        }
        if (now < m_retry_at_ms) return d;      // serving the backoff

        // Nothing is spawned until there is something to send it. ffmpeg given
        // an init segment and then nothing would sit on an idle socket, which
        // is exactly what a destination ends a broadcast for.
        if (!in.init_ready || !in.next_segment_ready) {
            if (m_state != RelayState::Waiting) {
                enter(RelayState::Waiting, now, {});
                d.note = "waiting for the event to download";
            }
            return d;
        }

        m_anchor_ms = now;
        m_anchor_seq = m_head;
        d.action = RelayAction::Spawn;
        d.note = (m_state == RelayState::Reconnecting) ? "reconnecting"
                                                       : "starting";
        enter(RelayState::Streaming, now, {});
        return d;
    }

    // ── Running: is there anything to send, and is it due? ───────────────────
    // An out-point closes the feed there, whatever the room is still doing: an
    // excerpt bounded by two cues ends when the excerpt ends, not when the
    // recording does.
    if (in.end_seq > 0 && m_head > in.end_seq &&
        m_state != RelayState::Ending) {
        d.action = RelayAction::CloseInput;
        enter(RelayState::Ending, now, {});
        d.note = "the out-point cue has been reached — sending up to it";
        return d;
    }

    const bool past_end = m_head > in.latest_seq;

    if (past_end && (in.room == RoomState::Ended ||
                     in.room == RoomState::Interrupted)) {
        // Everything the main site produced has been sent. Close the pipe and
        // let ffmpeg flush: the last seconds are lost otherwise, which was
        // measurable in the Stage 0 rig.
        if (m_state != RelayState::Ending) {
            d.action = RelayAction::CloseInput;
            enter(RelayState::Ending, now, {});
            d.note = in.room == RoomState::Interrupted
                       ? "the main site stopped unexpectedly — sending what "
                         "arrived"
                       : "the event has ended — sending the last of it";
            return d;
        }
        return d;   // waiting for ffmpeg to finish
    }
    if (m_state == RelayState::Ending) return d;

    // ── Content is not going out ────────────────────────────────────────────
    // Checked before anything is fed, because feeding a pipe nobody is
    // emptying only makes the backlog bigger. What it MEANS depends on
    // whether this destination calls out or is called:
    //
    //   • Called (an SRT listener) and nothing has ever gone out: the far end
    //     has not attached yet. That is the resting state of a listener, it
    //     may last the whole first half of an event, and it is neither a
    //     fault nor something to give up on.
    //   • Anything else: the destination has stopped reading. Held for the
    //     same grace period a silence gets — a receiver pausing for a moment
    //     is not worth splitting the recording over — and then dropped and
    //     rebuilt like any other lost connection.
    if (!in.output_accepting) {
        if (in.awaits_receiver && !in.output_ever_accepted) {
            if (m_state != RelayState::Awaiting) {
                enter(RelayState::Awaiting, now, {});
                d.note = "ready — waiting for the far end to connect";
            }
            // Keep taking up position while we wait. There is no continuity
            // to preserve for a connection that has never carried anything,
            // so someone attaching forty minutes in should get the event as
            // it is now rather than the forty minutes they missed — and
            // holding a stale position would pin the cache open besides.
            if (!in.from_beginning)
                m_head = seq_behind_live(in.latest_seq, in.first_available_seq,
                                         seg_s, in.delay_s);
            return d;
        }

        if (m_backed_up_since_ms == 0) m_backed_up_since_ms = now;
        if (now - m_backed_up_since_ms >= (int64_t)in.grace_s * 1000) {
            d.action = RelayAction::Kill;
            d.note = "the destination stopped accepting content for " +
                     std::to_string(in.grace_s) +
                     " seconds — dropping the connection";
            enter(RelayState::Reconnecting, now,
                  "the destination stopped accepting content");
            m_retry_at_ms = now + kBackoffFirstMs;
            return d;
        }
        return d;   // still inside the grace period: ride it out
    }

    if (m_state == RelayState::Awaiting) {
        // Something attached. Pacing is anchored afresh at this moment: the
        // segments that came due during the wait were never sent and must not
        // now all be released at once.
        m_anchor_ms = now;
        m_anchor_seq = m_head;
        m_overdue_since_ms = 0;
        enter(RelayState::Streaming, now, {});
        d.note = "the far end connected";
    }
    m_backed_up_since_ms = 0;

    const int64_t due_ms = m_anchor_ms +
                           (int64_t)(m_head - m_anchor_seq) * seg_ms;
    if (now < due_ms) {
        // Not yet its turn. Being early is normal and is how 1x pacing holds.
        m_overdue_since_ms = 0;
        if (m_state == RelayState::Stalled) {
            enter(RelayState::Streaming, now, {});
            d.note = "the main site is sending again";
        }
        return d;
    }

    if (!in.next_segment_ready) {
        // Overdue and nothing there. THIS is the only place starvation can be
        // noticed: ffmpeg is sitting on the pipe saying nothing.
        if (m_overdue_since_ms == 0) m_overdue_since_ms = now;
        const int64_t overdue = now - m_overdue_since_ms;

        if (overdue >= kStallAfterMs && m_state != RelayState::Stalled) {
            enter(RelayState::Stalled, now, {});
            m_stalled_since_ms = m_overdue_since_ms;   // measured from the gap
            d.note = "nothing arriving from the main site";
            return d;
        }
        if (overdue >= (int64_t)in.grace_s * 1000) {
            // Past what the destination will tolerate. Give up on this
            // connection deliberately rather than holding a socket the
            // far end has already abandoned.
            d.action = RelayAction::Kill;
            d.note = "nothing from the main site for " +
                     std::to_string(in.grace_s) +
                     " seconds — dropping the connection";
            enter(RelayState::Reconnecting, now,
                  "the main site stopped sending for more than " +
                  std::to_string(in.grace_s) + " seconds");
            m_retry_at_ms = now + kBackoffFirstMs;
            m_overdue_since_ms = 0;
            return d;
        }
        return d;   // still inside the grace period: ride it out
    }

    m_overdue_since_ms = 0;
    if (m_state == RelayState::Stalled) {
        enter(RelayState::Streaming, now, {});
        d.note = "the main site is sending again";
    }

    d.action = RelayAction::FeedSegment;
    d.seq = m_head;
    ++m_head;
    return d;
}

} // namespace multisite_relay
