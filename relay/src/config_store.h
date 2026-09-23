// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// config_store.h — everything the relay must remember, in one SQLite file.
//
// One file on a mounted volume is the whole persistent state: which bucket to
// read, which destinations to send to, and what each was told to send. No
// external database, nothing to administer, and a backup is a file copy.
//
// On secrets, plainly: stream keys are stored as they are given, in a file
// created 0600 on a volume the church controls. Encrypting them in the same
// file as the key that decrypts them would be theatre — anyone who can read
// the file can read both. What is done instead is narrower and real: a key is
// never returned by the API, never written to the log, and never included in
// the arguments any status page shows. Stage 3's OAuth refresh token is a
// different matter and is encrypted, because there the secret outlives the
// session and grants far more than one broadcast.
//
#include "relay_send.h"
#include "s3_transport.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace multisite_relay {

struct RoomSettings {
    std::string room_id = "main-auditorium";
    // The default for destinations that do not set their own. Three minutes:
    // enough to absorb a dropout at the main site without the public stream
    // noticing, and short enough that anyone watching alongside the room is
    // not badly out of step.
    int default_delay_s = 180;
};

class ConfigStore {
public:
    ~ConfigStore();

    // Opens (creating if needed) and brings the schema up to date. Returns a
    // human-readable error, or empty on success.
    std::string open(const std::string& path);

    // ── Secrets and small settings ───────────────────────────────────────────
    // Used for the operator login. Kept generic rather than growing a column
    // per field, because these are read once at sign-in and never iterated.
    std::string get_secret(const std::string& key) const;
    void        set_secret(const std::string& key, const std::string& value);

    // ── Storage ──────────────────────────────────────────────────────────────
    multisite::S3Config storage() const;
    void set_storage(const multisite::S3Config& c);
    bool storage_configured() const;

    // Which entry the settings page's provider dropdown is showing — "r2",
    // "aws", "backblaze", "wasabi" or "custom" (see storage_providers.h).
    // Empty on a database saved before this existed; api.cpp falls back to
    // detect_provider() in that case, the same rule the appliance uses.
    std::string storage_provider() const;
    void set_storage_provider(const std::string& key);

    // ── Multisite Cloud pairing (PROJECT-SCOPE.md §8.2, ADR-0001/0002) ──────
    // The relay is the third CloudIdentity host, after the OBS plugin and the
    // campus player, and it is its own role: a relay on a machine that also
    // runs an encoder pairs separately and shares nothing with it (ADR-0001).
    //
    // When the provider is "multisite-cloud" the collector supplies the
    // bucket, the endpoint and expiring credentials, and the typed storage
    // fields above are not consulted at all — there is no fallback between the
    // two, because a relay reading from a bucket nobody paired it to is the
    // state Phase 12 exists to make unrepresentable.
    struct PairingConfig {
        std::string collector_url;     // empty means not paired
        std::string appliance_id;
        std::string appliance_token;   // never leaves the container
        // Minted once and kept, so re-pairing the same relay is recognisable
        // as the same box rather than a new one each time.
        std::string device_id;
    };
    PairingConfig pairing() const;
    void set_pairing(const PairingConfig& c);
    bool paired() const;
    // Forget the pairing without touching anything else — the Disconnect an
    // operator reaches for when a relay is moved between organisations.
    void clear_pairing();

    // ── LAN / direct delivery (PROJECT-SCOPE.md §8.7) ───────────────────────
    // A relay sitting on the same network as the encoder (or reachable over an
    // existing VPN) can read the live feed straight from it instead of round-
    // tripping through the bucket — the same capability the OBS decoder and
    // the Pi appliance already have. Past events still need the bucket: an
    // event that has finished is no longer being served by the encoder's own
    // LanObjectServer, which only ever holds the one currently in progress.
    struct LanConfig {
        std::string host;              // empty means not configured
        int         port = 9080;
        std::string auth_token;
    };
    LanConfig lan() const;
    void set_lan(const LanConfig& c);
    bool lan_configured() const { return !lan().host.empty(); }

    // Whether the relay can reach the room at all, over either path. This is
    // the gate that decides whether a downloader gets built, and it is why
    // storage_configured() alone stopped being enough: a LAN-only relay has
    // no bucket credentials whatsoever.
    // Is there any path to the media at all?
    //
    // A pairing counts only when Multisite Cloud is also the chosen PROVIDER.
    // Pairing alone does not: a relay can be paired for monitoring while still
    // reading a bucket whose keys were typed in, and counting that as a storage
    // path let a relay with a pairing and no bucket through this gate. The
    // feeder then built with an empty S3Config, and RoomFeeder's "guaranteed
    // non-null" active transport — guaranteed BY this function — was null.
    // It crashed on the first poll.
    bool configured() const {
        return storage_configured() || lan_configured() ||
               (storage_is_paired() && paired());
    }

    // Whether the operator chose Multisite Cloud as the storage provider, as
    // distinct from merely having paired. A relay can be paired for monitoring
    // while still reading a bucket whose keys were typed in — the pairing and
    // the provider are separate choices, exactly as they are on the player.
    //
    // Compares against provider_key(MultisiteCloud) rather than a literal: the
    // key is "multisite_cloud" and writing it out here is how a rename becomes
    // a silent no-match instead of a build error.
    bool storage_is_paired() const;

    // ── Room ─────────────────────────────────────────────────────────────────
    RoomSettings room() const;
    void set_room(const RoomSettings& r);

    // ── Destinations ─────────────────────────────────────────────────────────
    std::vector<Destination> destinations() const;
    std::optional<Destination> destination(int64_t id) const;
    // Returns the new id, or 0 with `error` set.
    int64_t add(const Destination& d, std::string& error);
    bool    update(const Destination& d, std::string& error);
    bool    remove(int64_t id);
    void    set_enabled(int64_t id, bool on);

private:
    std::string exec(const std::string& sql);

    sqlite3* m_db = nullptr;
    mutable std::mutex m_mtx;
};

} // namespace multisite_relay
