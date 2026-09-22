// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace multisite {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

struct QueryParam { std::string key, value; };

std::vector<QueryParam> split_query(const std::string& q) {
    std::vector<QueryParam> out;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        const std::string one = q.substr(i, amp - i);
        i = amp + 1;
        if (one.empty()) continue;
        const size_t eq = one.find('=');
        if (eq == std::string::npos) out.push_back({one, ""});
        else out.push_back({one.substr(0, eq), one.substr(eq + 1)});
    }
    return out;
}

std::string join_query(const std::vector<QueryParam>& ps) {
    std::string out;
    for (const auto& p : ps) {
        if (!out.empty()) out += "&";
        out += p.key + "=" + p.value;
    }
    return out;
}

// ffmpeg's SRT timing options are given in MILLIONTHS of a second, not
// thousandths. A church pasting an address from a vendor page that says
// "latency=2000" meaning two seconds is therefore asking ffmpeg for two
// milliseconds, and would get a connection that falls apart on the first lost
// packet. We store and show milliseconds, convert on the way out, and convert
// on the way in — so a pasted address means here exactly what it would have
// meant to ffmpeg, and a value that comes out absurdly small is caught by
// validate() with the units spelled out rather than silently used.
constexpr int64_t kUsPerMs = 1000;

const char kSrtScheme[] = "srt://";

} // namespace

Protocol protocol_of(const Destination& d) {
    return starts_with(lower(d.url), kSrtScheme) ? Protocol::Srt
                                                 : Protocol::Rtmp;
}

bool is_listener(const Destination& d) {
    return protocol_of(d) == Protocol::Srt && d.srt_mode == SrtMode::Listener;
}

void normalize(Destination& d) {
    d.name           = trim(d.name);
    d.url            = trim(d.url);
    d.stream_key     = trim(d.stream_key);
    d.srt_passphrase = trim(d.srt_passphrase);

    if (protocol_of(d) != Protocol::Srt) {
        // RTMP keeps the shape it has always had: the key is a separate path
        // component, appended at spawn time, and never part of the address.
        while (!d.url.empty() && d.url.back() == '/') d.url.pop_back();
        d.srt_passphrase.clear();
        d.srt_latency_ms = 0;
        d.srt_mode = SrtMode::Caller;
        return;
    }

    // Split the address from its query. Everything we manage ourselves comes
    // out into its own field; anything else stays on the address untouched.
    std::string base = d.url, query;
    const size_t q = d.url.find('?');
    if (q != std::string::npos) {
        base  = d.url.substr(0, q);
        query = d.url.substr(q + 1);
    }
    while (!base.empty() && base.back() == '/') base.pop_back();

    std::vector<QueryParam> keep;
    for (const auto& p : split_query(query)) {
        const std::string k = lower(p.key);
        if (k == "streamid") {
            if (d.stream_key.empty()) d.stream_key = p.value;
        } else if (k == "passphrase") {
            if (d.srt_passphrase.empty()) d.srt_passphrase = p.value;
        } else if (k == "latency") {
            if (d.srt_latency_ms == 0) {
                try {
                    d.srt_latency_ms = (int)(std::stoll(p.value) / kUsPerMs);
                } catch (...) { /* left at 0, so our own default is used */ }
            }
        } else if (k == "mode") {
            if (lower(p.value) == "listener") d.srt_mode = SrtMode::Listener;
        } else {
            keep.push_back(p);
        }
    }

    // An address with no host at all — srt://:9000 — is how a listener is
    // usually written down, and is the one place the mode can be read off the
    // address itself.
    std::string hostport = base.substr(sizeof(kSrtScheme) - 1);
    if (hostport.empty() || hostport[0] == ':') {
        d.srt_mode = SrtMode::Listener;
        base = std::string(kSrtScheme) + "0.0.0.0" + hostport;
    }

    // Nothing for the far end to be told when the far end is the one calling
    // us: SRT carries a stream id from caller to listener, never the other
    // way. Dropped rather than stored, so it cannot sit in the database
    // looking like a setting that is being ignored.
    if (d.srt_mode == SrtMode::Listener) d.stream_key.clear();

    const std::string rest = join_query(keep);
    d.url = rest.empty() ? base : base + "?" + rest;
}

bool affects_stream(const Destination& a, const Destination& b) {
    return a.url != b.url
        || a.stream_key != b.stream_key
        || a.audio.label != b.audio.label
        || a.allow_transcode != b.allow_transcode
        || a.delay_s != b.delay_s
        || a.room_id != b.room_id
        || a.srt_mode != b.srt_mode
        || a.srt_passphrase != b.srt_passphrase
        || a.srt_latency_ms != b.srt_latency_ms;
}

std::string validate(const Destination& d) {
    if (d.name.empty())
        return "Give this destination a name, so you can tell it apart from "
               "the others.";
    if (d.room_id.empty())
        return "Choose which feed this destination should send.";

    const std::string u = lower(d.url);
    if (u.empty())
        return "Paste the server address from the streaming site — it starts "
               "with rtmp:// or srt://";

    // Bounds rather than opinions: below about ten seconds there is nothing to
    // absorb a hiccup with, and beyond an hour the operator has almost
    // certainly typed minutes into a seconds box.
    if (d.delay_s != 0 && (d.delay_s < 10 || d.delay_s > 3600))
        return "The delay should be between 10 seconds and an hour.";

    if (protocol_of(d) == Protocol::Rtmp) {
        if (!starts_with(u, "rtmp://") && !starts_with(u, "rtmps://"))
            return "That does not look like a streaming address. It should "
                   "start with rtmp://, rtmps:// or srt://";
        // A stream key pasted into the address field is the commonest setup
        // mistake, and it fails at the destination with nothing useful said.
        if (d.stream_key.empty())
            return "Paste the stream key from the streaming site as well. It "
                   "is usually shown next to the server address.";
        return {};
    }

    // ── SRT ──────────────────────────────────────────────────────────────────
    std::string hostport = d.url.substr(sizeof(kSrtScheme) - 1);
    const size_t q = hostport.find('?');
    if (q != std::string::npos) hostport = hostport.substr(0, q);

    const size_t colon = hostport.rfind(':');
    if (colon == std::string::npos || colon + 1 >= hostport.size())
        return "An SRT address needs a port on the end, like "
               "srt://stream.example.com:9000";
    int port = 0;
    try { port = std::stoi(hostport.substr(colon + 1)); } catch (...) {}
    if (port < 1 || port > 65535)
        return "That port number is not one a computer can use. It should be "
               "between 1 and 65535.";
    if (colon == 0)
        return "An SRT address needs the far end's name or number before the "
               "port, like srt://stream.example.com:9000";

    // ffmpeg reads these straight out of the address without decoding them,
    // so a value containing any of these would be cut short and the
    // connection refused for a reason nothing reports. Caught here, where it
    // can be explained, rather than at 10:29 on a Sunday.
    const std::string* secrets[] = { &d.stream_key, &d.srt_passphrase };
    for (const auto* s : secrets) {
        if (s->find('&') != std::string::npos ||
            s->find('#') != std::string::npos ||
            s->find(' ') != std::string::npos)
            return "The stream key and passphrase cannot contain a space, an "
                   "& or a #. If the streaming site gave you one with those "
                   "in it, please report it.";
    }

    if (!d.srt_passphrase.empty() &&
        (d.srt_passphrase.size() < 10 || d.srt_passphrase.size() > 79))
        return "An SRT passphrase has to be between 10 and 79 characters. "
               "Leave it empty if the streaming site did not give you one.";

    if (d.srt_latency_ms != 0 &&
        (d.srt_latency_ms < 20 || d.srt_latency_ms > 8000))
        return "The SRT latency should be between 20 and 8000 milliseconds. "
               "If it came in as part of the address, note that an SRT "
               "address gives it in millionths of a second — 2000000 there "
               "means 2000 here.";

    return {};
}

} // namespace multisite
