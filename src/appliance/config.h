// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// config.h — everything the appliance needs to know, in one file on disk.
//
// The appliance has no operator sitting at it, so every setting must be
// reachable from the web UI and must survive a power cut. That makes the
// config file the whole state of the box: storage credentials, which room to
// follow, how the picture and sound leave the machine, and what to show when
// there is nothing to play.
//
// Field names deliberately match the OBS plugin's DecoderSettings, so the two
// halves of the project describe the same things the same way and a setting
// learned in one place means the same in the other.
//
#include <string>
#include <cstdint>

namespace multisite_player {

// What the box puts on screen when nothing is playing. A campus screen is
// visible to a congregation, so "whatever was last decoded" is not always the
// right answer — sometimes black is, and sometimes a holding slide is.
enum class IdleMode {
    Black,      // safest: nothing on the screen
    HoldFrame,  // freeze the last decoded picture (matches the pause behaviour)
    Splash,     // the identity screen: hostname, IP, room, state
    Image,      // a still supplied by the campus (idle_image_path)
};

const char* to_string(IdleMode m);
IdleMode    idle_mode_from_string(const std::string& s, IdleMode fallback);

struct Config {
    // ── Storage (S3-compatible) ──────────────────────────────────────────────
    // Which provider the web page's dropdown is showing (see
    // storage_providers.h) — "r2", "aws", "backblaze", "wasabi" or "custom".
    // Empty on a config saved before this existed; the page then falls back
    // to guessing from endpoint_host/r2_account_id (detect_provider()), so an
    // upgrade never loses or misrepresents a working setup.
    std::string storage_provider;
    std::string endpoint_host;          // blank when using an R2 account id
    std::string r2_account_id;
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";

    // ── LAN / direct delivery (PROJECT-SCOPE.md §8.7) ────────────────────────
    // A host (and port, and an optional shared token) typed in by hand, the
    // same as the OBS decoder dock's equivalent fields — not auto-discovered.
    // Empty host means not configured, which is the same convention an empty
    // bucket above already uses: both may be set together, for automatic
    // LAN-preferred, cloud-fallback delivery, or LAN alone with no cloud
    // credentials at all.
    std::string lan_host;
    int         lan_port = 9080;
    std::string lan_auth_token;

    // ── What to receive ──────────────────────────────────────────────────────
    std::string room_id = "main-auditorium";
    // Play this specific past event instead of following the room. Empty is
    // the normal, live case.
    std::string pinned_event_id;

    // ── Receive tuning (see DecoderConfig for what each one buys) ────────────
    int prebuffer_segments   = 2;
    int start_buffer_seconds = 60;
    int poll_interval_ms     = 3000;
    int keep_behind_segments = 200;
    int buffer_minutes       = 10;
    int max_cached_segments  = 2000;
    int stale_after_ms       = 600000;
    // A USB SSD, not the SD card: the cache writes roughly 3 GB an hour and
    // would wear a card out. The install script points this at the SSD if it
    // finds one.
    std::string cache_dir = "/var/lib/multisite-player/cache";

    // ── Video output ─────────────────────────────────────────────────────────
    std::string drm_card;               // blank = first card with a connected output
    std::string connector;              // blank = first connected connector (e.g. "HDMI-A-1")
    // 0 means "whatever the display says it prefers", which is the right
    // default for a screen nobody has measured.
    int  out_width  = 0;
    int  out_height = 0;
    int  out_fps    = 0;
    // Which tile of a composited feed to put on screen, in reading order
    // (left to right, then top to bottom), or -1 for the whole picture. The
    // encoder declares the layout in event.json (Phase 10); a box following a
    // 2x1 room can show one of the two pictures full-screen rather than the
    // wide composite with bars. A layout with fewer tiles than this falls back
    // to the whole picture, which is recoverable by hand where a wrongly
    // cropped one is not obviously wrong at all.
    int  tile_index = -1;

    IdleMode    idle_mode = IdleMode::Splash;
    std::string idle_image_path;

    // ── Audio output ─────────────────────────────────────────────────────────
    bool        audio_enabled = true;
    // ALSA device name. "default" follows the system; the UI lists what the
    // box actually has.
    std::string alsa_device = "default";
    // 0 = take the channel count from the feed. HDMI carries up to 8 channels
    // of LPCM, which is what makes packed multi-channel work on the cheap tier.
    int         audio_channels = 0;
    // Which audio track to put to air. OBS publishes up to six, and an event
    // carries every one it was told to; a campus plays one of them.
    int         audio_track = 0;

    // ── Control surface ──────────────────────────────────────────────────────
    int         web_port = 8080;
    // Bound to every interface by design: the operator is on a phone on the
    // church network, not on this box.
    std::string web_bind = "0.0.0.0";

    // ── Behaviour on power-up ────────────────────────────────────────────────
    // An appliance that needs someone to press Play after a power cut is not
    // an appliance. Off only for a box being commissioned.
    bool auto_play = true;
    // Sit this far behind live, in seconds. 0 rides the live edge.
    double delay_from_live_s = 0.0;
    // Refuse control changes from the UI until unlocked — the same guard the
    // dock has, for a tablet left on a music stand mid-event.
    bool locked = false;

    // ── Remote access ────────────────────────────────────────────────────────
    // A box at the back of a hall cannot be reached from the office without
    // somebody driving to the campus, and a church network rarely allows an
    // inbound port-forward. Two optional tools fix that, and neither is
    // required: leave both blank and the player behaves exactly as it did
    // before, and the splash says nothing about remote access at all.
    //
    //   • ZeroTier puts the box on a private network that follows it. The key
    //     is the sixteen hex digits shown in ZeroTier Central; the box joins
    //     that network and takes an address on it, which is the address the
    //     splash calls the remote access address.
    //   • cloudflared publishes this control page on a public hostname with no
    //     port-forward and no static IP. The token is the one the Cloudflare
    //     dashboard issues for the tunnel.
    std::string zerotier_network_id;
    // Secret, like the bucket key: never sent back to the browser.
    std::string cloudflared_token;

    // ── The sound on the network (AES67) ─────────────────────────────────────
    // AES67 is a separate install — scripts/player/merging-aes67.sh builds
    // Merging's kernel module and the GPL aes67-daemon — and it registers a
    // sound card of its own. What this player can do about it is keep its
    // source set up the way it should be, publish what it is, and switch it.
    //
    // Off by default, deliberately: a box with no AES67 stack installed has no
    // business spending its life reporting that it cannot find one, and every
    // other install is the majority.
    bool        aes67_manage = false;
    // Blank means "whatever the daemon's own configuration names", which is what
    // a box that has never been edited should get. Set here to publish to a
    // different multicast group.
    std::string aes67_address;
    // The width of the published stream. Eight by default — the width the open
    // stack was proven at, and one most consoles are happy to receive — and it
    // is fixed rather than following the feed,
    // because receiving consoles configure against a stream's width and would
    // have to re-learn it every time an event carried a different number of
    // tracks. (aes67.h carries the same default as kAes67DefaultChannels; it is
    // repeated rather than included so that this header, which everything
    // includes, does not drag a JSON library in behind it.)
    int         aes67_channels = 8;
    // Where the sound was going before the network output took it over, so that
    // switching the network output off puts the room back where it was rather
    // than leaving it pointed at a card that is no longer carrying anything.
    // Internal: no interface shows it, and it survives a power cut between the
    // two clicks.
    std::string aes67_previous_device;

    bool cloud_configured() const {
        return !bucket.empty() &&
               (!endpoint_host.empty() || !r2_account_id.empty());
    }
    bool lan_configured() const { return !lan_host.empty(); }
    // Whether there is any way to reach a room at all — the gate the player
    // checks before it will try to build a session. cloud_configured() alone
    // used to BE this gate; a LAN-only box (no cloud credentials at all) is
    // exactly why it no longer is.
    bool configured() const { return cloud_configured() || lan_configured(); }

    // Read from `path`. Missing file is not an error: a freshly installed box
    // has no config and must still boot far enough to show its IP address so
    // somebody can go and give it one.
    bool load(const std::string& path, std::string& error);

    // Written to a temporary file and renamed, so a power cut during a save
    // cannot leave a half-written config that stops the box booting. Mode 0600:
    // it holds the bucket's secret key.
    bool save(const std::string& path, std::string& error) const;
};

// The path the player uses unless --config says otherwise.
const char* default_config_path();

} // namespace multisite_player
