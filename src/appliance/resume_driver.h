// SPDX-License-Identifier: GPL-3.0-or-later
// resume_driver.h — bring a restarted player back to where it was.
//
// next_resume_step() is pure (a saved state, a Status, what has been sent, the
// clock) so the ordering is testable; ResumeKeeper is the thread around it, and
// uses only the Player's public controls. See PROJECT-SCOPE §8.1.
//
// The order is the point. seek_to_media() clears the hold (Player::seek_to_media
// sets m_paused = false), so the Pause comes AFTER the Seek, never before. And a
// Seek is sent once: it is not retried because the playhead looks far from the
// target — a decoder still starting reads far from anything. The Pause waits
// for the Seek to land (Status::seek_target_ms clears on the first frame
// delivered there): while held nothing is delivered, so a hold sent early
// leaves the screen on a pre-seek frame while the page says "held at 10:00".
#pragma once

#include "player.h"
#include "resume_state.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace multisite_player {

enum class ResumeStep { Pin, Seek, Pause, Wait, Done };

struct ResumeProgress {            // owned by the caller, starts zeroed
    bool pinned_sent = false, seek_sent = false, pause_sent = false;
    // The pin was observed on the saved event. Pinning from the saved config
    // means pinned_sent never goes true, but the operator must still win.
    bool pin_seen = false;
    long long started_wall_ms = 0; // 0: stamped on the first call
};

constexpr long long kResumeGiveUpMs = 60 * 1000;

// Called once per tick until it returns Done. It records what it asks for in
// `p`, so the caller only has to do it. When it returns Done after
// `p.pinned_sent` because of the give-up, the caller unpins (see
// resume_gave_up()).
ResumeStep next_resume_step(const ResumeState& saved, const Status& now,
                            ResumeProgress& p, long long now_wall_ms);

// true when `Done` was the give-up, so the caller should undo its own pin.
bool resume_gave_up(const ResumeProgress& p, long long now_wall_ms);

// Called with the Status read straight after pin_event(). That call does
// nothing while the Player has no session (just after start()); un-send it so
// it is tried again, instead of the next tick reading the missing pin as the
// operator choosing something else. Only meaningful in the same tick as the Pin.
void resume_pin_result(const ResumeState& saved, const Status& after_pin,
                       ResumeProgress& p);

// When the keeper rewrites resume.json: at once when the event, pin or hold
// changed, and otherwise every 10 s as a heartbeat, so a box that has sat
// held for an hour still has a fresh saved_wall_ms (the rule is "down for
// less than 30 minutes", not "last moved less than 30 minutes ago").
constexpr long long kResumeHeartbeatMs = 10 * 1000;
bool resume_should_write(long long now_wall_ms, long long last_write_wall_ms,
                         bool changed);

// Waits for the wall clock to be trusted, so a saved timestamp is not judged
// against a clock still at its last-shutdown value. Polls `synced`, sleeping
// `step_ms` with `wait` (which returns false when asked to stop). True when
// synchronised; false on timeout or stop. Pure, so the bound is testable.
constexpr long long kResumeClockWaitMs = 30 * 1000;
bool resume_wait_for_clock(const std::function<bool()>& synced,
                           const std::function<bool(long long)>& wait,
                           long long limit_ms = kResumeClockWaitMs,
                           long long step_ms = 500);

class ResumeKeeper {
public:
    ResumeKeeper(Player& player, std::string path,
                 std::function<bool()> clock_synced = {});
    ~ResumeKeeper();
    // start() once, stop() any time (also without start()).
    void start();
    void stop();

private:
    void run();
    bool wait_ms(long long ms); // false when asked to stop
    bool stopping();
    void restore(const ResumeState& saved);
    void save_loop();

    Player&     m_player;
    std::string m_path;
    std::thread m_thread;
    std::mutex  m_mtx;
    std::condition_variable m_cv;
    bool        m_stop = false;
    std::function<bool()> m_clock_synced;
};

} // namespace multisite_player
