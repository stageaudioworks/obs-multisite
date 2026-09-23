// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_store.h"

#include "storage_providers.h"
#include "log.h"

#include <cstdint>
#include <sqlite3.h>
#include <sys/stat.h>

namespace multisite_relay {

namespace {

std::string text_col(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

void bind_text(sqlite3_stmt* st, int i, const std::string& s) {
    sqlite3_bind_text(st, i, s.c_str(), -1, SQLITE_TRANSIENT);
}

} // namespace

ConfigStore::~ConfigStore() {
    if (m_db) sqlite3_close(m_db);
}

std::string ConfigStore::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "unknown error";
        sqlite3_free(err);
        return e;
    }
    return {};
}

std::string ConfigStore::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (sqlite3_open(path.c_str(), &m_db) != SQLITE_OK)
        return "could not open " + path + ": " +
               (m_db ? sqlite3_errmsg(m_db) : "unknown error");

    // The file holds stream keys, so it is not world-readable even on a
    // volume somebody else can list.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);

    // WAL, so a read for the status page never waits behind a write.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA busy_timeout=3000;");

    std::string e = exec(
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS destinations ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  room_id TEXT NOT NULL,"
        "  url TEXT NOT NULL,"
        "  stream_key TEXT NOT NULL DEFAULT '',"
        "  audio_label TEXT NOT NULL DEFAULT '',"
        "  allow_transcode INTEGER NOT NULL DEFAULT 0,"
        "  enabled INTEGER NOT NULL DEFAULT 0,"
        "  delay_s INTEGER NOT NULL DEFAULT 0"
        ");");
    if (!e.empty()) return "could not prepare the database: " + e;

    // SRT arrived after the first release, so a relay that has been running
    // since then has a destinations table without these columns. Adding them
    // one at a time and ignoring the failure is the whole migration: SQLite
    // refuses a column that is already there, which on an up-to-date database
    // is every one of them, and that refusal is the success case rather than
    // something to report. The defaults are chosen so an RTMP destination
    // saved before any of this existed reads back meaning exactly what it
    // meant then.
    for (const char* col : { "srt_mode TEXT NOT NULL DEFAULT 'caller'",
                             "srt_passphrase TEXT NOT NULL DEFAULT ''",
                             "srt_latency_ms INTEGER NOT NULL DEFAULT 0" })
        exec(std::string("ALTER TABLE destinations ADD COLUMN ") + col + ";");

    return {};
}

// ── settings helpers ─────────────────────────────────────────────────────────

namespace {

std::string get_setting(sqlite3* db, const std::string& key,
                        const std::string& fallback = "") {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key=?;", -1,
                           &st, nullptr) != SQLITE_OK)
        return fallback;
    bind_text(st, 1, key);
    std::string out = fallback;
    if (sqlite3_step(st) == SQLITE_ROW) out = text_col(st, 0);
    sqlite3_finalize(st);
    return out;
}

void put_setting(sqlite3* db, const std::string& key, const std::string& value) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO settings(key,value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &st, nullptr) != SQLITE_OK)
        return;
    bind_text(st, 1, key);
    bind_text(st, 2, value);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

} // namespace

std::string ConfigStore::get_secret(const std::string& key) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return get_setting(m_db, key);
}

void ConfigStore::set_secret(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, key, value);
}

multisite::S3Config ConfigStore::storage() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    multisite::S3Config c;
    c.endpoint_host      = get_setting(m_db, "endpoint_host");
    c.r2_account_id      = get_setting(m_db, "r2_account_id");
    c.bucket             = get_setting(m_db, "bucket");
    c.access_key_id      = get_setting(m_db, "access_key_id");
    c.secret_access_key  = get_setting(m_db, "secret_access_key");
    c.region             = get_setting(m_db, "region", "auto");
    // Plain HTTP is refused by every hosted store, but a self-hosted MinIO on
    // the church's own LAN often has no certificate. Off by default, so it is
    // a deliberate choice rather than an accident.
    c.use_https          = get_setting(m_db, "use_https", "1") != "0";
    return c;
}

void ConfigStore::set_storage(const multisite::S3Config& c) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "endpoint_host", c.endpoint_host);
    put_setting(m_db, "r2_account_id", c.r2_account_id);
    put_setting(m_db, "bucket", c.bucket);
    put_setting(m_db, "access_key_id", c.access_key_id);
    put_setting(m_db, "secret_access_key", c.secret_access_key);
    put_setting(m_db, "region", c.region.empty() ? "auto" : c.region);
    put_setting(m_db, "use_https", c.use_https ? "1" : "0");
}

bool ConfigStore::storage_configured() const {
    const auto c = storage();
    return !c.bucket.empty() && !c.access_key_id.empty() &&
           (!c.endpoint_host.empty() || !c.r2_account_id.empty());
}

std::string ConfigStore::storage_provider() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return get_setting(m_db, "storage_provider");
}

bool ConfigStore::storage_is_paired() const {
    return storage_provider() ==
           multisite::provider_key(multisite::StorageProvider::MultisiteCloud);
}

ConfigStore::PairingConfig ConfigStore::pairing() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    PairingConfig c;
    c.collector_url   = get_setting(m_db, "collector_url");
    c.appliance_id    = get_setting(m_db, "appliance_id");
    c.appliance_token = get_setting(m_db, "appliance_token");
    c.device_id       = get_setting(m_db, "device_id");
    return c;
}

void ConfigStore::set_pairing(const PairingConfig& c) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "collector_url", c.collector_url);
    put_setting(m_db, "appliance_id", c.appliance_id);
    put_setting(m_db, "appliance_token", c.appliance_token);
    put_setting(m_db, "device_id", c.device_id);
}

bool ConfigStore::paired() const {
    const auto c = pairing();
    // All three together or none: a url from one pairing with the token from
    // another is a device the collector has never heard of (cloud_identity.h,
    // Enrolment).
    return !c.collector_url.empty() && !c.appliance_id.empty() &&
           !c.appliance_token.empty();
}

void ConfigStore::clear_pairing() {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "collector_url", "");
    put_setting(m_db, "appliance_id", "");
    put_setting(m_db, "appliance_token", "");
    // device_id is deliberately KEPT: it identifies this box, not this
    // pairing, so re-pairing after a move is recognisable as the same relay.
}

void ConfigStore::set_storage_provider(const std::string& key) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "storage_provider", key);
}

ConfigStore::LanConfig ConfigStore::lan() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    LanConfig c;
    c.host = get_setting(m_db, "lan_host");
    const std::string p = get_setting(m_db, "lan_port", "9080");
    try { c.port = std::stoi(p); } catch (...) {}
    c.auth_token = get_setting(m_db, "lan_auth_token");
    return c;
}

void ConfigStore::set_lan(const LanConfig& c) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "lan_host", c.host);
    put_setting(m_db, "lan_port", std::to_string(c.port));
    put_setting(m_db, "lan_auth_token", c.auth_token);
}

RoomSettings ConfigStore::room() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    RoomSettings r;
    r.room_id = get_setting(m_db, "room_id", "main-auditorium");
    const std::string d = get_setting(m_db, "default_delay_s", "180");
    try { r.default_delay_s = std::stoi(d); } catch (...) {}
    return r;
}

void ConfigStore::set_room(const RoomSettings& r) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "room_id", r.room_id);
    put_setting(m_db, "default_delay_s", std::to_string(r.default_delay_s));
}

// ── destinations ─────────────────────────────────────────────────────────────

namespace {
Destination read_row(sqlite3_stmt* st) {
    Destination d;
    d.id              = sqlite3_column_int64(st, 0);
    d.name            = text_col(st, 1);
    d.room_id         = text_col(st, 2);
    d.url             = text_col(st, 3);
    d.stream_key      = text_col(st, 4);
    d.audio.label     = text_col(st, 5);
    d.allow_transcode = sqlite3_column_int(st, 6) != 0;
    d.enabled         = sqlite3_column_int(st, 7) != 0;
    d.delay_s         = sqlite3_column_int(st, 8);
    d.srt_mode        = text_col(st, 9) == "listener" ? SrtMode::Listener
                                                      : SrtMode::Caller;
    d.srt_passphrase  = text_col(st, 10);
    d.srt_latency_ms  = sqlite3_column_int(st, 11);
    return d;
}
const char* kSelect =
    "SELECT id,name,room_id,url,stream_key,audio_label,allow_transcode,"
    "enabled,delay_s,srt_mode,srt_passphrase,srt_latency_ms FROM destinations";

const char* mode_text(SrtMode m) {
    return m == SrtMode::Listener ? "listener" : "caller";
}
} // namespace

std::vector<Destination> ConfigStore::destinations() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<Destination> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, (std::string(kSelect) + " ORDER BY id;").c_str(),
                           -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(read_row(st));
    sqlite3_finalize(st);
    return out;
}

std::optional<Destination> ConfigStore::destination(int64_t id) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, (std::string(kSelect) + " WHERE id=?;").c_str(),
                           -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<Destination> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = read_row(st);
    sqlite3_finalize(st);
    return out;
}

int64_t ConfigStore::add(const Destination& in, std::string& error) {
    // Tidied before it is judged, so what validate() reads and what is stored
    // are the same thing. Doing it here rather than in the API layer means a
    // destination cannot reach the database untidied by some other route.
    Destination d = in;
    normalize(d);
    error = validate(d);
    if (!error.empty()) return 0;

    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT INTO destinations(name,room_id,url,stream_key,audio_label,"
            "allow_transcode,enabled,delay_s,srt_mode,srt_passphrase,"
            "srt_latency_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK) {
        error = "could not save this destination";
        return 0;
    }
    bind_text(st, 1, d.name);
    bind_text(st, 2, d.room_id);
    bind_text(st, 3, d.url);
    bind_text(st, 4, d.stream_key);
    bind_text(st, 5, d.audio.label);
    sqlite3_bind_int(st, 6, d.allow_transcode ? 1 : 0);
    sqlite3_bind_int(st, 7, d.enabled ? 1 : 0);
    sqlite3_bind_int(st, 8, d.delay_s);
    bind_text(st, 9, mode_text(d.srt_mode));
    bind_text(st, 10, d.srt_passphrase);
    sqlite3_bind_int(st, 11, d.srt_latency_ms);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) { error = "could not save this destination"; return 0; }
    return sqlite3_last_insert_rowid(m_db);
}

bool ConfigStore::update(const Destination& in, std::string& error) {
    Destination d = in;
    normalize(d);
    error = validate(d);
    if (!error.empty()) return false;

    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "UPDATE destinations SET name=?,room_id=?,url=?,stream_key=?,"
            "audio_label=?,allow_transcode=?,enabled=?,delay_s=?,srt_mode=?,"
            "srt_passphrase=?,srt_latency_ms=? WHERE id=?;",
            -1, &st, nullptr) != SQLITE_OK) {
        error = "could not save this destination";
        return false;
    }
    bind_text(st, 1, d.name);
    bind_text(st, 2, d.room_id);
    bind_text(st, 3, d.url);
    bind_text(st, 4, d.stream_key);
    bind_text(st, 5, d.audio.label);
    sqlite3_bind_int(st, 6, d.allow_transcode ? 1 : 0);
    sqlite3_bind_int(st, 7, d.enabled ? 1 : 0);
    sqlite3_bind_int(st, 8, d.delay_s);
    bind_text(st, 9, mode_text(d.srt_mode));
    bind_text(st, 10, d.srt_passphrase);
    sqlite3_bind_int(st, 11, d.srt_latency_ms);
    sqlite3_bind_int64(st, 12, d.id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) error = "could not save this destination";
    return ok;
}

bool ConfigStore::remove(int64_t id) {
    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM destinations WHERE id=?;", -1,
                           &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

void ConfigStore::set_enabled(int64_t id, bool on) {
    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "UPDATE destinations SET enabled=? WHERE id=?;",
                           -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, on ? 1 : 0);
    sqlite3_bind_int64(st, 2, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

} // namespace multisite_relay
