// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// decoder_settings.h — storage credentials for the satellite, stored once per
// machine rather than per source.
//
// They used to live only in each source's own settings, which meant OBS lost
// them if it exited uncleanly, and meant re-entering credentials for every
// additional source (each packed audio track will eventually be its own
// source). Now they are saved alongside OBS's plugin config, and a source
// falls back to them whenever its own fields are blank.
//
#include <string>

namespace multisite_obs {

struct DecoderSettings {
    // Which provider the dock's dropdown is showing (see storage_providers.h)
    // — "r2", "aws", "backblaze", "wasabi" or "custom". Empty on a config
    // saved before this existed; the dock then falls back to
    // detect_provider() against whatever endpoint_host / r2_account_id is
    // already there, so an upgrade never loses or misrepresents a working
    // setup.
    std::string storage_provider;
    std::string endpoint_host;      // blank when using an R2 account id
    std::string r2_account_id;
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";
    std::string room_id = "main-auditorium";
    // Kept above zero on purpose: at zero the playhead sits at the live edge,
    // so a hold-and-resume has nothing new to play and the picture appears
    // frozen for up to a segment. A small reserve also absorbs network jitter.
    int    prebuffer_segments = 2;
    // Seconds of programme to bank before playback starts, so the picture
    // never chases the live edge. 0 restores "start immediately".
    int    start_buffer_seconds = 60;
    int    poll_interval_ms = 3000;
    int    keep_behind_segments = 200;
    // How far ahead to download, in minutes of programme. This is the figure
    // that decides how long the campus could keep broadcasting if the
    // connection dropped, so it is worth setting as high as the link allows.
    int    buffer_minutes = 10;

    // LAN / direct delivery (PROJECT-SCOPE.md §8.7) — a host:port typed in
    // here, by hand, rather than discovered automatically (out of scope for
    // now). Empty host means LAN is not configured at all: the source falls
    // back to cloud-only, exactly as it always has. Both may be configured
    // together (LAN preferred, cloud as fallback per request — see
    // fallback_transport.h) or LAN alone (cloud_configured() false).
    std::string lan_host;
    int         lan_port = 9080;
    std::string lan_auth_token;

    void load();
    void save() const;

    bool cloud_configured() const {
        return !bucket.empty() &&
               (!endpoint_host.empty() || !r2_account_id.empty());
    }
    bool lan_configured() const { return !lan_host.empty(); }
    // Whether there is ANY way to reach a room at all — the gate a source
    // checks before starting. cloud_configured() alone used to BE this gate;
    // LAN-only satellites are exactly why it no longer is.
    bool configured() const { return cloud_configured() || lan_configured(); }
};

// The machine-wide settings, shared by every multisite source and the dock.
DecoderSettings& decoder_settings();
// A snapshot, for a reader that is not on OBS's UI thread — the remote-control
// page polls from a network thread while the dock edits here.
DecoderSettings decoder_settings_copy();
void set_decoder_settings(const DecoderSettings& s);

} // namespace multisite_obs
