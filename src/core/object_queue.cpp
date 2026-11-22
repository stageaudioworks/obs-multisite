// SPDX-License-Identifier: GPL-3.0-or-later
#include "object_queue.h"
#include "log.h"
#include "../vendor/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace multisite {

// File layout: <counter:012>.obj holds a one-line JSON header (key, content
// type, body length), a newline, then the body. One file, so an object can
// never be half there: the rename is the commit.
static bool write_atomic(const fs::path& path, const std::string& header,
                         const std::vector<uint8_t>& body) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(header.data(), (std::streamsize)header.size());
        f.put('\n');
        f.write((const char*)body.data(), (std::streamsize)body.size());
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

ObjectQueue::ObjectQueue(std::string dir, Transport& transport,
                         int base_backoff_ms, int max_backoff_ms)
    : m_dir(std::move(dir)), m_tx(transport),
      m_base_ms(base_backoff_ms), m_max_ms(max_backoff_ms) {
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    std::lock_guard<std::mutex> lk(m_mtx);
    for (const auto& p : list_locked()) {
        const uint64_t n = std::strtoull(fs::path(p).stem().string().c_str(), nullptr, 10);
        m_next = std::max(m_next, n + 1);
    }
}

ObjectQueue::~ObjectQueue() { stop(); }

std::vector<std::string> ObjectQueue::list_locked() const {
    std::vector<std::string> out;
    // Non-throwing increments: the range-for form throws from operator++ on a
    // directory changing underneath it, and this runs on the queue's thread,
    // where an uncaught exception ends the process.
    std::error_code ec;
    for (fs::directory_iterator it(m_dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".obj") out.push_back(it->path().string());
    std::sort(out.begin(), out.end());
    return out;
}

void ObjectQueue::enqueue(const std::string& key, const std::vector<uint8_t>& body,
                          const std::string& content_type) {
    const std::string header =
        json({ {"key", key}, {"content_type", content_type},
               {"len", body.size()} }).dump();
    std::lock_guard<std::mutex> lk(m_mtx);
    char name[32];
    std::snprintf(name, sizeof(name), "%012llu.obj", (unsigned long long)m_next++);
    if (!write_atomic(fs::path(m_dir) / name, header, body))
        log_error("object queue: could not write %s for %s", name, key.c_str());
}

size_t ObjectQueue::pending() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return list_locked().size();
}

int ObjectQueue::try_one() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto all = list_locked();
        if (all.empty()) return 0;
        path = all.front();
    }
    std::string key, type;
    size_t len = 0;
    std::vector<uint8_t> body;
    bool readable = false;
    {
        // Closed before anything below removes the file. Windows refuses to
        // delete a file that is still open, so with the stream alive the
        // remove failed, the object stayed first in the queue, and it was
        // uploaded again for ever while nothing behind it moved (CI,
        // windows-latest: no group index ever reached the store).
        std::ifstream f(path, std::ios::binary);
        std::string header;
        std::getline(f, header);
        try {
            json h = json::parse(header);
            key  = h.value("key", "");
            type = h.value("content_type", "application/octet-stream");
            len  = h.value("len", (size_t)0);
        } catch (...) {}
        body.resize(len);
        if (len) f.read((char*)body.data(), (std::streamsize)len);
        readable = !key.empty() && (len == 0 || (size_t)f.gcount() == len);
    }
    if (!readable) {
        // Unreadable: a write that never completed its rename cannot be here,
        // so this is damage on the disk. Nothing to send; say so and move on.
        log_error("object queue: %s is unreadable — skipping it", path.c_str());
        std::error_code ec; fs::remove(path, ec);
        return 1;
    }
    PutResult r = m_tx.put(key, body, type, {});
    if (!r.success) return -1;
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        // Stored, but still queued: it will be sent again (harmless — a PUT
        // of the same bytes) on the next try, and nothing behind it moves
        // until the remove succeeds. Said, because that is a stuck queue.
        log_error("object queue: %s is stored but could not be removed from "
                  "the queue (%s)", key.c_str(), ec.message().c_str());
        return -1;
    }
    m_stored++;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stored_by_type[type];
    }
    return 1;
}

uint64_t ObjectQueue::stored_of_type(const std::string& content_type) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_stored_by_type.find(content_type);
    return it == m_stored_by_type.end() ? 0 : it->second;
}

void ObjectQueue::remove_dir_if_empty() {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    if (fs::is_empty(m_dir, ec) && !ec) fs::remove(m_dir, ec);
}

void ObjectQueue::run() {
    int attempt = 0;
    while (m_running) {
        const int r = (!m_may_upload || m_may_upload()) ? try_one() : 0;
        if (r == 1) { attempt = 0; continue; }
        int wait = 100;                      // idle poll
        if (r < 0) {
            ++attempt;
            wait = std::min(m_max_ms, m_base_ms << std::min(attempt - 1, 16));
        }
        for (int slept = 0; slept < wait && m_running; slept += 25)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void ObjectQueue::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void ObjectQueue::stop() {
    if (!m_running.exchange(false)) return;
    m_tx.cancel_pending();
    if (m_thread.joinable()) m_thread.join();
}

bool ObjectQueue::drain_blocking(int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int attempt = 0;
    while (std::chrono::steady_clock::now() < end) {
        const int r = try_one();
        if (r == 0) return true;
        if (r == 1) { attempt = 0; continue; }
        ++attempt;
        const int wait = std::min(m_max_ms, m_base_ms << std::min(attempt - 1, 16));
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::min<int64_t>(wait, std::chrono::duration_cast<std::chrono::milliseconds>(
                end - std::chrono::steady_clock::now()).count() + 1)));
    }
    return pending() == 0;
}

} // namespace multisite
