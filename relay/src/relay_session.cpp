// SPDX-License-Identifier: GPL-3.0-or-later
#include "relay_session.h"
#include "log.h"
#include "relay_send.h"

#include <chrono>
#include <cstdint>

namespace multisite_relay {

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
constexpr int kDefaultDelayS = 180;   // three minutes, the room default
constexpr int kGraceS        = 45;    // under YouTube's ~60s starvation window
// How long a backlog may sit still before it counts as the far end having
// stopped reading. A fragment is several megabytes and leaves in 64 KB bites,
// so a pipe that is briefly full is the normal condition of a healthy stream,
// not a symptom. Two seconds is comfortably longer than one fragment takes to
// drain at any bitrate this carries, and short enough that the real thing is
// noticed well inside the grace period.
constexpr int64_t kDrainQuietMs = 2000;
} // namespace

RelaySession::RelaySession(Destination dest, RoomFeeder& feeder,
                           bool from_beginning, uint64_t start_seq,
                           uint64_t end_seq)
    : m_dest(std::move(dest)), m_feeder(feeder),
      m_from_beginning(from_beginning),
      m_start_seq(start_seq), m_end_seq(end_seq) {
    m_enabled = m_dest.enabled;
}

RelaySession::~RelaySession() { stop_thread(); }

void RelaySession::start_thread() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void RelaySession::stop_thread() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    if (m_child) { m_child->stop(); m_child.reset(); }
}

void RelaySession::set_enabled(bool on) {
    m_enabled = on;
    std::lock_guard<std::mutex> lk(m_mtx);
    m_dest.enabled = on;
}

void RelaySession::update(const Destination& d) {
    std::lock_guard<std::mutex> lk(m_mtx);
    const int64_t id = m_dest.id;
    const bool rebuild = affects_stream(m_dest, d);
    m_dest = d;
    m_dest.id = id;
    m_enabled = d.enabled;
    if (rebuild) m_reconfigured = true;
}

Destination RelaySession::destination() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_dest;
}

bool RelaySession::queue_init() {
    auto init = m_feeder.load_init();
    if (!init) return false;
    m_pending.insert(m_pending.end(), init->begin(), init->end());
    return true;
}

bool RelaySession::queue_segment(uint64_t seq) {
    auto media = m_feeder.load_segment(seq);
    if (!media) return false;
    m_pending.insert(m_pending.end(), media->begin(), media->end());
    return true;
}

void RelaySession::pump_writes() {
    if (!m_child || m_pending_offset >= m_pending.size()) return;
    // One bite per pass. The pipe fills, the write returns short, and we come
    // back — which is exactly the back-pressure that keeps the send at 1x
    // without ffmpeg needing to be told to pace itself.
    const size_t left = m_pending.size() - m_pending_offset;
    const long n = m_child->write_some(m_pending.data() + m_pending_offset, left);
    if (n < 0) return;                       // gone; the machine will notice
    m_pending_offset += (size_t)n;
    if (n > 0) {
        m_last_accept_ms = now_ms();
        m_ever_accepted = true;
        std::lock_guard<std::mutex> lk(m_mtx);
        m_sent_bytes += n;
    }
    if (m_pending_offset >= m_pending.size()) {
        m_pending.clear();
        m_pending_offset = 0;
    }
}

void RelaySession::run() {
    while (m_running) {
        const int64_t t = now_ms();
        const auto snap = m_feeder.snapshot();

        Destination dest;
        { std::lock_guard<std::mutex> lk(m_mtx); dest = m_dest; }

        // The gate. Re-evaluated every pass rather than once at start, because
        // an event can change underneath us — a new event with a different
        // codec, or a track the operator chose that this one does not have.
        StreamPlan plan;
        if (snap.have_event_info) {
            plan = plan_stream(snap.manifest, dest, "pipe:0");
        } else {
            plan.ok = false;
            plan.problem = "Waiting to hear what the main site is sending.";
        }

        RelayInput in;
        in.now_ms = t;
        in.operator_wants_running = m_enabled;
        in.room = snap.room;
        in.latest_seq = snap.latest_seq;
        in.first_available_seq = snap.first_available_seq;
        in.segment_duration_s = snap.segment_duration_s;
        in.plan_ok = plan.ok;
        in.plan_problem = plan.problem +
                          (plan.remedy.empty() ? "" : " " + plan.remedy);
        in.child_alive = m_child && m_child->alive();
        in.next_segment_ready = m_feeder.has_segment(m_machine.head());
        in.init_ready = m_feeder.load_init().has_value();
        in.delay_s = dest.delay_s > 0 ? dest.delay_s : kDefaultDelayS;
        in.grace_s = kGraceS;
        // Nothing waiting to go out is not the same as nothing being taken.
        // Only a backlog that has stopped moving says anything at all.
        const bool backlog = m_pending_offset < m_pending.size();
        in.output_accepting = !backlog ||
                              (t - m_last_accept_ms) < kDrainQuietMs;
        in.output_ever_accepted = m_ever_accepted;
        in.awaits_receiver = is_listener(dest);
        in.from_beginning = m_from_beginning;
        in.start_seq = m_start_seq;
        in.end_seq   = m_end_seq;

        // A child that exited on its own must be reported to the machine
        // before it decides anything, so the backoff is applied.
        if (m_child && !in.child_alive && !m_child->exited_cleanly()) {
            // ffmpeg's stderr, with the secrets taken out. It echoes the
            // output URL in its error messages, and for SRT that URL carries
            // the passphrase and stream id while for RTMP it ends in the
            // stream key — so the unredacted line would put them on the
            // operator's screen, in /api/destinations, and in the container
            // log, having carefully kept them out of the command line.
            const std::string line = redact(m_child->last_error_line(), dest);
            if (!line.empty()) {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_error = line;
            }
        }

        // An edit that changes what is being sent: rebuild the stream, but
        // deliberately, so it is not reported to the operator as a fault.
        if (m_reconfigured.exchange(false)) {
            if (m_child) { m_child->stop(); m_child.reset(); }
            m_pending.clear();
            m_pending_offset = 0;
            m_ever_accepted = false;
            m_machine.reconfigured(t);
            rlog_info("[%s] settings changed — restarting this stream",
                      dest.name.c_str());
            continue;
        }

        const RelayDecision d = m_machine.step(in);

        if (!d.note.empty())
            rlog_info("[%s] %s", dest.name.c_str(), d.note.c_str());

        switch (d.action) {
            case RelayAction::Spawn: {
                m_pending.clear();
                m_pending_offset = 0;
                // Per connection, not per session: a listener that loses its
                // far end and rebuilds is waiting to be attached to again,
                // which is not the same as a destination that went deaf.
                m_ever_accepted = false;
                m_last_accept_ms = t;
                m_child = std::make_unique<FfmpegProcess>();
                std::string err;
                const auto safe = redact(plan.args, dest);
                std::string joined;
                for (const auto& a : safe) { joined += a; joined += " "; }
                rlog_info("[%s] %s", dest.name.c_str(), plan.summary.c_str());
                rlog_info("[%s] running: %s", dest.name.c_str(), joined.c_str());
                if (!m_child->start(plan.args, err)) {
                    m_child.reset();
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_error = err;
                    break;
                }
                // Only the init segment, which carries the codec
                // configuration ffmpeg needs before any fragment makes sense.
                // The fragments themselves arrive through FeedSegment, on the
                // machine's schedule — queueing one here as well sent the
                // first fragment twice and started every stream with a
                // stutter.
                if (!queue_init())
                    rlog_warn("[%s] the opening data is not downloaded yet",
                              dest.name.c_str());
                std::lock_guard<std::mutex> lk(m_mtx);
                m_started_ms = t;
                m_sent_bytes = 0;       // this connection's total, not all time
                m_detail = plan.summary;
                m_audio_label = plan.audio_label;
                m_error.clear();
                break;
            }
            case RelayAction::FeedSegment:
                queue_segment(d.seq);
                break;
            case RelayAction::CloseInput: {
                // Drain what is queued first, or the last few seconds never
                // leave. Measured: closing early truncates the tail.
                //
                // Bounded, because a drain is not guaranteed to finish: an SRT
                // listener that nobody ever attached to will never empty its
                // pipe, and without a deadline the end of an event would hang
                // this thread for good. Thirty seconds is far longer than a
                // real tail takes and far shorter than anyone would wait.
                const int64_t give_up_at = now_ms() + 30000;
                while (m_running && m_child &&
                       m_pending_offset < m_pending.size() &&
                       m_child->alive() && now_ms() < give_up_at) {
                    pump_writes();
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (m_child) m_child->close_input();
                break;
            }
            case RelayAction::Kill:
                if (m_child) { m_child->stop(); m_child.reset(); }
                m_pending.clear();
                m_pending_offset = 0;
                m_ever_accepted = false;
                break;
            case RelayAction::None:
                break;
        }

        if (m_child && !m_child->alive() &&
            m_machine.state() == RelayState::Reconnecting) {
            m_machine.note_child_exited(t, false);
            m_child->stop();
            m_child.reset();
        }

        pump_writes();

        // Outbound rate: everything sent, over how long it has been sending.
        //
        // A sliding window was the obvious thing and was wrong. Content leaves
        // in one burst per segment, so any window not an exact multiple of the
        // segment duration catches one burst or two and the figure swings by
        // a factor of two — which reads as an unstable connection when
        // nothing is wrong at all. A copy remux sends at whatever the main
        // site recorded, so the average since the stream started IS the
        // current rate, and it holds still.
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            const int64_t span = t - m_started_ms;
            if (m_started_ms > 0 && span > 1000)
                m_bitrate_kbps = (double)m_sent_bytes * 8.0 / (double)span;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

RelayStatus RelaySession::status() const {
    RelayStatus s;
    const auto snap = m_feeder.snapshot();

    std::lock_guard<std::mutex> lk(m_mtx);
    s.id = m_dest.id;
    s.name = m_dest.name;
    s.enabled = m_enabled;
    s.state = to_string(m_machine.state());
    s.state_text = describe(m_machine.state());
    s.detail = m_detail;
    s.audio_label = m_audio_label;
    s.restarts = m_machine.restarts();
    s.sent_bytes = m_sent_bytes;
    s.bitrate_kbps = m_bitrate_kbps;

    const auto st = m_machine.state();
    s.live = (st == RelayState::Streaming || st == RelayState::Stalled ||
              st == RelayState::Ending);
    if (s.live && m_started_ms > 0)
        s.uptime_s = (now_ms() - m_started_ms) / 1000;

    RelayInput in;
    in.latest_seq = snap.latest_seq;
    in.segment_duration_s = snap.segment_duration_s;
    s.behind_live_s = m_machine.behind_live_s(in);

    // The machine's reason wins: it is the one that describes why a stream is
    // not running, which is what the operator is looking at the screen for.
    s.error = !m_machine.last_error().empty() ? m_machine.last_error() : m_error;
    return s;
}

} // namespace multisite_relay
