// SPDX-License-Identifier: GPL-3.0-or-later
//
// aes67.cpp — asking the AES67 daemon on this box what it is doing.
//
// The daemon (aes67-linux-daemon) is somebody else's program with a REST
// interface on loopback. Everything here is either that interface over libcurl
// or one systemctl call, and there is deliberately no more to it than that: the
// shapes being sent and read are in aes67.h, where they can be checked without
// a Pi, and this file is only the part that cannot be.
//
// Two rules run through it:
//
//   • Ask on loopback with a short timeout. The daemon is on this machine, so
//     it answers at once or not at all, and a wedged daemon must never hold up
//     the interface an operator is reading.
//   • Never throw. An unusual body is a state to be reported, not an exception
//     to reach whoever happened to be looking at the settings page.
#include "aes67.h"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace multisite_player {

namespace {

// ── Talking to a process on this machine ─────────────────────────────────────
// libcurl is already linked for the S3 transport, which initialises it once the
// same way. Two initialisations are harmless — curl counts them — and doing it
// here means the AES67 pages work on a box whose storage has never been used.
std::once_flag g_curl_once;
void ensure_curl() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Short, because this is loopback. The connect timeout is the one that matters:
// nothing listening on the port is refused at once, whereas a daemon that has
// accepted a connection and stopped answering is what would otherwise hold a
// request for its whole duration.
constexpr long kConnectTimeoutMs = 500;
constexpr long kRequestTimeoutMs = 2500;

struct HttpReply {
    bool        transport_ok = false;   // libcurl got as far as a reply
    long        status = 0;             // the HTTP status, when it did
    std::string body;
};

size_t append_to_string(char* data, size_t size, size_t nmemb, void* user) {
    static_cast<std::string*>(user)->append(data, size * nmemb);
    return size * nmemb;
}

HttpReply http_request(const char* method, const std::string& url,
                       const std::string& body) {
    HttpReply out;

    ensure_curl();
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return out;

    struct curl_slist* headers = nullptr;
    if (!body.empty())
        headers = curl_slist_append(headers,
                                    "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // The daemon never redirects, so following one would be a surprise — and a
    // stale redirect to another host is not something to obey quietly.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    }
    // CUSTOMREQUEST overrides the method word while POSTFIELDS still supplies
    // the body, which is what a PUT carrying JSON needs.
    if (std::strcmp(method, "PUT") == 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    else if (std::strcmp(method, "DELETE") == 0)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    if (headers != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        out.transport_ok = true;
        out.status = code;
    }

    if (headers != nullptr) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return out;
}

// ── Reading this box ─────────────────────────────────────────────────────────
std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A shell command whose only useful output is its exit status. Nothing here is
// built from anything an operator typed, so there is nothing to quote.
bool command_ok(const std::string& command) {
    FILE* pipe = ::popen((command + " >/dev/null 2>&1").c_str(), "r");
    if (pipe == nullptr) return false;
    return ::pclose(pipe) == 0;
}

// The daemon's own interface address on this box, always loopback: the player
// and the daemon are the same machine, and going out to the network interface
// and back would make the answer depend on the network.
std::string api_base(int port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/api";
}

// What the daemon said instead of doing the thing. The body is included because
// the daemon explains itself in text when it refuses — "(daemon) failed to add
// source" and the like — and that sentence is worth more than the status code.
std::string refusal(const HttpReply& r, const std::string& what) {
    if (!r.transport_ok)
        return "the AES67 daemon did not answer — is it running?";
    std::string detail = r.body;
    if (detail.size() > 200) detail = detail.substr(0, 200) + "…";
    return what + " (HTTP " + std::to_string(r.status) + ")" +
           (detail.empty() ? std::string() : ": " + detail);
}

std::string lowered(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

int aes67_daemon_port() {
    const int from_conf =
        aes67_daemon_port_from_conf(read_text_file(kAes67DaemonConf));
    return from_conf > 0 ? from_conf : kAes67DaemonDefaultPort;
}

std::string aes67_card_device(const std::string& name) {
    const std::string id =
        aes67_card_id_from_cards(read_text_file("/proc/asound/cards"), name);
    // plughw rather than hw: the card takes integer samples and the player
    // hands over floats, so the plug layer is doing the converting. This is the
    // same device string merging-aes67.sh writes.
    return id.empty() ? std::string() : "plughw:CARD=" + id;
}

Aes67State aes67_probe(const std::string& alsa_device, int want_channels,
                       const std::string& want_address,
                       int player_web_port) {
    Aes67State st;
    st.port = aes67_daemon_port();

    // The unit is the honest test of "installed": the installer writes it, and
    // a binary with no unit would not be started after a power cut anyway.
    st.installed = command_ok(std::string("systemctl list-unit-files ") +
                              kAes67DaemonUnit + ".service 2>/dev/null | grep -q '^" +
                              kAes67DaemonUnit + "\\.service'");
    st.service_active =
        command_ok(std::string("systemctl is-active --quiet ") + kAes67DaemonUnit);

    const std::string base = api_base(st.port);

    // ── The clock ────────────────────────────────────────────────────────────
    // Asked first, because it is the one thing the player cannot fix and the one
    // thing that leaves every other correct answer still silent. The daemon
    // answers with a word — "locked", and whatever it says when it is not —
    // which is passed through as it came and reduced to a yes/no here.
    const HttpReply ptp = http_request("GET", base + "/ptp/status", {});
    st.rest_reachable = ptp.transport_ok && ptp.status == 200;
    if (!st.rest_reachable && st.service_active) {
        if (player_web_port > 0 && st.port == player_web_port) {
            // Not a daemon fault at all. Upstream's sample configuration puts
            // the daemon on 8080, which is where this player already is, and
            // the symptom of that is indistinguishable from a broken daemon
            // unless somebody says so.
            st.error = "the AES67 daemon is set to port " +
                       std::to_string(st.port) + ", which is the port this "
                       "player's own interface uses. Two things cannot hold "
                       "one port, so the daemon starts and then answers "
                       "nothing. Change http_port in " +
                       std::string(kAes67DaemonConf) +
                       " to a free port — the installer uses 8081 — and "
                       "restart the daemon.";
        } else {
            st.error = "the AES67 daemon is running but nothing answered on "
                       "port " + std::to_string(st.port) +
                       " — check http_port in " + std::string(kAes67DaemonConf);
        }
    }
    if (st.rest_reachable) {
        nlohmann::json j = nlohmann::json::object();
        try { j = nlohmann::json::parse(ptp.body); } catch (...) {}
        st.ptp_known = true;

        const auto it = j.find("status");
        if (it != j.end() && it->is_string()) {
            st.ptp_status_word = it->get<std::string>();
        } else if (it != j.end() && it->is_number()) {
            // A daemon that reports a code rather than a word: 0 is its own
            // convention for "fine" everywhere else, so it is read that way.
            const int code = it->get<int>();
            st.ptp_status_word = std::to_string(code);
            st.ptp_locked = (code == 0);
        }
        if (!st.ptp_status_word.empty())
            st.ptp_locked =
                st.ptp_locked || lowered(st.ptp_status_word) == "locked";

        const auto gm = j.find("gmid");
        if (gm != j.end() && gm->is_string()) st.ptp_gmid = gm->get<std::string>();
        const auto jt = j.find("jitter");
        if (jt != j.end() && jt->is_number()) st.ptp_jitter = jt->get<double>();
    }

    // ── The source ───────────────────────────────────────────────────────────
    if (st.rest_reachable) {
        const HttpReply list = http_request("GET", base + "/sources", {});
        std::vector<Aes67Source> sources;
        if (list.transport_ok && list.status == 200 &&
            aes67_parse_sources(list.body, sources)) {
            st.sources_known = true;
            const int want = aes67_channels_to_map(want_channels);
            for (const auto& s : sources) {
                if (s.id != kAes67SourceId) continue;
                st.source_present = true;
                st.source_enabled = s.enabled;
                st.source_channels = s.channels;
                st.source_address = s.address;
                st.source_name = s.name;
                st.source_correct = s.matches_shape(want, want_address);
                break;
            }
        }

        // What is actually on the wire, which is worth having separately from
        // what was asked for: it is what a console's engineer will want to see,
        // and the only place the port number appears.
        if (st.source_present) {
            const HttpReply sdp = http_request(
                "GET", base + "/source/sdp/" + std::to_string(kAes67SourceId),
                {});
            if (sdp.transport_ok && sdp.status == 200) {
                const Aes67Sdp parsed = aes67_parse_sdp(sdp.body);
                st.sdp_valid = parsed.valid;
                st.sdp_port = parsed.port;
                st.sdp_codec = parsed.codec;
                st.sdp_channels = parsed.channels;
                st.sdp_ptp = parsed.ptp_referenced;
                st.sdp_text = sdp.body;
            }
        }
    }

    // ── Whether any of it can be heard ───────────────────────────────────────
    // The daemon reads the card; the player writes to it. Both halves have to be
    // true, and neither is visible from inside the other — a source that is
    // perfectly configured carries nothing if the player is still sending the
    // sound to HDMI.
    // A card comparison rather than the file containing the name somewhere: a
    // box with a second card whose name merely contains it ("RAVENNA2") must
    // not have its sound reported as present when it is not.
    st.card_present = aes67_card_present(read_text_file("/proc/asound/cards"),
                                         kAes67CardName);
    st.player_on_card = aes67_device_is_card(alsa_device, kAes67CardName);

    return st;
}

const char* aes67_action_word(Aes67Action a) {
    // "None" is the empty string rather than the word: it is what the log
    // prints for a tick that did nothing, and a tick that did nothing must be
    // able to say nothing at all.
    switch (a) {
    case Aes67Action::None:         return "";
    case Aes67Action::StartService: return "starting the AES67 daemon";
    case Aes67Action::EnsureSource: return "putting the AES67 source right";
    case Aes67Action::RepointCard:  return "pointing the sound at the AES67 card";
    case Aes67Action::WaitForClock: return "waiting for the clock to lock";
    }
    return "";
}

std::string aes67_ensure_source(int channels, const std::string& address,
                                const std::string& name, bool enabled) {
    const std::string url = api_base(aes67_daemon_port()) + "/source/" +
                            std::to_string(kAes67SourceId);

    // The daemon's PUT adds the source if it is not there and replaces it if it
    // is, which is exactly "ensure": one call, and the same call whether this is
    // the first time or the fiftieth.
    const HttpReply r = http_request(
        "PUT", url,
        aes67_source_body(kAes67SourceId, channels, address, name, enabled));
    if (!r.transport_ok || r.status < 200 || r.status >= 300)
        return refusal(r, "the AES67 daemon would not take the source");
    return {};
}

std::string aes67_set_source_enabled(bool enabled) {
    const std::string base = api_base(aes67_daemon_port());

    // The daemon's PUT replaces the whole source rather than patching it, so the
    // source that is there has to be read first and sent back with one field
    // changed. Sending only {"enabled":…} would be refused outright — its parser
    // reads the other keys with no default — and sending a body built from this
    // player's own idea of the source would overwrite an address or a width that
    // somebody had set deliberately, from the daemon's own interface.
    const HttpReply list = http_request("GET", base + "/sources", {});
    if (!list.transport_ok || list.status != 200)
        return refusal(list, "could not read the sources to switch one");

    std::vector<Aes67Source> sources;
    if (!aes67_parse_sources(list.body, sources))
        return "the AES67 daemon's source list was not in a form this player "
               "understands";

    for (const auto& s : sources) {
        if (s.id != kAes67SourceId) continue;
        const HttpReply put = http_request(
            "PUT", base + "/source/" + std::to_string(kAes67SourceId),
            aes67_source_body(s.id, s.channels, s.address, s.name, enabled));
        if (!put.transport_ok || put.status < 200 || put.status >= 300)
            return refusal(put, "the AES67 daemon would not switch the source");
        return {};
    }
    return "there is no AES67 source set up on this box yet";
}

std::string aes67_set_service(bool start, bool enable) {
    // Starting and enabling are different intentions and are asked for
    // separately: enabling is what makes the daemon survive a power cut, which
    // is the whole point of an appliance, while starting is what somebody does
    // when they want the network feed back on.
    if (start) {
        if (!command_ok(std::string("systemctl start ") + kAes67DaemonUnit))
            return "the AES67 daemon would not start — see: journalctl -u " +
                   std::string(kAes67DaemonUnit) + " -n 40";
    } else if (!command_ok(std::string("systemctl stop ") + kAes67DaemonUnit)) {
        return "the AES67 daemon would not stop";
    }

    if (enable) {
        if (!command_ok(std::string("systemctl enable ") + kAes67DaemonUnit))
            return "the AES67 daemon could not be set to start at boot";
    } else if (!command_ok(std::string("systemctl disable ") + kAes67DaemonUnit)) {
        return "the AES67 daemon could not be stopped from starting at boot";
    }
    return {};
}

} // namespace multisite_player
