// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// receipt.h — what an event delivered, said once, in numbers.
//
// The answer to "did everything we recorded reach the cloud?" used to be
// assembled by an engineer from a log. A receipt is written beside the event
// (events/{id}/receipt.json) when End finishes, and rewritten by EventFinisher
// when a backlog left at End lands, so the latest one is always the truth:
//
//   captured   segments the encoder produced for the event
//   in_cloud   of those, confirmed stored (by the store's 2xx)
//   lost       never uploaded — evicted for disk space or damaged on the
//              encoder's disk — each listed in `gaps`
//   pending    still in the encoder's spool, to be uploaded in the background
//
// in_cloud is the encoder's count, from the write-ordering invariant (§4.7):
// it is captured − lost − pending, not a listing of the bucket. Reporting
// only; nothing is re-sent because of it (docs/adr/0003 §1).
//
// docs/scope/upload-delivery-assurance.md R1. The campus side of the receipt
// (what each campus received) belongs to the collector, in multisite-cloud.
//
#include "model.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace multisite {

struct DeliveryReceipt {
    int         protocol_version = kProtocolVersion;
    std::string event_id;
    std::string writer;          // "encoder" at End, "finisher" after
    int64_t     written_at_ms = 0;
    uint64_t    first_seq = 0;
    uint64_t    last_seq = 0;    // meaningful only when captured > 0
    uint64_t    captured = 0;
    uint64_t    in_cloud = 0;
    uint64_t    lost = 0;
    uint64_t    pending = 0;
    std::vector<std::pair<uint64_t, uint64_t>> gaps;
    int         group_k = 0;     // segment groups published (parity.h), 0 none
    // The second bucket, when one is configured.
    bool        mirror_configured = false;
    bool        mirror_complete = false;

    // "complete" (everything captured is in the cloud), "finishing" (some is
    // still to upload, none lost) or "incomplete" (something was lost).
    std::string state() const;
    // One sentence for a log line or the dock.
    std::string summary() const;
    std::string to_json() const;
    static DeliveryReceipt from_json(const std::string&);
};

// Builds the counts from the event's manifest (its first sequence and its
// gaps) and the spool's view (the last sequence enqueued, if any, and how many
// are still pending).
DeliveryReceipt make_receipt(const Manifest& m, bool any_enqueued,
                             uint64_t last_enqueued, uint64_t pending);

std::string receipt_key(const std::string& event_id);

} // namespace multisite
