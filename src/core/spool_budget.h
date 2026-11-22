// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// spool_budget.h — how much unsent media the encoder may keep on its disk.
//
// The cap used to be a fixed 4 GiB. That is about 72 minutes of an 8 Mbit/s
// programme, so an outage a little over an hour deleted the start of the
// service on an encoder that typically had hundreds of GB free
// (docs/scope/upload-delivery-assurance.md, E3 and R2). The cap is now what
// the disk can spare: everything free except a reserve kept for the operating
// system, OBS's own recording and everything else on the machine.
//
// Pure arithmetic over numbers the caller measures (std::filesystem::space),
// like disk_health.h, so it is tested without a disk.
//
#include <algorithm>
#include <cstdint>

namespace multisite {

struct SpoolBudget {
    // Always left free: the larger of an absolute floor and a fraction of the
    // drive. The floor protects a small drive, the fraction a large one that
    // other software expects to keep some headroom on.
    uint64_t reserve_floor_bytes = 20ull * 1024 * 1024 * 1024;   // 20 GiB
    double   reserve_fraction    = 0.10;
    // Never less than this, even on a nearly full disk — but never more than
    // half of what is actually free, so a full disk is not made fuller by it.
    uint64_t min_cap_bytes       = 1ull * 1024 * 1024 * 1024;    // 1 GiB
};

// The cap for the spool, in bytes of unconfirmed segments, given what is
// pending now and the disk under it. `pending` is already on the disk, so the
// spool's share is pending + free, less the reserve.
//
// The cap can come out BELOW `pending` — that is the point. A disk that fills
// for other reasons (OBS recording to it) must shrink the spool, and a first
// version computed "pending + whatever is spare" instead, which can never be
// under pending: on a full disk it dropped nothing and went on filling it.
// test_spool_budget §4 caught that.
inline uint64_t spool_cap_from_disk(uint64_t pending, uint64_t free_bytes,
                                    uint64_t capacity_bytes,
                                    const SpoolBudget& b = {}) {
    const uint64_t reserve = std::max<uint64_t>(
        b.reserve_floor_bytes, (uint64_t)((double)capacity_bytes * b.reserve_fraction));
    const uint64_t share = pending + free_bytes;
    const uint64_t cap   = share > reserve ? share - reserve : 0;
    const uint64_t floor = std::min<uint64_t>(b.min_cap_bytes, share / 2);
    return std::max<uint64_t>(cap, floor);
}

// How long the spool could go on absorbing the programme if nothing more were
// uploaded — the worst case, a link that stays down — before it starts
// dropping the oldest unsent segments. -1 when it cannot be said (no
// bitrate measured yet). Seconds.
inline double seconds_until_spool_drops(uint64_t cap, uint64_t pending,
                                        double encode_bytes_per_s) {
    if (encode_bytes_per_s <= 0.0) return -1.0;
    if (pending >= cap) return 0.0;
    return (double)(cap - pending) / encode_bytes_per_s;
}

} // namespace multisite
