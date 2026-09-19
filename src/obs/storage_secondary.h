// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// storage_secondary.h — the second bucket, for redundancy (PROJECT-SCOPE.md §10
// Phase 9).
//
// One store per machine, not per role, and not part of either dock's settings:
// it is the same bucket whichever half of the product asks about it. The
// encoder mirrors to it; the decoder falls back to it; a box that is only one
// of those still has exactly one answer to give.
//
// Kept out of DecoderSettings and BroadcastSettings deliberately. Those are per
// role and are already duplicated between the two docks; a third copy of the
// same credentials would be a third thing to drift.
//
#include <string>

namespace multisite { struct S3Config; }

namespace multisite_obs {

struct SecondaryTarget {
    // Off by default, and off means nothing is said or written about it
    // anywhere. Redundancy costs double storage and double origin writes, so
    // it is something an operator turns on, never something they discover.
    bool        enabled = false;
    std::string storage_provider;
    std::string endpoint_host;      // the raw shape, as DecoderSettings keeps it
    std::string r2_account_id;
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";

    // Enabled AND complete enough to write to. The distinction matters: an
    // operator who has ticked the box but not finished typing must be told,
    // not silently given a half-configured mirror.
    bool configured() const {
        return enabled && !bucket.empty() && !access_key_id.empty() &&
               !secret_access_key.empty() &&
               (!endpoint_host.empty() || !r2_account_id.empty());
    }
};

// The measured burst moved to storage_probe.h and now takes the bucket as an
// argument: it measures the LINK, which is not the second bucket's property.

// Fills `out` from the stored target. False when it is not configured, so a
// caller cannot build a transport for a half-typed bucket. Both halves use this
// one conversion rather than each assembling an S3Config of their own.
bool secondary_s3_config(multisite::S3Config& out);

SecondaryTarget secondary_target();
void set_secondary_target(const SecondaryTarget& t);

} // namespace multisite_obs
