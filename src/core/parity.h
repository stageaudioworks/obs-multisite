// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// parity.h — segment groups: a checksum index and XOR recovery data, so a
// campus can verify any segment and rebuild one the store lost or damaged,
// with nothing ever re-sent (docs/scope/upload-delivery-assurance.md R5,
// docs/adr/0004).
//
// Every `k` consecutive sequence numbers form a group (group = seq / k). Once
// every member the encoder will ever upload is confirmed, it publishes:
//
//   events/{id}/parity/{group:08d}.bin   XOR of the members, each zero-padded
//                                        to the longest
//   events/{id}/groups/{group:08d}.json  { k, group, parity_len,
//                                          members: [{seq, len, checksum}] }
//
// parity first, the index after it, so an index that names parity means the
// parity exists (the same write-ordering rule as §4.7). The manifest's
// `group_k` says groups exist and how wide they are. All three are additive:
// a reader that predates them ignores them, so no protocol bump (§4.8).
//
// Two jobs, one object:
//   • The index carries every member's checksum for the whole event. The
//     manifest lists checksums only for its rolling window (50 segments), so a
//     campus further behind than that — a timeslip campus, routinely — stored
//     segments with no check at all.
//   • With the parity, any ONE member of a group that is missing or fails its
//     checksum can be rebuilt from the other members. The checksum says which
//     member is bad, so this is an erasure, and XOR suffices.
//
// Who forms groups. The live Session, for every segment it confirms; and the
// EventFinisher, for the backlog an End left (event_finisher.h). Both publish
// through an ObjectQueue in `<spool_root>.objects/<event_id>`, so groups an
// End could not send are drained by the finisher too. With a second bucket
// configured, the Session queues a copy for it in `<spool_root>.mirror-objects`
// that waits on the mirror's yield rule (session.h).
//
// A group published in part, then carried on. End stopped mid-group publishes
// that group's partial index (flush()), and the finisher later uploads the
// rest of its members. The finisher CARRIES THE GROUP ON rather than starting
// at the next boundary: it reads the stored index and parity back, resume()s
// from them, and XORs its own members in — XOR is associative and the
// padding is zeros, so the result is exactly the parity of the union. It then
// overwrites both objects, parity first. That is safe only because the new
// members set is a strict SUPERSET of the old: every member the old index
// named is still named, with the same checksum and length, so whatever a
// campus already verified against the old index stays true, and a campus
// that reads the new parity with the old index (or the reverse) gets a
// rebuild that fails its checksum and is refused, never wrong bytes.
// resume() refuses anything that would not be a superset — a stored group
// naming a segment still to come, parity of the wrong length, another group —
// and the finisher then leaves that group as End published it and begins at
// the next boundary: those members go unprotected, which is what they were
// before groups existed. The cost of carrying on is one GET of the parity
// (one segment's size) per event. Starting at the next boundary instead
// would leave up to k-1 segments at the very end of every slow-uplink event
// with no group at all — exactly the segments most likely to need one. A
// campus that cached the old index keeps it (decoder_session.cpp caches
// indexes per group); the new members are then unverified for that campus,
// as they would have been without groups, and nothing it holds is wrong.
//
// Patent limits (docs/adr/0003): the uploader runs no test and nothing is
// re-sent; k is fixed by configuration and never adapted to measured loss or
// latency (§5). Members lost before upload (gaps) are simply not members.
//
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace multisite {

struct GroupMember {
    uint64_t    seq = 0;
    uint64_t    len = 0;
    std::string checksum;   // sha256 hex
};

struct GroupIndex {
    int         k = 0;
    uint64_t    group = 0;
    uint64_t    parity_len = 0;   // 0: no parity for this group
    std::vector<GroupMember> members;

    const GroupMember* member(uint64_t seq) const;
    std::string to_json() const;
    // Throws on malformed input, like the other from_json functions.
    static GroupIndex from_json(const std::string&);
};

inline uint64_t group_of(uint64_t seq, int k) { return k > 0 ? seq / (uint64_t)k : 0; }
// `prefix` is "events/{id}/".
std::string group_index_key(const std::string& prefix, uint64_t group);
std::string group_parity_key(const std::string& prefix, uint64_t group);

// Folds confirmed segments into the current group, in sequence order.
class ParityAccumulator {
public:
    explicit ParityAccumulator(int k) : m_k(k) {}
    int k() const { return m_k; }

    // A finished group to publish.
    struct Finished {
        GroupIndex           index;
        std::vector<uint8_t> parity;
    };

    // Add a confirmed segment. Returns the group(s) this completed or left
    // behind: a segment in a later group closes the open one, whatever it
    // holds (its other members were lost before upload).
    std::vector<Finished> add(uint64_t seq, const std::vector<uint8_t>& bytes,
                              const std::string& checksum);
    // A sequence number the encoder will never upload (a gap). Completes the
    // group when it was the last one outstanding.
    std::vector<Finished> skip(uint64_t seq);
    // Close the open group with whatever it holds — at End.
    std::optional<Finished> flush();
    // Carry on a group already published in part (see the header), from its
    // stored index and parity, so that `next_seq` onwards join it. Refused —
    // false, nothing changed — unless the result can only be a superset:
    // nothing open yet, the same k, `next_seq` in that group, the parity as
    // long as the index says, and every listed member before `next_seq`.
    bool resume(const GroupIndex& index, const std::vector<uint8_t>& parity,
                uint64_t next_seq);

private:
    int                  m_k;
    bool                 m_open = false;
    uint64_t             m_group = 0;
    uint64_t             m_next = 0;       // next seq expected in the group
    GroupIndex           m_index;
    std::vector<uint8_t> m_acc;

    std::optional<Finished> close();
    void open(uint64_t group);
    // Advance past `seq`, closing groups as their last member goes by.
    std::vector<Finished> pass(uint64_t seq);
};

// Rebuild `missing` from the group's parity and every other member's bytes.
// Returns the bytes only when they match the index's checksum for it.
std::optional<std::vector<uint8_t>> rebuild_member(
    const GroupIndex& index, uint64_t missing,
    const std::vector<uint8_t>& parity,
    const std::map<uint64_t, std::vector<uint8_t>>& others);

} // namespace multisite
