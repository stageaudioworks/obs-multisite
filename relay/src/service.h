// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// service.h — the whole relay, assembled.
//
// Holds the configuration, the one feeder per room, and exactly one
// RelaySession per destination. Sessions are created here and nowhere else,
// which is what makes "two processes pushing to the same destination" not a
// race to be guarded against but a thing that cannot be expressed: starting a
// destination sets a flag on the session that already exists.
//
#include "config_store.h"
#include "reporter.h"
#include "relay_session.h"
#include "room_feeder.h"

#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_relay {

struct ServiceStatus {
    // Whether the relay can reach the room at all, over either the bucket or
    // LAN. Cloud alone used to be this whole gate; it is not any more.
    bool        configured = false;
    // Whether the live feed is being read over LAN rather than the bucket —
    // see RoomSnapshot's identical fields, which these are copied from.
    bool        lan_configured = false;
    bool        lan_active = false;
    std::string room_id;
    std::string room_state;       // "live", "offline", "ended", "interrupted"
    std::string room_state_text;  // plain language for the banner
    std::string event_id;
    std::string storage_error;
    // What the main site says it is sending, so the UI can offer sound feeds
    // by name and warn before anything is started.
    std::vector<std::string> audio_labels;
    std::string video_summary;    // "1920x1080 H.264"
    // Whether ANY destination could carry this event. The two protocols do
    // not accept the same video, so these are three separate facts and not one:
    // nothing can take it, everything can, or some can and some cannot.
    bool        can_send = false;
    // Why nothing can. Empty when something can.
    std::string cannot_send_reason;
    // Why some can and some cannot — an HEVC event, in practice, which no
    // streaming site will take but an SRT destination carries unchanged. Kept
    // apart from the above because it is information rather than an obstacle,
    // and showing it as one would have an operator stop setting up a stream
    // that was going to work.
    std::string send_note;
    // The number an operator needs when several destinations share one VPS's
    // upload: what is going out in total, right now.
    double      total_out_kbps = 0;
    std::vector<RelayStatus> destinations;
};

class Service {
public:
    ~Service();

    std::string start(const std::string& db_path);
    void stop();

    ConfigStore& config() { return m_cfg; }

    // The relay's side of Multisite Cloud: pairing, the heartbeat, and the
    // credential lifecycle storage reads through when the provider is
    // Multisite Cloud. Always constructed; it sends nothing until paired.
    Reporter& reporter() { return m_reporter; }

    // Brings the running relay into line with the database. Rebuilds the
    // downloader only if the bucket or the room actually changed; otherwise
    // adds, removes and updates sessions in place, so a destination that is
    // on air is not disturbed because a different one was edited.
    void reload();

    ServiceStatus status() const;

    // Test the stored credentials. Empty string means they work.
    std::string check_storage();

    void set_enabled(int64_t id, bool on);

    // ── Past events ──────────────────────────────────────────────────────────
    std::vector<multisite::EventSummary> events(bool force = false);
    // Whether this event may be downloaded or rebroadcast: it must exist, and
    // it must not be the one currently on air. Returns a reason, or empty.
    std::string check_event_is_finished(const std::string& event_id);
    RoomFeeder* feeder();

    // ── Rebroadcast (proof of concept) ───────────────────────────────────────
    // Plays a finished event out to a destination as though it were live.
    // One at a time on purpose: this is the first version of the idea, and a
    // church running several at once needs answers about bandwidth and
    // scheduling that do not exist yet.
    //
    // `start_cue` and `end_cue` name two of the event's cues, used as in and
    // out points so the replay is an excerpt rather than the whole recording.
    // Empty means "no bound", which is what the replay always did.
    std::string start_rebroadcast(const std::string& event_id, int64_t dest_id,
                                  const std::string& start_cue = "",
                                  const std::string& end_cue = "");
    void        stop_rebroadcast();
    bool        rebroadcasting() const;
    RelayStatus rebroadcast_status() const;
    std::string rebroadcast_event() const;

    // The cues of a specific past event, for the rebroadcast UI's in/out
    // pickers. Reads the store directly; cloud only.
    std::vector<multisite::Marker> event_cues(const std::string& event_id,
                                              std::string& error) const;

private:
    void supervise();
    // Typed keys, or the pairing's — never a mixture, and never a fallback
    // from one to the other.
    multisite::S3Config effective_storage() const;
    void sync_destinations_locked(const std::vector<Destination>& dests,
                                  const RoomSettings& room);

    ConfigStore m_cfg;
    mutable std::mutex m_mtx;
    std::unique_ptr<RoomFeeder> m_feeder;
    std::map<int64_t, std::unique_ptr<RelaySession>> m_sessions;
    // What the current downloader was built from, so a save that changes
    // nothing about the bucket or the LAN path does not rebuild it.
    Reporter m_reporter;
    multisite::S3Config m_feeder_storage;
    ConfigStore::LanConfig m_feeder_lan;
    std::string         m_feeder_room;

    // A rebroadcast gets its own downloader, pinned to the event it is
    // playing, so it never disturbs the live room's download window.
    std::unique_ptr<RoomFeeder>   m_rebroadcast_feeder;
    std::unique_ptr<RelaySession> m_rebroadcast;
    std::string                   m_rebroadcast_event;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::string m_storage_error;
};

} // namespace multisite_relay
