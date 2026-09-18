// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_probe.h"

#include "../core/s3_transport.h"
#include "../core/storage_providers.h"

#include <vector>

namespace multisite_obs {

namespace {

// What to show an operator when it fails. The provider's own words when there
// are any — "SignatureDoesNotMatch" says more than "403" — and plain language
// when there are not.
std::string explain(long status, const std::string& err) {
    if (!err.empty()) return err;
    if (status == 403 || status == 401)
        return "the bucket refused this key";
    if (status == 404)
        return "the bucket was not found";
    if (status != 0) return "HTTP " + std::to_string(status);
    return "no answer — check the address and the firewall";
}

} // namespace

void fill_s3_config(multisite::S3Config& out, const std::string& provider_key,
                    const std::string& account_id, const std::string& endpoint,
                    const std::string& region_field, const std::string& bucket,
                    const std::string& key_id, const std::string& secret) {
    out = multisite::S3Config{};
    auto provider = multisite::provider_from_key(provider_key);
    if (provider == multisite::StorageProvider::Custom) {
        out.endpoint_host = endpoint;
        out.region        = region_field;
    } else {
        const std::string input =
            provider == multisite::StorageProvider::CloudflareR2 ? account_id
                                                                 : region_field;
        auto derived = multisite::derive(provider, input);
        out.r2_account_id = derived.r2_account_id;
        out.endpoint_host = derived.endpoint_host;
        out.region        = derived.region;
    }
    out.bucket            = bucket;
    out.access_key_id     = key_id;
    out.secret_access_key = secret;
}

ProbeResult probe_bucket(const multisite::S3Config& cfg, bool write,
                         const std::string& room_id) {
    ProbeResult r;
    multisite::S3Transport tx(cfg);
    r.url = tx.base_url();

    if (write) {
        // Write, then remove. A key that can read a bucket but not write to it
        // looks perfectly healthy until the first segment — which is the exact
        // failure this test exists to find, and the reason it writes.
        std::vector<uint8_t> probe(256, 0x5A);
        const std::string key = "multisite-connection-test.bin";
        auto p = tx.put(key, probe, "application/octet-stream", {});
        r.answered    = p.success || tx.last_request_reached_server();
        r.http_status = p.http_status;
        if (!p.success) {
            r.detail = explain(p.http_status, p.error);
            return r;
        }
        // Best effort: a failure to tidy up is not the operator's problem, and
        // whatever is left is one 256-byte object with an obvious name.
        tx.remove(key);
        r.wrote = true;
        r.ok    = true;
        return r;
    }

    // Read: the request a campus actually makes. live.json rather than a
    // listing, because a decoder does not need ListBucket — object-scoped keys
    // are the recommended kind and often lack it — so asking for a list would
    // report a perfectly good key as broken.
    auto g = tx.get("rooms/" + room_id + "/live.json");
    r.answered    = g.success || tx.last_request_reached_server();
    r.http_status = g.http_status;
    if (g.success) {
        r.ok = true;
        r.found_live = true;
    } else if (g.http_status == 404) {
        // The bucket, the key and the address all work; there is simply nothing
        // being broadcast right now. That is a pass, not a failure.
        r.ok = true;
    } else {
        r.detail = explain(g.http_status, g.error);
    }
    return r;
}

} // namespace multisite_obs
