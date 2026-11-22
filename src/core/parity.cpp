// SPDX-License-Identifier: GPL-3.0-or-later
#include "parity.h"
#include "checksum.h"
#include "../vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>

using json = nlohmann::json;

namespace multisite {

const GroupMember* GroupIndex::member(uint64_t seq) const {
    for (const auto& m : members)
        if (m.seq == seq) return &m;
    return nullptr;
}

std::string GroupIndex::to_json() const {
    json ms = json::array();
    for (const auto& m : members)
        ms.push_back({ {"seq", m.seq}, {"len", m.len}, {"checksum", m.checksum} });
    return json({ {"k", k}, {"group", group}, {"parity_len", parity_len},
                  {"members", ms} }).dump();
}

GroupIndex GroupIndex::from_json(const std::string& s) {
    json j = json::parse(s);
    GroupIndex g;
    g.k          = j.value("k", 0);
    g.group      = j.value("group", (uint64_t)0);
    g.parity_len = j.value("parity_len", (uint64_t)0);
    if (j.contains("members") && j["members"].is_array())
        for (const auto& m : j["members"]) {
            GroupMember gm;
            gm.seq      = m.value("seq", (uint64_t)0);
            gm.len      = m.value("len", (uint64_t)0);
            gm.checksum = m.value("checksum", "");
            g.members.push_back(gm);
        }
    return g;
}

static std::string numbered(const std::string& prefix, const char* dir,
                            uint64_t group, const char* ext) {
    char b[24];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)group);
    return prefix + dir + "/" + b + ext;
}
std::string group_index_key(const std::string& prefix, uint64_t group) {
    return numbered(prefix, "groups", group, ".json");
}
std::string group_parity_key(const std::string& prefix, uint64_t group) {
    return numbered(prefix, "parity", group, ".bin");
}

// ── Accumulator ──────────────────────────────────────────────────────────────

void ParityAccumulator::open(uint64_t group) {
    m_open  = true;
    m_group = group;
    m_next  = group * (uint64_t)m_k;
    m_index = GroupIndex{};
    m_index.k = m_k;
    m_index.group = group;
    m_acc.clear();
}

std::optional<ParityAccumulator::Finished> ParityAccumulator::close() {
    if (!m_open) return std::nullopt;
    m_open = false;
    if (m_index.members.empty()) return std::nullopt;   // nothing to protect
    Finished f;
    m_index.parity_len = m_acc.size();
    f.index  = std::move(m_index);
    f.parity = std::move(m_acc);
    // Both left in a known state, not merely a valid one: open() resets them
    // before the next use, but nothing should depend on that ordering.
    m_index = GroupIndex{};
    m_acc.clear();
    return f;
}

std::vector<ParityAccumulator::Finished> ParityAccumulator::pass(uint64_t seq) {
    std::vector<Finished> out;
    if (m_k <= 0) return out;
    const uint64_t g = group_of(seq, m_k);
    if (m_open && g != m_group) {
        if (auto f = close()) out.push_back(std::move(*f));
    }
    if (!m_open) open(g);
    if (seq >= m_next) m_next = seq + 1;
    return out;
}

std::vector<ParityAccumulator::Finished> ParityAccumulator::add(
        uint64_t seq, const std::vector<uint8_t>& bytes, const std::string& checksum) {
    std::vector<Finished> out = pass(seq);
    if (m_k <= 0) return out;
    if (m_acc.size() < bytes.size()) m_acc.resize(bytes.size(), 0);
    for (size_t i = 0; i < bytes.size(); ++i) m_acc[i] ^= bytes[i];
    m_index.members.push_back({ seq, (uint64_t)bytes.size(), checksum });
    if (m_next == (m_group + 1) * (uint64_t)m_k)
        if (auto f = close()) out.push_back(std::move(*f));
    return out;
}

std::vector<ParityAccumulator::Finished> ParityAccumulator::skip(uint64_t seq) {
    std::vector<Finished> out = pass(seq);
    if (m_k <= 0) return out;
    if (m_next == (m_group + 1) * (uint64_t)m_k)
        if (auto f = close()) out.push_back(std::move(*f));
    return out;
}

std::optional<ParityAccumulator::Finished> ParityAccumulator::flush() {
    return close();
}

// ── Rebuild ──────────────────────────────────────────────────────────────────

std::optional<std::vector<uint8_t>> rebuild_member(
        const GroupIndex& index, uint64_t missing,
        const std::vector<uint8_t>& parity,
        const std::map<uint64_t, std::vector<uint8_t>>& others) {
    const GroupMember* target = index.member(missing);
    if (!target || index.parity_len == 0 || parity.size() != index.parity_len ||
        target->len > parity.size())
        return std::nullopt;
    std::vector<uint8_t> acc = parity;
    for (const auto& m : index.members) {
        if (m.seq == missing) continue;
        auto it = others.find(m.seq);
        if (it == others.end() || it->second.size() != m.len) return std::nullopt;
        for (size_t i = 0; i < it->second.size(); ++i) acc[i] ^= it->second[i];
    }
    acc.resize(target->len);
    if (!verify_sha256(acc, target->checksum)) return std::nullopt;
    return acc;
}

} // namespace multisite
