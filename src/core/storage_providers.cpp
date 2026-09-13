// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_providers.h"

#include <algorithm>
#include <cctype>

namespace multisite {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

std::string provider_key(StorageProvider p) {
    switch (p) {
        case StorageProvider::CloudflareR2:   return "r2";
        case StorageProvider::AwsS3:          return "aws";
        case StorageProvider::BackblazeB2:    return "backblaze";
        case StorageProvider::Wasabi:         return "wasabi";
        case StorageProvider::MultisiteCloud: return "multisite_cloud";
        default:                              return "custom";
    }
}

StorageProvider provider_from_key(const std::string& key) {
    if (key == "r2")              return StorageProvider::CloudflareR2;
    if (key == "aws")             return StorageProvider::AwsS3;
    if (key == "backblaze")       return StorageProvider::BackblazeB2;
    if (key == "wasabi")          return StorageProvider::Wasabi;
    if (key == "multisite_cloud") return StorageProvider::MultisiteCloud;
    return StorageProvider::Custom;
}

const std::vector<ProviderInfo>& all_providers() {
    static const std::vector<ProviderInfo> table = {
        { StorageProvider::CloudflareR2, "r2", "Cloudflare R2",
          /*account_id*/ true,  /*region*/ false, /*endpoint*/ false, /*available*/ true },
        { StorageProvider::AwsS3, "aws", "AWS S3",
          false, true, false, true },
        { StorageProvider::BackblazeB2, "backblaze", "Backblaze B2",
          false, true, false, true },
        { StorageProvider::Wasabi, "wasabi", "Wasabi",
          false, true, false, true },
        { StorageProvider::Custom, "custom", "Custom / other S3-compatible",
          false, true, true, true },
        { StorageProvider::MultisiteCloud, "multisite_cloud", "Multisite Cloud (coming soon)",
          false, false, false, /*available*/ false },
    };
    return table;
}

const ProviderInfo& provider_info(StorageProvider p) {
    for (const auto& info : all_providers())
        if (info.id == p) return info;
    return all_providers().back(); // unreachable given the enum, but never null
}

DerivedFields derive(StorageProvider provider, const std::string& input) {
    DerivedFields d;
    switch (provider) {
        case StorageProvider::CloudflareR2:
            d.r2_account_id = input;
            d.region = "auto";
            break;
        case StorageProvider::AwsS3:
            d.endpoint_host = "s3." + input + ".amazonaws.com";
            d.region = input;
            break;
        case StorageProvider::BackblazeB2:
            d.endpoint_host = "s3." + input + ".backblazeb2.com";
            d.region = input;
            break;
        case StorageProvider::Wasabi:
            d.endpoint_host = "s3." + input + ".wasabisys.com";
            d.region = input;
            break;
        case StorageProvider::Custom:
        case StorageProvider::MultisiteCloud:
        default:
            // Nothing to derive: Custom's fields already are what goes into
            // S3Config, and MultisiteCloud isn't real yet.
            break;
    }
    return d;
}

StorageProvider detect_provider(const std::string& endpoint_host,
                                const std::string& r2_account_id) {
    const std::string host = to_lower(endpoint_host);
    if (host.empty() && !r2_account_id.empty())
        return StorageProvider::CloudflareR2;
    if (ends_with(host, ".r2.cloudflarestorage.com"))
        return StorageProvider::CloudflareR2;
    if (ends_with(host, ".amazonaws.com"))
        return StorageProvider::AwsS3;
    if (ends_with(host, ".backblazeb2.com"))
        return StorageProvider::BackblazeB2;
    if (ends_with(host, ".wasabisys.com"))
        return StorageProvider::Wasabi;
    return StorageProvider::Custom;
}

} // namespace multisite
