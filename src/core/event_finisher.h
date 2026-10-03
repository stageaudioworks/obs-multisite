// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// event_finisher.h — uploads what an ended event still had queued, in the
// background, after the event is over.
//
// WHY. End broadcast drains the upload queue only to a deadline: the event has
// to end on air promptly, so campuses stop following it. What was still queued
// used to be marked ended with the event and deleted by the next Go Live, so a
// slow uplink at the end of a service lost the end of the recording. Now each
// event has its own spool folder (spool_queue.h), and this finishes the ones
// that ended with segments left: uploads them in order, adds each to that
// event's manifest as it lands, and removes the folder once it is empty. The
// recording simply gets longer.
//
// The live event always comes first: `may_upload` is asked before every
// segment (the mirror's yield rule), so a backlog never competes with the
// programme going out now. It survives a restart for free — the folders are on
// disk — so a backlog left when the application quits carries on next launch.
//
// One target: the primary bucket. ponytail: a second bucket does not get the
// backlog; add a second uploader here if that ever matters.

#include "spool_queue.h"
#include "transport.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace multisite {

struct FinisherConfig {
    std::string spool_root;
    size_t      manifest_window = 50;     // SessionConfig::manifest_window
    // Built per event from the current settings, so a credential refresh or a
    // settings change is picked up. Null = nothing to upload through yet.
    std::function<std::shared_ptr<Transport>()> transport;
    // False while the live event has its own uploads waiting.
    std::function<bool()> may_upload;
    int    base_backoff_ms = 500;
    int    max_backoff_ms  = 30000;
    double jitter          = 0.30;
    int    rescan_ms       = 3000;        // how often to look for work
};

class EventFinisher {
public:
    explicit EventFinisher(FinisherConfig cfg);
    ~EventFinisher();
    void start();
    void stop();

    struct Status {
        size_t events = 0;      // ended events with segments still to send
        size_t segments = 0;    // across all of them
    };
    // From the disk, so it is right whether or not the thread is running.
    Status status() const;

    // The operator's explicit choice: delete every ended event's backlog. The
    // one being uploaded stops first.
    void abandon();

    // One pass over the work, on the calling thread: finish every ended event
    // it can, until `stop()` or `may_upload` says no. For tests; the thread
    // runs exactly this.
    void run_once();

private:
    void loop();
    // Upload `event_id`'s backlog and keep its manifest up to date. True once
    // its folder is empty and removed.
    bool finish(const std::string& event_id);

    FinisherConfig    m_cfg;
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_abandon{false};
    std::mutex        m_work_mtx;   // held while an event is being finished
};

} // namespace multisite
