// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// mirror_verify.h — is the second copy really a copy?
// See PROJECT-SCOPE.md §10 Phase 9.
//
// Everything else in the redundancy work makes the second bucket receive
// objects; this is the only part that says whether it actually has them. The
// protocol already carries a checksum per segment (§4.7), so a comparison is
// arithmetic rather than a re-read — which is what makes "mirrored" a fact
// instead of a claim, and what would have caught a segment that never reached
// the second bucket at all.
//
#include "transport.h"

#include <string>
#include <vector>

namespace multisite {

struct MirrorDiff {
    // Segments listed by one target's manifest and not the other's. Either side
    // can be short: the primary is short whenever the encoder has been writing
    // to the second bucket, and vice versa.
    std::vector<uint64_t> only_primary;
    std::vector<uint64_t> only_second;
    // In both, with different contents. This is the one that matters most: an
    // object that exists in both but differs is worse than one that is simply
    // missing, because nothing downstream would ever notice.
    std::vector<uint64_t> checksum_mismatch;

    bool        complete = false;   // no differences of any kind
    std::string error;              // non-empty when a manifest could not be read

    bool   ok() const { return error.empty(); }
    size_t total() const {
        return only_primary.size() + only_second.size() + checksum_mismatch.size();
    }
};

// Compares the two targets' manifests for one event. Reads only the manifests,
// so it costs two requests rather than the whole event.
MirrorDiff compare_targets(Transport& primary, Transport& second,
                           const std::string& event_id);

} // namespace multisite
