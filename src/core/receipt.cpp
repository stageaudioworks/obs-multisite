// SPDX-License-Identifier: GPL-3.0-or-later
#include "receipt.h"
#include "../vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>

using json = nlohmann::json;

namespace multisite {

std::string receipt_key(const std::string& event_id) {
    return "events/" + event_id + "/receipt.json";
}

DeliveryReceipt make_receipt(const Manifest& m, bool any_enqueued,
                             uint64_t last_enqueued, uint64_t pending) {
    DeliveryReceipt r;
    r.event_id  = m.event_id;
    r.first_seq = m.first_available_seq;
    r.group_k   = m.group_k;
    if (any_enqueued && last_enqueued >= r.first_seq) {
        r.last_seq = last_enqueued;
        r.captured = last_enqueued - r.first_seq + 1;
    }
    // Gaps inside what was captured only; a gap can never lie outside it, but
    // a malformed manifest should not be able to make `lost` exceed `captured`.
    for (const auto& g : m.gaps) {
        if (r.captured == 0 || g.first > r.last_seq || g.second < r.first_seq) continue;
        const uint64_t a = std::max(g.first, r.first_seq);
        const uint64_t b = std::min(g.second, r.last_seq);
        r.gaps.emplace_back(a, b);
        r.lost += b - a + 1;
    }
    r.pending = std::min<uint64_t>(pending, r.captured - std::min(r.captured, r.lost));
    r.in_cloud = r.captured - r.lost - r.pending;
    return r;
}

std::string DeliveryReceipt::state() const {
    if (lost > 0) return "incomplete";
    if (pending > 0) return "finishing";
    return "complete";
}

std::string DeliveryReceipt::summary() const {
    std::string s = std::to_string(in_cloud) + " of " + std::to_string(captured) +
                    " segments are in the cloud";
    if (lost > 0)
        s += "; " + std::to_string(lost) + " were lost before upload (see gaps)";
    if (pending > 0)
        s += "; " + std::to_string(pending) + " still uploading in the background";
    if (lost == 0 && pending == 0 && captured > 0) s += " — complete";
    if (mirror_configured)
        s += mirror_complete ? "; the second bucket has all of it"
                             : "; the second bucket is still catching up";
    return s;
}

std::string DeliveryReceipt::to_json() const {
    json gp = json::array();
    for (const auto& g : gaps) gp.push_back({ g.first, g.second });
    json j = {
        {"protocol_version", protocol_version}, {"event_id", event_id},
        {"writer", writer}, {"written_at_ms", written_at_ms},
        {"state", state()},
        {"first_seq", first_seq}, {"last_seq", last_seq},
        {"captured", captured}, {"in_cloud", in_cloud},
        {"lost", lost}, {"pending", pending}, {"gaps", gp},
        {"group_k", group_k},
    };
    if (mirror_configured) {
        j["mirror_configured"] = true;
        j["mirror_complete"]   = mirror_complete;
    }
    return j.dump();
}

DeliveryReceipt DeliveryReceipt::from_json(const std::string& s) {
    json j = json::parse(s);
    DeliveryReceipt r;
    r.protocol_version = j.value("protocol_version", 1);
    r.event_id   = j.value("event_id", "");
    r.writer     = j.value("writer", "");
    r.written_at_ms = j.value("written_at_ms", (int64_t)0);
    r.first_seq  = j.value("first_seq", (uint64_t)0);
    r.last_seq   = j.value("last_seq", (uint64_t)0);
    r.captured   = j.value("captured", (uint64_t)0);
    r.in_cloud   = j.value("in_cloud", (uint64_t)0);
    r.lost       = j.value("lost", (uint64_t)0);
    r.pending    = j.value("pending", (uint64_t)0);
    r.group_k    = j.value("group_k", 0);
    r.mirror_configured = j.value("mirror_configured", false);
    r.mirror_complete   = j.value("mirror_complete", false);
    if (j.contains("gaps") && j["gaps"].is_array())
        for (const auto& g : j["gaps"])
            if (g.is_array() && g.size() == 2)
                r.gaps.emplace_back(g[0].get<uint64_t>(), g[1].get<uint64_t>());
    return r;
}

} // namespace multisite
