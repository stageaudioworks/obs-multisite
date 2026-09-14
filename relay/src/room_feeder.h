// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// room_feeder.h — one room's receive side, shared by every destination.
//
// There is exactly ONE of these per room, however many places the event is
// being sent to. Each destination having its own poller would multiply the
// bucket reads (and, on a store that bills egress, the bill) by the number of
// destinations, for identical bytes. So the feeder downloads once into one
// verified cache and every relay reads from there.
//
// The receive logic itself is not reimplemented here: DecoderSession already
// does discovery, download-ahead, checksum verification, and the
// live/ended/interrupted classification, and it is the same code a campus
// runs. A second implementation would be a second thing to be subtly wrong.
// This adds only what a relay needs and a player does not: keeping the
// download window parked over the delayed position the destinations are
// actually reading from.
//
#include "decoder_session.h"
#include "event_catalog.h"
#include "fallback_transport.h"
#include "lan_transport.h"
#include "model.h"
#include "s3_transport.h"

#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace multisite_relay {

struct FeederConfig {
    multisite::S3Config storage;
    // Empty host means not configured — see ConfigStore::LanConfig. May be
    // set alongside `storage` (LAN-preferred, cloud-fallback), or alone for a
    // relay that never touches the bucket for the live feed at all.
    multisite::LanTransportConfig lan;
    std::string room_id = "main-auditorium";
    std::string cache_dir = "/data/cache";
    int poll_interval_ms = 3000;
    // Enough to cover the largest delay any destination is using, plus room to
    // run ahead. Set from the configured delays at startup.
    int buffer_minutes = 15;
    int stale_after_ms = 600000;      // the same 10 minutes a campus uses
    // Follow one specific event instead of whatever live.json names. This is
    // what makes a rebroadcast possible: DecoderSession already knows how to
    // pin an event (§7.5), so playing a past event needs no new receive path.
    std::string pinned_event_id;
};

// A consistent view of the room, taken under one lock so a caller cannot see
// the live edge from one moment and the audio layout from another.
struct RoomSnapshot {
    multisite::RoomState room = multisite::RoomState::Unknown;
    std::string event_id;
    // Shaped as a Manifest because that is what plan_stream() reads. Video and
    // segment duration come from the event descriptor (static, written once);
    // the audio layout and the sequence numbers come from the live manifest.
    multisite::Manifest manifest;
    bool     have_event_info = false;
    uint64_t latest_seq = 0;
    uint64_t first_available_seq = 0;
    double   segment_duration_s = 6.0;
    std::string last_error;
    uint64_t downloaded = 0;
    uint64_t checksum_failures = 0;
    // Whether the live feed is (or could be) coming over LAN rather than the
    // bucket — the same distinction the OBS decoder dock and the Pi appliance
    // already surface. lan_active means the LAN path is actually reachable
    // right now, not merely configured.
    bool lan_configured = false;
    bool lan_active = false;
};

class RoomFeeder {
public:
    explicit RoomFeeder(FeederConfig cfg);
    ~RoomFeeder();

    void start();
    void stop();

    RoomSnapshot snapshot() const;

    // Is this segment on disk and verified, ready to hand to ffmpeg?
    bool has_segment(uint64_t seq) const;
    std::optional<std::vector<uint8_t>> load_segment(uint64_t seq) const;
    std::optional<std::vector<uint8_t>> load_init() const;

    // Where the destinations are actually reading from. The download window is
    // parked here rather than at the live edge, or a relay sitting three
    // minutes back would find its segments already pruned.
    void set_lowest_reader(uint64_t seq);

    // ── Past events ────────────────────────────────────────────────────────
    // What the room has recorded, each with its own state. Uses the same
    // EventCatalog the decoder dock uses, so the relay's list and a campus's
    // list cannot disagree about which event is which.
    //
    // A refresh makes one request per event, so it is cached: a caller asking
    // for the list every second gets the same answer until it is stale.
    std::vector<multisite::EventSummary> events(bool force = false);

    // Exactly what a download consists of: the init segment followed by every
    // fragment, in order, each with its size.
    //
    // Size and content come from this one list on purpose. They were derived
    // separately at first, and disagreed — the size counted manifest.json and
    // event.json, which are not part of the video, so every download declared
    // about five kilobytes more than it sent and no browser could ever see it
    // finish.
    struct EventPart { std::string key; int64_t size = 0; };
    std::vector<EventPart> event_parts(const std::string& event_id,
                                       std::string& error) const;

    // Sum of the above. -1 when the store did not report sizes, in which case
    // the download still works but without a progress bar.
    int64_t event_byte_size(const std::string& event_id) const;

    // Streams a whole event as one fragmented MP4: the init segment followed
    // by every fragment in order, which is already a valid MP4 and needs no
    // re-muxing. `sink` returns false to abandon the download — a browser
    // closing the tab must not leave this fetching a five-gigabyte event.
    // Reads straight from the store, deliberately bypassing the relay's cache
    // so a download cannot evict what a live stream is about to need.
    bool stream_event(const std::string& event_id,
                      const std::function<bool(const uint8_t*, size_t)>& sink,
                      std::string& error) const;

    // The same, given a part list already fetched — so a caller that needed
    // the size does not pay for a second listing.
    bool stream_parts(const std::vector<EventPart>& parts,
                      const std::function<bool(const uint8_t*, size_t)>& sink,
                      std::string& error) const;

    // A one-off credential and connectivity check, so a mistyped key is
    // reported when it is entered rather than when an event starts.
    std::string check_storage();

private:
    void run();

    FeederConfig m_cfg;
    // Present only when cloud storage is configured — nullptr on a LAN-only
    // relay. Past events (list()-based) always go through this one, never
    // through LAN: LanObjectServer only ever holds the event in progress.
    std::unique_ptr<multisite::S3Transport> m_transport;
    std::unique_ptr<multisite::LanTransport> m_lan_transport;
    // Present only when both are configured; composes the two above for the
    // live feed the session reads.
    std::unique_ptr<multisite::FallbackTransport> m_fallback_transport;
    // Whichever of the three above the session and the live event.json fetch
    // actually read from. Never null once the constructor returns: the caller
    // (Service::reload()) only builds a RoomFeeder when at least one of cloud
    // or LAN is configured.
    multisite::Transport* m_active = nullptr;
    std::unique_ptr<multisite::DecoderSession> m_session;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_lowest_reader{0};
    std::atomic<bool> m_reader_set{false};

    // event.json is static for the life of an event, so it is fetched once
    // rather than on every poll.
    mutable std::mutex m_events_mtx;
    std::vector<multisite::EventSummary> m_events;
    int64_t m_events_checked_ms = 0;

    mutable std::mutex m_mtx;
    std::string  m_info_event_id;
    multisite::EventInfo m_info;
    bool m_have_info = false;
};

} // namespace multisite_relay
