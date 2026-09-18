// SPDX-License-Identifier: GPL-3.0-or-later
#include "mirror_verify.h"

#include "model.h"

#include <map>

namespace multisite {

namespace {

bool read_manifest(Transport& tx, const std::string& event_id,
                   std::map<uint64_t, std::string>& out, std::string& error) {
    GetResult r = tx.get(event_prefix_for(event_id) + "manifest.json");
    if (!r.success) {
        error = "manifest.json: HTTP " + std::to_string(r.http_status) +
                (r.error.empty() ? "" : " " + r.error);
        return false;
    }
    try {
        Manifest m = Manifest::from_json(std::string(r.body.begin(), r.body.end()));
        for (const auto& s : m.segments) out.emplace(s.seq, s.checksum);
    } catch (...) {
        error = "manifest.json is not valid JSON";
        return false;
    }
    return true;
}

} // namespace

MirrorDiff compare_targets(Transport& primary, Transport& second,
                           const std::string& event_id) {
    MirrorDiff d;

    std::map<uint64_t, std::string> a, b;
    std::string ea, eb;
    const bool ok_a = read_manifest(primary, event_id, a, ea);
    const bool ok_b = read_manifest(second, event_id, b, eb);

    // A manifest that cannot be read is reported as itself, not as a difference
    // in content: "the second bucket has no manifest" and "the second bucket is
    // unreachable" are different problems and are fixed differently.
    if (!ok_a && !ok_b) { d.error = "neither target: " + ea; return d; }
    if (!ok_a)          { d.error = "primary: " + ea;       return d; }
    if (!ok_b)          { d.error = "second: " + eb;        return d; }

    for (const auto& [seq, sum] : a) {
        auto it = b.find(seq);
        if (it == b.end()) { d.only_primary.push_back(seq); continue; }
        // A manifest written before checksums were recorded would compare empty
        // against empty and read as verified, which is exactly the false
        // reassurance this is for. Say nothing rather than something wrong.
        if (!sum.empty() && !it->second.empty() && sum != it->second)
            d.checksum_mismatch.push_back(seq);
    }
    for (const auto& [seq, sum] : b)
        if (a.find(seq) == a.end()) d.only_second.push_back(seq);

    // Only the segments still inside each manifest's rolling window are
    // compared, so an old event can legitimately come back "complete" while
    // both targets have aged its early segments out.
    d.complete = d.total() == 0;
    return d;
}

} // namespace multisite
