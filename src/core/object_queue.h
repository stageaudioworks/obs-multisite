// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// object_queue.h — a small durable queue of whole objects to PUT, in order,
// retried until each is stored.
//
// For the objects that are not segments but must still reach the store
// through an outage: a group's parity and its index (parity.h). The spool
// cannot carry them — it is indexed by segment sequence number, and its
// confirmed position means "every segment up to here", which an object that
// is not a segment would make false.
//
// One file per object, written tmp+rename like the spool, named by an
// increasing counter so the upload order is the enqueue order — a group's
// parity is always stored before the index that names it. Survives a crash:
// a restarted queue on the same folder carries on with what is left.
//
#include "transport.h"

#include <atomic>
#include <functional>
#include <map>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite {

class ObjectQueue {
public:
    ObjectQueue(std::string dir, Transport& transport,
                int base_backoff_ms = 250, int max_backoff_ms = 15000);
    ~ObjectQueue();

    // Durably queue an object. Safe from any thread.
    void enqueue(const std::string& key, const std::vector<uint8_t>& body,
                 const std::string& content_type);

    // Asked before every upload the queue's own thread makes; false waits.
    // The second bucket's copy uses it for the mirror's yield rule (session.h).
    // drain_blocking() does not ask: whoever calls it has decided to wait.
    // Set before start().
    void set_may_upload(std::function<bool()> f) { m_may_upload = std::move(f); }

    void start();
    void stop();
    // Upload until empty or the deadline passes. True when empty.
    bool drain_blocking(int timeout_ms);

    size_t   pending() const;
    uint64_t stored() const { return m_stored.load(); }
    // Objects of this content type stored so far.
    uint64_t stored_of_type(const std::string& content_type) const;
    // Remove the folder when nothing is left in it. Call with the queue stopped.
    void remove_dir_if_empty();

private:
    std::string m_dir;
    Transport&  m_tx;
    int         m_base_ms, m_max_ms;
    mutable std::mutex m_mtx;          // the counter and the folder listing
    uint64_t    m_next = 0;
    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_stored{0};
    std::map<std::string, uint64_t> m_stored_by_type;   // under m_mtx
    std::thread m_thread;
    std::function<bool()> m_may_upload;

    std::vector<std::string> list_locked() const;   // sorted *.obj paths
    // One attempt at the oldest object. 1 stored, 0 nothing queued, -1 failed.
    int try_one();
    void run();
};

} // namespace multisite
