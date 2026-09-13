// SPDX-License-Identifier: GPL-3.0-or-later
#include "lan_object_server.h"

#include <cstring>

namespace multisite {

namespace {
const std::string kEventsPrefix = "/events/";
const std::string kSegmentsInfix = "segments/";
}

LanObjectServer::LanObjectServer(LanServerConfig cfg, std::string cache_dir)
    : m_cfg(std::move(cfg)) {
    // "pending" until the first real event arrives, the same placeholder a
    // fresh DecoderSession starts with — nothing is ever actually served
    // under that name, since handle_events() checks the request's event id
    // against m_event_id and there is no event to match until set_event()
    // (called from on_event_started()) moves it off "pending".
    m_cache = std::make_unique<SegmentCache>(std::move(cache_dir), "pending");
    m_http = std::make_unique<HttpServer>(m_cfg.bind_address, m_cfg.port);
    m_http->route_prefix("GET", kEventsPrefix,
                         [this](const HttpRequest& req, HttpResponse& res) {
                             handle_events(req, res);
                         });
    // Exact match, not a prefix: the room id is fixed for the life of this
    // server (see LanServerConfig::room_id), so there is exactly one such
    // path, the identical shape live_pointer_key() builds for the bucket.
    m_http->route("GET", "/rooms/" + m_cfg.room_id + "/live.json",
                 [this](const HttpRequest& req, HttpResponse& res) {
                     handle_live(req, res);
                 });
}

LanObjectServer::~LanObjectServer() { stop(); }

bool LanObjectServer::start(std::string& error) { return m_http->start(error); }
void LanObjectServer::stop() { m_http->stop(); }
int  LanObjectServer::port() const { return m_http->port(); }

bool LanObjectServer::check_auth(const HttpRequest& req) const {
    if (m_cfg.auth_token.empty()) return true;
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return false;
    return it->second == ("Bearer " + m_cfg.auth_token);
}

void LanObjectServer::handle_events(const HttpRequest& req, HttpResponse& res) {
    if (!check_auth(req)) { res.text(401, "unauthorized"); return; }

    // Path shape: /events/<id>/(manifest.json|event.json|init.mp4|segments/<seq>.m4s)
    const std::string rest = req.path.substr(kEventsPrefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) { res.text(404, "not found"); return; }
    const std::string id   = rest.substr(0, slash);
    const std::string tail = rest.substr(slash + 1);

    // Copy what's needed and release the lock before any disk read — the
    // same discipline SegmentCache itself uses, for the same reason: a
    // multi-megabyte read must not stall every other satellite's request.
    std::string manifest_json, event_json, markers_json;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (id.empty() || id != m_event_id) { res.text(404, "not found"); return; }
        manifest_json = m_manifest_json;
        event_json    = m_event_json;
        markers_json  = m_markers_json;
    }

    if (tail == "manifest.json") { res.json(manifest_json); return; }
    if (tail == "event.json")    { res.json(event_json);    return; }
    if (tail == "markers.json") {
        // Unlike manifest.json, nothing seeds this at event start — Session
        // only ever publishes it from add_marker(), so "no marker dropped
        // yet" is a real, honest 404, the same as a cloud decoder would see.
        if (markers_json.empty()) { res.text(404, "not found"); return; }
        res.json(markers_json);
        return;
    }

    if (tail == "init.mp4") {
        auto init = m_cache->load_init();
        if (!init) { res.text(404, "not found"); return; }
        res.content_type = "video/mp4";
        res.body.assign(init->begin(), init->end());
        return;
    }

    if (tail.compare(0, kSegmentsInfix.size(), kSegmentsInfix) == 0) {
        std::string fname = tail.substr(kSegmentsInfix.size());
        const std::string ext = ".m4s";
        if (fname.size() > ext.size() &&
            fname.compare(fname.size() - ext.size(), ext.size(), ext) == 0) {
            const std::string digits = fname.substr(0, fname.size() - ext.size());
            uint64_t seq = 0;
            bool ok = !digits.empty();
            for (char c : digits) if (c < '0' || c > '9') { ok = false; break; }
            if (ok) {
                try { seq = std::stoull(digits); } catch (...) { ok = false; }
            }
            if (ok) {
                auto media = m_cache->load(seq);
                if (media) {
                    res.content_type = "video/mp4";
                    res.body.assign(media->begin(), media->end());
                    return;
                }
                // A real, in-range segment that has already aged out of the
                // retention window (or an unconfirmed one still only in the
                // spool) — a genuine "not here", not a malformed request.
                res.text(404, "not found");
                return;
            }
        }
    }
    res.text(404, "not found");
}

void LanObjectServer::handle_live(const HttpRequest& req, HttpResponse& res) {
    if (!check_auth(req)) { res.text(401, "unauthorized"); return; }
    std::string json;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        json = m_live_json;
    }
    // Nothing published yet (server just started, no event has ever begun
    // under it) — a genuine "no live pointer", not a malformed request.
    if (json.empty()) { res.text(404, "not found"); return; }
    res.json(json);
}

void LanObjectServer::on_live_published(std::string json) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_live_json = std::move(json);
}

void LanObjectServer::on_markers_published(std::string json) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_markers_json = std::move(json);
}

void LanObjectServer::on_event_started(const std::string& event_id,
                                       const std::string& event_json,
                                       const std::vector<uint8_t>& init_bytes) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_event_id = event_id;
        m_event_json = event_json;
        m_manifest_json.clear();
        // A new event's markers start from nothing — the previous event's
        // are not meaningful under a different event id, the same reasoning
        // the manifest reset above and SegmentCache::set_event() both apply.
        m_markers_json.clear();
    }
    // Discards whatever the previous event retained — exactly
    // SegmentCache::set_event()'s own reasoning: a new event means a new
    // sequence space, and keeping the old one's segments around under it
    // would only ever be served to the wrong request by mistake.
    m_cache->set_event(event_id);
    m_cache->store_init(init_bytes);
}

void LanObjectServer::on_segment_confirmed(uint64_t seq,
                                           const std::vector<uint8_t>& bytes) {
    // Our own freshly-produced bytes, not a download — nothing to verify
    // against, unlike the decoder's own store(), which checks a downloaded
    // segment against the manifest's checksum before trusting it.
    m_cache->store(seq, bytes);
    if ((int)m_cache->count() > m_cfg.max_cached_segments) {
        const uint64_t lowest = m_cache->lowest_seq();
        const size_t excess = m_cache->count() - (size_t)m_cfg.max_cached_segments;
        m_cache->prune_below(lowest + (uint64_t)excess);
    }
}

void LanObjectServer::on_manifest_published(std::string json) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_manifest_json = std::move(json);
}

size_t LanObjectServer::cached_count() const { return m_cache->count(); }

} // namespace multisite
