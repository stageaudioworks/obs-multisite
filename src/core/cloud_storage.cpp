// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_storage.h"

namespace multisite {

CloudTransport::CloudTransport(CloudIdentity& identity, CloudRole role,
                               const CloudStorageConfig& cfg)
    : m_identity(identity), m_role(role), m_cfg(cfg) {}

CloudTransport::~CloudTransport() = default;

std::string CloudTransport::bucket() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_identity.credentials().bucket;
}

bool CloudTransport::credentials_are_stale() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_identity.credentials().from_last_good;
}

bool CloudTransport::has_credentials() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_identity.credentials().present();
}

StorageProbe CloudTransport::probe(const std::string& key) {
    std::string why;
    std::shared_ptr<S3Transport> tx;
    {
        // inner_for touches the identity and the inner pointer, so it is taken
        // under the same lock the other accessors use. The probe itself is NOT
        // run under the lock — it is a network round trip, and holding the
        // mutex across it would block every writer for its whole duration.
        std::lock_guard<std::mutex> lk(m_mtx);
        tx = inner_for(why);
    }
    if (!tx) {
        StorageProbe p;
        p.reachable = false;
        p.error = why.empty() ? "no cloud credentials" : why;
        return p;
    }
    return tx->probe(key);
}

std::string CloudTransport::host() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    // The HOST IS THE COLLECTOR'S, known from the moment credentials land, and
    // reported even before an inner exists so the dock can name the endpoint it
    // is about to try. The typed-field equivalent returns the same thing off
    // its config without connecting.
    return m_identity.credentials().endpoint;
}

std::shared_ptr<S3Transport> CloudTransport::inner_for(std::string& why) const {
    // Caller holds m_mtx. Reads the identity ONCE and builds from exactly what
    // it read (standards §2): re-reading between the check and the build is how
    // a bucket and a token from two different refreshes end up in one request.
    const Credentials c = m_identity.credentials();
    if (!c.present()) {
        why = m_identity.unpaired()
            ? "this device was unpaired by the collector"
            : "no cloud credentials yet";
        return nullptr;
    }

    // Rebuild only when the set we built from has changed. Compare the token
    // AND the expiry AND the staleness flag: a refresh that hands back the same
    // token with a later expiry is still a different set to sign with, and a
    // fall to last-good changes only the flag.
    const std::string key = std::to_string(c.expires_at_ms);
    if (m_inner && m_built_expiry == key && m_built_stale == c.from_last_good &&
        m_built_session == c.session_token)
        return m_inner;

    S3Config s3 = s3_config_from_credentials(c);
    // The transport's own talking preferences, which are the host's and not the
    // collector's. The identity-derived fields — bucket, endpoint, the key pair,
    // the token — came from the one shared converter.
    s3.region = m_cfg.region;
    s3.use_https = m_cfg.use_https;
    s3.connect_timeout_ms = m_cfg.connect_timeout_ms;
    s3.request_timeout_ms = m_cfg.request_timeout_ms;

    m_prev_inner = m_inner;   // may still be carrying a request — see the header
    m_inner = std::make_shared<S3Transport>(s3);
    m_built_expiry = key;
    m_built_stale = c.from_last_good;
    m_built_session = c.session_token;
    why.clear();
    return m_inner;
}

PutResult CloudTransport::put_refused() const {
    PutResult r;
    r.success = false;
    r.http_status = 0;
    // Permanent: retrying will not change the device's role. Stated as the
    // reason rather than as a status, so a log line is readable.
    r.retryable = false;
    r.error = "this device reads only — the collector granted a decoder role";
    return r;
}

PutResult CloudTransport::put(const std::string& key,
                              const std::vector<uint8_t>& body,
                              const std::string& content_type,
                              const std::map<std::string, std::string>& tags) {
    // The role is honoured HERE, not trusted to the caller: a decoder that
    // somehow reaches put() is refused rather than writing with read-only
    // credentials, which would fail at the bucket with a worse message.
    if (m_role != CloudRole::Encoder) return put_refused();

    std::shared_ptr<S3Transport> tx;
    std::string why;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        tx = inner_for(why);
    }
    if (!tx) {
        PutResult r;
        r.retryable = true;   // a credential refresh may fix it
        r.error = why;
        return r;
    }
    return tx->put(key, body, content_type, tags);
}

int64_t CloudTransport::object_size(const std::string& key) {
    std::shared_ptr<S3Transport> tx;
    std::string why;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        tx = inner_for(why);
    }
    if (!tx) return -1;
    return tx->object_size(key);
}

GetResult CloudTransport::get(const std::string& key) {
    std::shared_ptr<S3Transport> tx;
    std::string why;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        tx = inner_for(why);
    }
    if (!tx) {
        GetResult r;
        r.retryable = true;
        r.error = why;
        return r;
    }
    return tx->get(key);
}

ListResult CloudTransport::list(const std::string& prefix,
                                const std::string& delimiter,
                                const std::string& continuation_token,
                                int max_keys) {
    std::shared_ptr<S3Transport> tx;
    std::string why;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        tx = inner_for(why);
    }
    if (!tx) {
        ListResult r;
        r.retryable = true;
        r.error = why;
        return r;
    }
    return tx->list(prefix, delimiter, continuation_token, max_keys);
}

DeleteResult CloudTransport::remove(const std::string& key) {
    // Deletion is a WRITE. A decoder has no business removing an archive.
    if (m_role != CloudRole::Encoder) {
        DeleteResult r;
        r.retryable = false;
        r.error = "this device reads only — the collector granted a decoder role";
        return r;
    }
    std::shared_ptr<S3Transport> tx;
    std::string why;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        tx = inner_for(why);
    }
    if (!tx) {
        DeleteResult r;
        r.retryable = true;
        r.error = why;
        return r;
    }
    return tx->remove(key);
}

void CloudTransport::cancel_pending() {
    // Cancel the CURRENT inner and the one a refresh replaced: a request that
    // began before the refresh is still running on the old one, and it is as
    // much "the request in flight" as anything on the new. A transport rebuilt
    // after this call has its own flag and starts uncancelled.
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_inner) m_inner->cancel_pending();
    if (m_prev_inner) m_prev_inner->cancel_pending();
}

void CloudTransport::resume_pending() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_inner) m_inner->resume_pending();
    if (m_prev_inner) m_prev_inner->resume_pending();
}

const S3Transport* CloudTransport::observed_locked() const {
    if (m_inner && (m_inner->download_samples() > 0 ||
                    !m_inner->last_colo().empty() ||
                    !m_inner->last_server().empty()))
        return m_inner.get();
    if (m_prev_inner) return m_prev_inner.get();
    return m_inner.get();
}

bool CloudTransport::last_request_reached_server() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_inner ? m_inner->last_request_reached_server() : false;
}

bool CloudTransport::last_request_cancelled() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_inner ? m_inner->last_request_cancelled() : false;
}

std::string CloudTransport::last_colo() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const S3Transport* t = observed_locked();
    return t ? t->last_colo() : std::string();
}

std::string CloudTransport::last_server() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const S3Transport* t = observed_locked();
    return t ? t->last_server() : std::string();
}

double CloudTransport::observed_download_bytes_per_s() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const S3Transport* t = observed_locked();
    return t ? t->observed_download_bytes_per_s() : 0.0;
}

uint64_t CloudTransport::download_samples() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    const S3Transport* t = observed_locked();
    return t ? t->download_samples() : 0;
}

int64_t CloudTransport::server_clock_skew_ms() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_inner ? m_inner->server_clock_skew_ms() : 0;
}

} // namespace multisite
