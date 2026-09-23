// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// cloud_storage.h — the Transport a PAIRED device reads and writes through.
//
// WHY. A paired device must read the bucket its credential broker handed it,
// with the short-lived credentials that broker minted — not with a key an
// operator pasted. This decorator is where that becomes true: it holds a
// reference to the device's CloudIdentity, builds the S3 request from the
// credential set that identity currently holds, and delegates to a real
// S3Transport. When the identity's credentials are unreachable it degrades to
// the last good set; it never falls back to a configured key, because there is
// none to fall back to (docs/scope/spec-cloud-identity.md).
//
// It is a SEAM, not an implementation: signature construction, retries and the
// network all stay in S3Transport. What this adds is (a) the bucket/endpoint/
// credentials come from the identity, (b) the session token is carried as
// X-Amz-Security-Token, and (c) an encoder writes while a decoder only reads.
//
// The session token needs no signer change: SigV4 signs whatever headers the
// caller supplies and lists them in SignedHeaders, so passing
// X-Amz-Security-Token through the existing `extra_headers` seam produces a
// correct signed request for temporary credentials.

#include "transport.h"
#include "cloud_identity.h"
#include "s3_transport.h"

#include <memory>
#include <mutex>
#include <string>

namespace multisite {

// Which way this device is allowed to use its credentials. The collector says
// so in the credentials reply, and the role is honoured here: a decoder's
// transport refuses a put rather than trusting that nothing will call it.
enum class CloudRole { Encoder, Decoder };

// What the transport needs beyond the identity: the region and scheme the
// broker's endpoint implies, and the timeouts S3Transport wants. Kept separate
// from S3Config because the bucket and credentials come from the identity and
// are deliberately not settable here — that is the whole point of the class.
struct CloudStorageConfig {
    std::string region = "auto";
    bool        use_https = true;
    int         connect_timeout_ms = 5000;
    int         request_timeout_ms = 30000;
};

class CloudTransport : public Transport {
public:
    // `identity` must outlive this transport. The role is fixed at construction
    // because it is the device's, not the request's.
    CloudTransport(CloudIdentity& identity, CloudRole role,
                   const CloudStorageConfig& cfg);
    ~CloudTransport() override;

    PutResult put(const std::string& key,
                  const std::vector<uint8_t>& body,
                  const std::string& content_type,
                  const std::map<std::string, std::string>& tags) override;
    int64_t object_size(const std::string& key) override;
    GetResult get(const std::string& key) override;
    ListResult list(const std::string& prefix, const std::string& delimiter = "",
                    const std::string& continuation_token = "",
                    int max_keys = 1000) override;
    DeleteResult remove(const std::string& key) override;

    void cancel_pending() override;
    void resume_pending() override;
    bool last_request_reached_server() const override;
    bool last_request_cancelled() const override;
    int64_t server_clock_skew_ms() const override;

    // What the transport is doing, for the dock's status line: which bucket it
    // is using, and whether the credentials behind it are fresh or last-good.
    // "Brokered must not come to mean opaque" (PROJECT-SCOPE §8.5).
    std::string bucket() const;
    bool credentials_are_stale() const;
    bool has_credentials() const;

    // The idle monitor's link probe, delegated to whichever inner transport the
    // identity's CURRENT credentials build. It exists here rather than being
    // rebuilt in the controller because the rebuild rule — a new inner whenever
    // the credential set changes — is inner_for's and must not have a second
    // implementation to drift from. Returns an unreachable probe, not an
    // exception, when the identity holds nothing.
    StorageProbe probe(const std::string& key);
    std::string host() const;

    // Observations from ordinary traffic — which PoP and server answered, and
    // the download rate — for a status page. Taken from the CURRENT inner, as
    // server_clock_skew_ms() is; a refresh rebuilds it about every 7.5 minutes
    // at a 900 s TTL. Empty/0
    // before any inner exists. The previous inner stands in until the new one
    // has seen traffic (see m_prev_inner).
    std::string last_colo() const;
    std::string last_server() const;
    double      observed_download_bytes_per_s() const;
    uint64_t    download_samples() const;

private:
    // The inner transport for the identity's CURRENT credentials, rebuilt when
    // the credential set changes (a refresh, or a fall to last-good). Returns
    // nullptr and sets `why` when the identity holds nothing usable.
    std::shared_ptr<S3Transport> inner_for(std::string& why) const;

    PutResult put_refused() const;

    CloudIdentity&      m_identity;
    CloudRole           m_role;
    CloudStorageConfig  m_cfg;
    mutable std::mutex  m_mtx;
    // The inner transport, rebuilt when the credentials it was built from
    // change. Cached so a per-frame or per-segment call is not re-signing a
    // fresh S3Transport on every request.
    mutable std::shared_ptr<S3Transport> m_inner;
    // The inner a refresh replaced, kept for two reasons. A request already in
    // flight is still running on it, so cancel_pending() must reach it too —
    // otherwise stopping a source in the moment after a refresh waited out
    // that request's whole timeout. And its observations stand in until the
    // new inner has seen traffic of its own, so a status page does not read
    // "unreachable" for the seconds after every refresh.
    mutable std::shared_ptr<S3Transport> m_prev_inner;
    // Caller holds m_mtx: the inner whose observations to report.
    const S3Transport* observed_locked() const;
    // What m_inner was built from, so a refresh is detected by comparing rather
    // than by trusting a flag the identity would have to remember to set.
    mutable std::string m_built_expiry;
    mutable std::string m_built_session;
    mutable bool        m_built_stale = false;
};

} // namespace multisite
