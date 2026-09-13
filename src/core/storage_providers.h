// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// storage_providers.h — which S3-compatible provider an operator picked, and
// what that means for S3Config.
//
// S3Config itself never changes: bucket, key, secret, and either an R2
// account id or a raw endpoint host plus region. This is a layer in front of
// it — given a provider and the one field THAT provider actually needs,
// derive the rest, so an operator picks "Cloudflare R2" and types an account
// id rather than learning that R2's hostname is
// "<account>.r2.cloudflarestorage.com". See PROJECT-SCOPE.md §8.6.
//
// Deliberately a fixed table rather than data read from disk: the five real
// providers below are a technical fact about how each service names its own
// endpoint, not something an operator or an integrator would ever want to
// edit — unlike the locale strings and web pages this project does load
// from data/ at runtime, for exactly that reason.
//
#include <string>
#include <vector>

namespace multisite {

enum class StorageProvider {
    CloudflareR2,
    AwsS3,
    BackblazeB2,
    Wasabi,
    Custom,           // today's full form: a raw endpoint, unchanged
    MultisiteCloud,   // not yet real — see PROJECT-SCOPE.md §8.5. Listed so a
                      // dock can grey it out now rather than add it in later.
};

// Persisted as a plain string, the same way every other setting in this
// project is — not the enum's own numeric value, which would silently break
// the day the enum is reordered. provider_from_key() falls back to Custom
// for anything unrecognised (an older save, a typo, a future key an older
// build doesn't know) rather than refusing to load.
std::string provider_key(StorageProvider p);
StorageProvider provider_from_key(const std::string& key);

struct ProviderInfo {
    StorageProvider id = StorageProvider::Custom;
    std::string     key;           // provider_key(id)
    std::string     display_name;  // "Cloudflare R2"
    // Which single field this provider needs beyond bucket/key/secret, which
    // every provider needs regardless of which of these is set.
    bool needs_account_id = false; // R2 only
    bool needs_region     = false; // AWS / Backblaze / Wasabi
    bool needs_endpoint   = false; // Custom only — the raw hostname field
    // False only for MultisiteCloud today: listed for the dropdown, not yet
    // selectable.
    bool available = true;
};

// In display order — Custom second-to-last, MultisiteCloud last and marked
// unavailable until it exists.
const std::vector<ProviderInfo>& all_providers();
const ProviderInfo& provider_info(StorageProvider p);

// What a provider (other than Custom) derives into S3Config's shape, given
// the one field the operator actually typed — an account id for R2, a
// region for the other three. Never called for Custom: its fields already
// ARE what goes into S3Config, unmodified.
struct DerivedFields {
    std::string endpoint_host;  // empty for R2
    std::string r2_account_id; // empty except for R2
    std::string region;         // "auto" for R2, the typed region otherwise
};
DerivedFields derive(StorageProvider provider, const std::string& input);

// The reverse, for loading a saved configuration: given what's actually on
// disk (endpoint_host, r2_account_id), guess which provider it matches, so
// the dock can preselect the right dropdown entry. A saved endpoint that
// doesn't match any known template — MinIO, a provider not in this list, a
// hand-edited config — reads back as Custom, never misrepresented as
// something it isn't.
StorageProvider detect_provider(const std::string& endpoint_host,
                                const std::string& r2_account_id);

} // namespace multisite
