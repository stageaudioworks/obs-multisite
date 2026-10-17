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
// target — a decoder still starting reads far from anything.
#pragma once

#include "player.h"
#include "resume_state.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace multisite_player {

enum class ResumeStep { Pin, Seek, Pause, Wait, Done };

struct ResumeProgress {            // owned by the caller, starts zeroed
    bool pinned_sent = false, seek_sent = false, pause_sent = false;
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

class ResumeKeeper {
public:
    ResumeKeeper(Player& player, std::string path);
    ~ResumeKeeper();
    // start() once, stop() any time (also without start()).
    void start();
    void stop();

private:
    void run();
    bool wait_ms(long long ms); // false when asked to stop
    void restore(const ResumeState& saved);
    void save_loop();

    Player&     m_player;
    std::string m_path;
    std::thread m_thread;
    std::mutex  m_mtx;
    std::condition_variable m_cv;
    bool        m_stop = false;
};

} // namespace multisite_player
