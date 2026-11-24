// SPDX-License-Identifier: GPL-3.0-or-later
#include "resume_driver.h"

#include "log.h"
#include "sysinfo.h"

#include <chrono>

namespace multisite_player {

static long long wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool resume_gave_up(const ResumeProgress& p, long long now_wall_ms) {
    return now_wall_ms - p.started_wall_ms > kResumeGiveUpMs;
}

ResumeStep next_resume_step(const ResumeState& saved, const Status& now,
                            ResumeProgress& p, long long now_wall_ms) {
    if (p.started_wall_ms == 0) p.started_wall_ms = now_wall_ms;

    if (resume_gave_up(p, now_wall_ms)) return ResumeStep::Done;

    if (saved.pinned && now.pinned_event_id == saved.event_id) p.pin_seen = true;

    // The operator chose something else after the pin took (ours or the
    // config's): theirs wins, and we neither re-pin nor seek.
    if ((p.pinned_sent || p.pin_seen) && now.pinned_event_id != saved.event_id)
        return ResumeStep::Done;

    // The pinned event is also a saved setting, so the box is often already on
    // it; pinning again would rebuild the session for nothing.
    if (saved.pinned && !p.pinned_sent && !p.pin_seen &&
        now.pinned_event_id != saved.event_id) {
        p.pinned_sent = true;
        return ResumeStep::Pin;
    }

    if (now.loading || now.event_id != saved.event_id) {
        // Unpinned, and the room is on a different event than it was: it moved
        // on, so follow it rather than drag it back.
        if (!saved.pinned && !now.loading && !now.event_id.empty() &&
            now.event_id != saved.event_id)
            return ResumeStep::Done;
        return ResumeStep::Wait;
    }

    if (!p.seek_sent) {
        p.seek_sent = true;
        // Held at the very start: a just-pinned event already begins there, and
        // Player::seek_to_media(0) reads as its own failure (BUGS #9).
        if (saved.media_ms > 0) return ResumeStep::Seek;
    }
    // Landing is the first delivered frame at the target; a hold before that
    // would keep the frame from ever arriving. Bounded by the give-up above.
    if (now.seek_target_ms != 0 && !p.pause_sent) return ResumeStep::Wait;
    if (saved.paused && !p.pause_sent) { p.pause_sent = true; return ResumeStep::Pause; }
    return ResumeStep::Done;
}

void resume_pin_result(const ResumeState& saved, const Status& after_pin,
                       ResumeProgress& p) {
    if (after_pin.pinned_event_id != saved.event_id) p.pinned_sent = false;
}

bool resume_should_write(long long now_wall_ms, long long last_write_wall_ms,
                         bool changed) {
    return changed || now_wall_ms - last_write_wall_ms >= kResumeHeartbeatMs;
}

bool resume_wait_for_clock(const std::function<bool()>& synced,
                           const std::function<bool(long long)>& wait,
                           long long limit_ms, long long step_ms) {
    for (long long waited = 0;; waited += step_ms) {
        if (synced()) return true;
        if (waited >= limit_ms) return false;
        if (!wait(step_ms)) return false;
    }
}

ResumeKeeper::ResumeKeeper(Player& player, std::string path,
                           std::function<bool()> clock_synced)
    : m_player(player), m_path(std::move(path)),
      m_clock_synced(clock_synced ? std::move(clock_synced)
                                  : std::function<bool()>(system_clock_synchronized)) {}

ResumeKeeper::~ResumeKeeper() { stop(); }

void ResumeKeeper::start() {
    if (m_thread.joinable()) return;
    m_thread = std::thread([this] { run(); });
}

void ResumeKeeper::stop() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

bool ResumeKeeper::stopping() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_stop;
}

bool ResumeKeeper::wait_ms(long long ms) {
    std::unique_lock<std::mutex> lk(m_mtx);
    m_cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return m_stop; });
    return !m_stop;
}

// One thread does the restore and then the saving, in that order: the file is
// never written while it is still being read for a restore, and a crash during
// the restore leaves the old file for the next start.
void ResumeKeeper::run() {
    ResumeState saved;
    if (load_resume_state(m_path, saved) && resume_worth_restoring(saved)) {
        // With no RTC the clock at boot is timesyncd's saved one, about when the
        // box was shut down: a file from last week would read seconds old.
        // Judge freshness only once the clock is trusted; if it never is, follow
        // live.
        const bool trusted = resume_wait_for_clock(
            m_clock_synced, [this](long long ms) { return wait_ms(ms); });
        if (!trusted) {
            if (!stopping())
                plog_warn("resume: ignoring %s — the clock was not synchronised "
                          "within 30 s, so its age cannot be judged; following "
                          "live as configured", m_path.c_str());
        } else if (!resume_is_fresh(saved, wall_ms())) {
            plog_info("resume: ignoring %s — saved more than 30 minutes ago, "
                      "following live as configured", m_path.c_str());
        } else {
            restore(saved);
        }
    }
    // Stopped mid-restore: the first pass of save_loop writes at once, which
    // would replace a good file with a half-restored state.
    if (stopping()) return;
    save_loop();
}

void ResumeKeeper::restore(const ResumeState& saved) {
    plog_info("resume: restoring event %s%s, %lld s in%s", saved.event_id.c_str(),
              saved.pinned ? " (pinned)" : "", saved.media_ms / 1000,
              saved.paused ? ", held" : "");
    ResumeProgress p;
    for (;;) {
        Status st;
        m_player.status(st);
        const long long now = wall_ms();
        switch (next_resume_step(saved, st, p, now)) {
        case ResumeStep::Pin:
            m_player.pin_event(saved.event_id);
            {
                Status after;
                m_player.status(after);
                resume_pin_result(saved, after, p);
            }
            break;
        case ResumeStep::Seek:
            m_player.seek_to_media(saved.media_ms);
            break;
        case ResumeStep::Pause:
            m_player.pause();
            break;
        case ResumeStep::Wait:
            break;
        case ResumeStep::Done:
            if (resume_gave_up(p, now)) {
                plog_warn("resume: gave up after 60 s — event %s did not load",
                          saved.event_id.c_str());
                if (p.pinned_sent) m_player.unpin_event();
            } else if (!p.seek_sent) {
                plog_info("resume: not restoring — the room is on another "
                          "event or the choice was changed");
            } else {
                plog_info("resume: restored");
            }
            return;
        }
        if (!wait_ms(250)) return;
    }
}

void ResumeKeeper::save_loop() {
    std::string last_event;
    bool last_pinned = false, last_paused = false, have_last = false;
    long long last_write_ms = 0;
    for (;;) {
        Status st;
        m_player.status(st);
        if (!st.event_id.empty() && !st.loading) {
            const bool pinned = !st.pinned_event_id.empty();
            const long long now = wall_ms();
            const bool changed = !have_last || st.event_id != last_event ||
                                 pinned != last_pinned || st.paused != last_paused;
            if (resume_should_write(now, last_write_ms, changed)) {
                ResumeState s;
                s.event_id = st.event_id;
                s.pinned = pinned;
                s.paused = st.paused;
                s.media_ms = st.playhead_ms;   // the delivered frame, not the download head
                s.saved_wall_ms = now;
                std::string err;
                if (!save_resume_state(m_path, s, err))
                    plog_warn("resume: cannot write %s: %s", m_path.c_str(), err.c_str());
                last_event = st.event_id; last_pinned = pinned;
                last_paused = st.paused;
                last_write_ms = now;
                have_last = true;
            }
        }
        if (!wait_ms(2000)) return;
    }
}

} // namespace multisite_player
