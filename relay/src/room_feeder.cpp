// SPDX-License-Identifier: GPL-3.0-or-later
#include "room_feeder.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>

namespace multisite_relay {

using namespace multisite;

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

bool cloud_configured(const S3Config& c) {
    return !c.bucket.empty() &&
           (!c.endpoint_host.empty() || !c.r2_account_id.empty());
}
} // namespace

RoomFeeder::RoomFeeder(FeederConfig cfg) : m_cfg(std::move(cfg)) {
    if (cloud_configured(m_cfg.storage))
        m_transport = std::make_unique<S3Transport>(m_cfg.storage);
    if (!m_cfg.lan.host.empty())
        m_lan_transport = std::make_unique<LanTransport>(m_cfg.lan);

    if (m_transport && m_lan_transport) {
        m_fallback_transport =
            std::make_unique<FallbackTransport>(*m_lan_transport, *m_transport);
        m_active = m_fallback_transport.get();
    } else if (m_lan_transport) {
        m_active = m_lan_transport.get();
    } else {
        // Expected non-null: Service::reload() only builds a RoomFeeder once
        // ConfigStore::configured() is true, so cloud alone is the only
        // remaining case here.
        m_active = m_transport.get();
    }

    // ...but "expected" is not "guaranteed", and the cost of being wrong was a
    // segfault on the first poll rather than a message. It happened once:
    // configured() briefly counted a bare pairing as a storage path, so a
    // relay that had paired but chosen no bucket got here with nothing. The
    // gate is fixed; this makes the next way of getting it wrong say so
    // instead of taking the container down mid-event.
    if (!m_active) {
        rlog_error("no way to read the media: neither a bucket nor a LAN host "
                   "is configured. Nothing will be sent.");
        return;   // m_session stays null; run() and every accessor check it
    }

    DecoderConfig dc;
    dc.room_id = m_cfg.room_id;
    dc.cache_dir = m_cfg.cache_dir;
    dc.buffer_minutes = m_cfg.buffer_minutes;
    dc.stale_after_ms = m_cfg.stale_after_ms;
    // The relay never scrubs backwards, but it does sit behind live, so what
    // it needs kept is whatever the furthest-behind destination has not
    // reached yet, with margin for a restart.
    dc.keep_behind_segments = 100;
    dc.prebuffer_segments = 0;
    // The relay runs its own delay buffer (delay_s) and must not also wait for
    // a start-buffer window; it starts as soon as its first segment is ready.
    dc.start_buffer_seconds = 0;
    dc.pinned_event_id = m_cfg.pinned_event_id;
    m_session = std::make_unique<DecoderSession>(dc, *m_active);
}

RoomFeeder::~RoomFeeder() { stop(); }

std::vector<multisite::Marker> RoomFeeder::markers() const {
    return m_session ? m_session->markers() : std::vector<multisite::Marker>{};
}

std::vector<multisite::Marker> RoomFeeder::event_cues(
    const std::string& event_id, std::string& error) const {
    if (!m_transport) {
        error = "a cue list needs cloud storage — a LAN hub only holds the "
                "event in progress";
        return {};
    }
    const std::string prefix = multisite::event_prefix_for(event_id);
    std::vector<multisite::MarkerList> parts;

    // The encoder's own file, then one per author, merged — the same rule the
    // decoder applies to the live event, so the two agree about a recording's
    // cues.
    auto mk = m_transport->get(prefix + "markers.json");
    if (mk.success) {
        try {
            parts.push_back(multisite::MarkerList::from_json(
                std::string(mk.body.begin(), mk.body.end())));
        } catch (...) {}
    }
    auto ls = m_transport->list(multisite::cues_prefix_for(event_id), "", "", 1000);
    if (ls.success) {
        for (const auto& e : ls.keys) {
            if (e.key.size() < 6 ||
                e.key.compare(e.key.size() - 5, 5, ".json") != 0)
                continue;
            auto cg = m_transport->get(e.key);
            if (!cg.success) continue;
            try {
                parts.push_back(multisite::MarkerList::from_json(
                    std::string(cg.body.begin(), cg.body.end())));
            } catch (...) {}
        }
    }
    if (parts.empty()) return {};
    return multisite::merge_markers(std::move(parts)).markers;
}

std::string RoomFeeder::check_storage() {
    // Cloud, when there is one, is the more informative check: self_test()
    // actually verifies the credentials rather than just reachability, and a
    // relay with both configured cares most about the path event browsing and
    // download rely on.
    if (m_transport) return m_transport->self_test();
    if (m_lan_transport) {
        m_lan_transport->get(live_pointer_key(m_cfg.room_id));
        if (!m_lan_transport->last_request_reached_server())
            return "Could not reach the encoder at " + m_cfg.lan.host + ":" +
                   std::to_string(m_cfg.lan.port) + ".";
        return {};
    }
    return "Storage has not been set up yet.";
}

void RoomFeeder::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void RoomFeeder::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
}

void RoomFeeder::set_lowest_reader(uint64_t seq) {
    m_lowest_reader = seq;
    m_reader_set = true;
}

void RoomFeeder::run() {
    // Nothing to read from. The constructor has already said why.
    if (!m_session) return;
    int64_t next_poll = 0;
    while (m_running) {
        const int64_t t = now_ms();

        if (t >= next_poll) {
            next_poll = t + m_cfg.poll_interval_ms;
            m_session->poll(t);

            // event.json is written once at Go Live and carries what the codec
            // gate needs: the video codec and the segment duration. Fetch it
            // when the event changes, not on every pass.
            const std::string ev = m_session->event_id();
            bool need = false;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                need = !ev.empty() && (ev != m_info_event_id || !m_have_info);
            }
            if (need) {
                auto r = m_active->get(event_prefix_for(ev) + "event.json");
                if (r.success) {
                    try {
                        EventInfo info = EventInfo::from_json(
                            std::string(r.body.begin(), r.body.end()));
                        std::lock_guard<std::mutex> lk(m_mtx);
                        m_info = std::move(info);
                        m_info_event_id = ev;
                        m_have_info = true;
                    } catch (...) {
                        // Left unset: a snapshot without it reports that it
                        // does not know the video format, and plan_stream
                        // refuses rather than guessing.
                    }
                }
            }

            // Park the download window where the destinations are reading,
            // not at the live edge. Without this the relay would be fetching
            // segments three minutes newer than the ones it needs, and the
            // ones it does need would age out of the cache underneath it.
            if (m_reader_set) {
                const uint64_t want = m_lowest_reader.load();
                if (want >= m_session->earliest_available() &&
                    m_session->playback_head() != want)
                    m_session->seek(want);
            }
        }

        // Downloads run flat out until the buffer target is met, which is what
        // banks content ahead of a dropout rather than trickling along at
        // playback speed.
        const int got = m_session->pump_downloads(4);
        if (got == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

std::vector<EventSummary> RoomFeeder::events(bool force) {
    // Browsing past events needs list(), which LAN does not serve — see
    // lan_transport.h. A LAN-only relay has nothing to show here; it can
    // still relay the live feed, just not the history behind it.
    if (!m_transport) return {};

    {
        std::lock_guard<std::mutex> lk(m_events_mtx);
        // One request per event, so this is not something to do on every poll
        // of a page that refreshes every second.
        if (!force && !m_events.empty() &&
            now_ms() - m_events_checked_ms < 30000)
            return m_events;
    }

    CatalogConfig cc;
    cc.room_id = m_cfg.room_id;
    cc.stale_after_ms = m_cfg.stale_after_ms;
    EventCatalog cat(cc, *m_transport);
    cat.refresh();
    auto found = cat.events();

    std::lock_guard<std::mutex> lk(m_events_mtx);
    m_events = std::move(found);
    m_events_checked_ms = now_ms();
    return m_events;
}

std::vector<RoomFeeder::EventPart> RoomFeeder::event_parts(
        const std::string& event_id, std::string& error) const {
    if (!m_transport) {
        error = "This relay has no cloud storage configured, and past "
                "events need it.";
        return {};
    }
    const std::string prefix = event_prefix_for(event_id);
    const std::string seg_prefix = prefix + "segments/";

    std::vector<EventPart> init_part, segments;
    std::string token;
    do {
        auto r = m_transport->list(prefix, "", token, 1000);
        if (!r.success) {
            error = "Could not read that event from storage.";
            return {};
        }
        for (const auto& e : r.keys) {
            // Only the media. manifest.json and event.json live under the same
            // prefix and are not part of the recording.
            if (e.key == prefix + "init.mp4")
                init_part.push_back({ e.key, e.size });
            else if (e.key.rfind(seg_prefix, 0) == 0 &&
                     e.key.size() > seg_prefix.size())
                segments.push_back({ e.key, e.size });
        }
        token = r.next_continuation_token;
    } while (!token.empty());

    if (init_part.empty()) {
        error = "That event has no opening data and cannot be downloaded.";
        return {};
    }
    // Segment names are zero-padded, so name order is play order.
    std::sort(segments.begin(), segments.end(),
              [](const EventPart& a, const EventPart& b) { return a.key < b.key; });

    std::vector<EventPart> out;
    out.reserve(segments.size() + 1);
    out.push_back(init_part.front());
    out.insert(out.end(), segments.begin(), segments.end());
    return out;
}

int64_t RoomFeeder::event_byte_size(const std::string& event_id) const {
    std::string error;
    const auto parts = event_parts(event_id, error);
    if (parts.empty()) return -1;
    int64_t total = 0;
    for (const auto& p : parts) {
        if (p.size < 0) return -1;      // the store did not say
        total += p.size;
    }
    return total;
}

bool RoomFeeder::stream_parts(
        const std::vector<EventPart>& parts,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        std::string& error) const {
    if (!m_transport) {
        error = "This relay has no cloud storage configured, and past "
                "events need it.";
        return false;
    }
    for (const auto& part : parts) {
        auto obj = m_transport->get(part.key);
        if (!obj.success) {
            // Nothing can be done about this mid-stream: the length has
            // already been promised, so the download will be short and the
            // browser will report it as failed. That is the honest outcome.
            error = "The event could not be read all the way through.";
            return false;
        }
        if (!sink(obj.body.data(), obj.body.size())) return true;  // gave up
    }
    return true;
}

bool RoomFeeder::stream_event(
        const std::string& event_id,
        const std::function<bool(const uint8_t*, size_t)>& sink,
        std::string& error) const {
    const auto parts = event_parts(event_id, error);
    if (parts.empty()) return false;
    return stream_parts(parts, sink, error);
}

RoomSnapshot RoomFeeder::snapshot() const {
    RoomSnapshot s;
    s.room = m_session->room_state();
    s.event_id = m_session->event_id();
    s.latest_seq = m_session->live_edge();
    s.first_available_seq = m_session->earliest_available();
    s.last_error = m_session->last_error();

    const auto& st = m_session->stats();
    s.downloaded = st.downloaded;
    s.checksum_failures = st.checksum_failures;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_have_info && m_info_event_id == s.event_id) {
            s.have_event_info = true;
            s.manifest.video = m_info.video;
            if (m_info.segment_duration_s > 0.1)
                s.segment_duration_s = m_info.segment_duration_s;
            // Only as a fallback: the live manifest is what a decoder trusts
            // for the audio layout, so it wins when it has anything to say.
            s.manifest.audio_tracks = m_info.audio_tracks;
        }
    }

    auto live_tracks = m_session->audio_layout();
    if (!live_tracks.empty()) s.manifest.audio_tracks = std::move(live_tracks);

    s.manifest.latest_seq = s.latest_seq;
    s.manifest.first_available_seq = s.first_available_seq;
    s.manifest.event_id = s.event_id;

    s.lan_configured = m_lan_transport != nullptr;
    // Not "did the last request come from LAN" but "is the LAN path
    // currently healthy" — see FallbackTransport::last_get_was_primary()'s
    // own doc comment. On a LAN-only relay it is simply whether the last
    // request reached the encoder at all.
    s.lan_active = m_fallback_transport
                       ? m_fallback_transport->last_get_was_primary()
                       : (m_lan_transport &&
                          m_lan_transport->last_request_reached_server());
    return s;
}

bool RoomFeeder::has_segment(uint64_t seq) const {
    return m_session->cache().has(seq);
}

std::optional<std::vector<uint8_t>> RoomFeeder::load_segment(uint64_t seq) const {
    return m_session->cache().load(seq);
}

std::optional<std::vector<uint8_t>> RoomFeeder::load_init() const {
    return m_session->cache().load_init();
}

} // namespace multisite_relay
