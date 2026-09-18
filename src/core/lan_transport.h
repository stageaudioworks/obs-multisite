// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// lan_transport.h — a satellite's client for a LanObjectServer on the same
// network or an existing VPN. See PROJECT-SCOPE.md §8.7.
//
// Read-only, on purpose: a decoder never writes, and LanObjectServer never
// accepts a write. Object keys are the identical cloud shape DecoderSession
// already asks for ("rooms/{room}/live.json", "events/{id}/manifest.json",
// "events/{id}/segments/{seq}.m4s", …), translated into the LanObjectServer's
// own path shape — DecoderSession itself never learns LAN delivery exists,
// exactly as Session never learns on the encoder side.
//
#include "transport.h"

#include <atomic>
#include <string>

namespace multisite {

struct LanTransportConfig {
    std::string host;              // e.g. "192.168.1.50" or a .local name
    int         port = 9080;
    std::string auth_token;        // sent as "Authorization: Bearer <token>"
    // Short on purpose: this is meant to be the SAME network. A slow or dead
    // LAN path should fail fast so FallbackTransport can fall back to cloud
    // within a poll interval, not stall the decoder for a cloud-sized timeout.
    int connect_timeout_ms = 800;
    int request_timeout_ms = 2000;
};

class LanTransport : public Transport {
public:
    explicit LanTransport(LanTransportConfig cfg);

    // Translates the cloud key shape onto the LanObjectServer's own routes:
    //   rooms/{room}/live.json        -> GET /rooms/{room}/live.json
    //   events/{id}/manifest.json     -> GET /events/{id}/manifest.json
    //   events/{id}/event.json        -> GET /events/{id}/event.json
    //   events/{id}/init.mp4          -> GET /events/{id}/init.mp4
    //   events/{id}/segments/{n}.m4s  -> GET /events/{id}/segments/{n}.m4s
    //   events/{id}/markers.json      -> GET /events/{id}/markers.json
    // An event-listing prefix is the one thing genuinely not served: event
    // browsing (§7.5) is inherently a cloud-only operation, and falling back
    // to cloud for it is correct rather than a gap.
    GetResult get(const std::string& key) override;

    // Not implemented: a decoder never writes, lists, or deletes, and a
    // LanObjectServer never accepts any of the three. The base class's
    // default ("not implemented by this transport") is exactly right.
    PutResult put(const std::string&, const std::vector<uint8_t>&,
                 const std::string&, const std::map<std::string, std::string>&) override {
        PutResult r;
        r.error = "LanTransport is read-only";
        r.retryable = false;
        return r;
    }

    // The ONE thing a LAN satellite sends rather than reads: a cue, handed to
    // the encoder's hub (POST /api/cue) so a box with no bucket can still set
    // one and stay read-only. On success `merged_json` is the hub's whole
    // merged cue list, so the caller can show the new cue at once.
    bool publish_cue(const std::string& author, const std::string& label,
                     std::string& merged_json, std::string& error);

    // Abandon whatever request is in flight, same contract as S3Transport's
    // (see transport.h) — needed for the identical reason: tearing down a
    // decoder source while a LAN request is mid-flight must not block OBS's
    // UI thread for up to request_timeout_ms.
    void cancel_pending() override;

    // Clears the cancel flag so this transport can be used again — the same
    // soft-stop/resume pattern SourceCtx already applies to its S3Transport
    // (Stop cancels in-flight requests without tearing the source down; Play
    // re-arms them). Call before issuing new requests, never while one is in
    // flight.
    void resume_pending() override { m_cancel = false; }

    // Whether the last get() actually reached the LAN endpoint (any HTTP
    // response at all, including a 404) rather than failing to connect —
    // i.e. "is the LAN path itself alive", independent of whether this
    // particular key happened to be found. FallbackTransport uses exactly
    // this distinction to decide the decoder dock's "via LAN" / "via cloud"
    // indicator (§8.7): a 404 for one key (markers.json before the first
    // marker, a segment that aged out of the retention window) must not
    // read as "LAN is down" the way a connection failure genuinely should.
    bool last_request_reached_server() const override { return m_last_reached.load(); }
    bool last_request_cancelled() const override { return m_cancel.load(); }

private:
    LanTransportConfig m_cfg;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_last_reached{false};

    std::string url_for(const std::string& key) const;
};

} // namespace multisite
