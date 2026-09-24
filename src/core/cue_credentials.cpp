// SPDX-License-Identifier: GPL-3.0-or-later
#include "cue_credentials.h"

#include "../vendor/nlohmann/json.hpp"

using json = nlohmann::json;

namespace multisite {
namespace {

// A failed fetch with nothing held is retried on this interval. Slower than the
// main credential's 5 s floor: a cue is not the programme, and a collector that
// is down is asked by every paired player at once.
constexpr long long kCueRetryMs = 15000;

// How many events' credentials are kept. The wanted one is refreshed; the rest
// are only kept so switching back to a recent event does not wait on a fetch.
constexpr size_t kMaxEntries = 4;

// After a failed refresh with a live set still held, try again within this
// long rather than at half the remaining lifetime — which, on a twelve-hour
// credential, could be hours after the collector came back.
constexpr long long kCueRetryCapMs = 5 * 60 * 1000;

} // namespace

CueCredentialsReply cloud_parse_cue_credentials(const std::string& body,
                                                long http_code,
                                                const std::string& event_id) {
    CueCredentialsReply out;
    out.http_code = http_code;
    if (http_code == 403) { out.outcome = CueCredentialsReply::Outcome::Unpaired; return out; }
    if (http_code == 409) { out.outcome = CueCredentialsReply::Outcome::NotADecoder; return out; }
    if (http_code == 400) { out.outcome = CueCredentialsReply::Outcome::BadEvent; return out; }
    if (http_code != 200) return out;   // Failed

    // The credential fields are the main credential's, parsed by the one
    // parser, so the two cannot disagree about a field.
    const CredentialsReply base = cloud_parse_credentials(body, 200);
    if (!base.ok) return out;

    std::string key;
    try {
        const json j = json::parse(body);
        if (j.is_object() && j.contains("object_key") && j["object_key"].is_string())
            key = j["object_key"].get<std::string>();
    } catch (...) {
        return out;
    }
    // Only a key inside THIS event's cues/, ending .json, and no deeper: a
    // reply is not allowed to point this device's writes anywhere else.
    const std::string prefix = "events/" + event_id + "/cues/";
    const std::string suffix = ".json";
    const bool shaped =
        !event_id.empty() &&
        key.size() > prefix.size() + suffix.size() &&
        key.compare(0, prefix.size(), prefix) == 0 &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0 &&
        key.find('/', prefix.size()) == std::string::npos;
    if (!shaped) return out;

    out.outcome = CueCredentialsReply::Outcome::Ok;
    out.creds = base.creds;
    out.object_key = key;
    return out;
}

std::string cue_credentials_request(const std::string& event_id) {
    json j;
    j["event_id"] = event_id;
    return j.dump();
}

void CueCredentials::want_event(const std::string& event_id) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (event_id == m_wanted) return;
    m_wanted = event_id;
    if (event_id.empty()) return;
    m_entries.emplace(event_id, Entry{});   // no-op if already held
    // Past the cap, drop events that are not the wanted one. Map order is by
    // event id, which for ULIDs is by time, so the oldest go first.
    for (auto it = m_entries.begin();
         m_entries.size() > kMaxEntries && it != m_entries.end();) {
        if (it->first == m_wanted) { ++it; continue; }
        it = m_entries.erase(it);
    }
}

std::string CueCredentials::wanted_event() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_wanted;
}

CueCredentials::Due CueCredentials::tick(long long now_ms) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    Due d;
    // A 403 stopped this device deliberately; retrying is how a device becomes
    // a stuck one (same rule as CloudIdentity::tick).
    if (m_unpaired || m_wanted.empty()) return d;
    const auto it = m_entries.find(m_wanted);
    if (it == m_entries.end()) return d;
    const Entry& e = it->second;
    if (e.stopped) return d;
    if (e.next_fetch_at_ms == 0 || now_ms >= e.next_fetch_at_ms) {
        d.fetch = true;
        d.event_id = m_wanted;
    }
    return d;
}

void CueCredentials::on_reply(const std::string& event_id,
                              const CueCredentialsReply& r, long long now_ms) {
    std::lock_guard<std::mutex> lk(m_mtx);
    using O = CueCredentialsReply::Outcome;
    if (r.outcome == O::Unpaired) {
        // Stop everything, and keep what is held: a running event's cues are
        // covered by it until it expires, as the main credential's are.
        m_unpaired = true;
        for (auto& kv : m_entries) {
            if (kv.second.creds.present()) kv.second.creds.from_last_good = true;
            kv.second.why = "the collector says this player is no longer paired";
        }
        return;
    }
    // A reply for an event dropped from the cache meanwhile is not re-added.
    const auto it = m_entries.find(event_id);
    if (it == m_entries.end()) return;
    Entry& e = it->second;

    switch (r.outcome) {
    case O::Ok:
        e.creds = r.creds;
        e.creds.from_last_good = false;
        e.object_key = r.object_key;
        e.why.clear();
        e.next_fetch_at_ms = now_ms + cloud_next_refresh_ms(e.creds, now_ms);
        if (e.next_fetch_at_ms <= now_ms) e.next_fetch_at_ms = now_ms + kCueRetryMs;
        return;
    case O::NotADecoder:
        // Not a retry: an encoder writes its cues with the credential it has.
        e.stopped = true;
        e.why = "the collector treats this device as an encoder, which writes "
                "cues with its own credential";
        return;
    case O::BadEvent:
        // Not a retry either: the id is this side's, and asking again with the
        // same one gets the same answer.
        e.stopped = true;
        e.why = "the collector rejected this event's id";
        return;
    case O::Unreachable:
    case O::Failed:
    case O::Unpaired:   // handled above
        break;
    }
    // Keep the last good set, mark it, and try again: within kCueRetryCapMs
    // while a set is still held, every kCueRetryMs when there is none.
    e.why = r.outcome == O::Unreachable
        ? "Multisite Cloud could not be reached for a cue permission"
        : "Multisite Cloud did not grant a cue permission (HTTP " +
              std::to_string(r.http_code) + ")";
    long long wait = 0;
    if (e.creds.present()) {
        e.creds.from_last_good = true;
        wait = cloud_next_refresh_ms(e.creds, now_ms);
    }
    if (wait <= 0) wait = kCueRetryMs;
    if (wait > kCueRetryCapMs) wait = kCueRetryCapMs;
    if (wait < kCueRetryMs) wait = kCueRetryMs;
    e.next_fetch_at_ms = now_ms + wait;
}

CueCredentials::Held CueCredentials::held(const std::string& event_id,
                                          long long now_ms) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    Held h;
    const auto it = m_entries.find(event_id);
    if (it == m_entries.end()) {
        h.why = "no cue permission has been fetched for this event yet";
        return h;
    }
    const Entry& e = it->second;
    // Last-good is usable — that is the point of fetching ahead — until it
    // actually expires.
    if (e.creds.present() && !e.object_key.empty() &&
        now_ms < e.creds.expires_at_ms) {
        h.present = true;
        h.creds = e.creds;
        h.object_key = e.object_key;
        return h;
    }
    if (e.creds.present()) h.why = "the cue permission for this event has expired";
    else if (!e.why.empty()) h.why = e.why;
    else h.why = "no cue permission has been fetched for this event yet";
    return h;
}

void CueCredentials::reset() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_entries.clear();
    m_unpaired = false;
    // The wanted event stays: it is a fact about playback, not about the
    // pairing, and a new pairing should fetch for it at once.
    if (!m_wanted.empty()) m_entries.emplace(m_wanted, Entry{});
}

} // namespace multisite
