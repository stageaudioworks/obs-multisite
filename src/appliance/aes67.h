// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// aes67.h — the text and arithmetic of putting the sound on the network.
//
// The AES67 stack is not ours to write: it is Merging's kernel module and the
// GPL aes67-daemon, installed by scripts/player/merging-aes67.sh. What this
// program can do about it is configure it, watch it and switch it — all through
// the daemon's REST interface on this box.
//
// Everything in this header is that interface's shapes and nothing else: the
// JSON a source is made of, the JSON the daemon answers with, and the SDP it
// publishes. Nothing here opens a socket, runs a command or touches a device,
// so it can be checked anywhere — which matters, because the parts that go
// quietly wrong are exactly these: the channel map that decides which eight
// channels go where, and the reading back of what the daemon says it is sending.
//
// `tests/test_aes67.cpp` checks all of it on a laptop, with no Pi and no card.
// The part that does talk to the daemon is in aes67.cpp, and is deliberately
// not in here.
#include <string>
#include <vector>

#include "../vendor/nlohmann/json.hpp"

namespace multisite_player {

// ── The daemon's own limits and vocabulary ───────────────────────────────────
// From aes67-linux-daemon's daemon/README.md and daemon/CMakeLists.txt: sources
// and sinks are numbered 0-63, at most 64 of each, and a stream carries at most
// 64 channels.
constexpr int kAes67MaxStreams  = 64;
constexpr int kAes67MaxChannels = 64;

// The one source this player owns. A fixed id rather than "the first free one"
// is what keeps everything else simple: the multicast address is predictable,
// "is it set up the way we want it?" is a comparison rather than a search, and
// running this twice cannot quietly leave a second stream transmitting.
constexpr int kAes67SourceId = 0;

// AES67 carries L16 or L24. The appliance's device is addressed as `plughw:`,
// whose plug layer hands the card L24, so the stream asks for the same thing —
// announcing L16 while sending L24 is the sort of mismatch a console decodes as
// noise.
constexpr const char* kAes67Codec = "L24";
constexpr int kAes67SampleRate   = 48000;
// The RTP payload type AES67 assigns to L24. A receiver told 98 that is sent
// something else will not decode it.
constexpr int kAes67PayloadType = 98;
// 48 samples is one millisecond at 48 kHz, the AES67 packet time: the daemon's
// own default and what its tests run at.
constexpr int kAes67SamplesPerPacket = 48;
// Hop limit and DSCP the daemon's own interface defaults to (34 is AF41, the
// expedited-forwarding class AES67 audio is expected to use).
constexpr int kAes67Ttl  = 15;
constexpr int kAes67Dscp = 34;
// Eight channels by default: the width the open stack was proven at, and one
// that suits a stream carrying a room's own mix rather than a stereo listener
// feed. What those channels contain is the campus's business — this carries
// them.
constexpr int kAes67DefaultChannels = 8;

// Where the daemon's own configuration is, and the port it uses when its file
// says nothing. The installer makes this 8081 because the player already holds
// 8080; the player reads the file rather than assuming it, so a box that was
// given another port still works.
constexpr const char* kAes67DaemonConf = "/etc/daemon.conf";
constexpr int kAes67DaemonDefaultPort = 8081;
// The systemd unit, and the name the kernel module registers its card under.
// Both are the installer's, and both are read back rather than guessed at.
constexpr const char* kAes67DaemonUnit = "aes67-daemon";
constexpr const char* kAes67CardName   = "RAVENNA";

namespace aes67_detail {

// The daemon's JSON is read defensively. Its own parser uses get<>() with no
// default, so a body missing a key is rejected — fine for what the daemon will
// accept, and fatal for what this side must survive: an older daemon, a newer
// one, or a half-written status file has to be reported as "not what we
// expected", never as a crash in the interface an operator is reading.
inline std::string str(const nlohmann::json& j, const char* k) {
    const auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>()
                                              : std::string();
}
inline int num(const nlohmann::json& j, const char* k, int fallback) {
    const auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<int>() : fallback;
}
inline bool flag(const nlohmann::json& j, const char* k, bool fallback) {
    const auto it = j.find(k);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

} // namespace aes67_detail

// ── A source, as the daemon describes one ────────────────────────────────────
struct Aes67Source {
    int         id = -1;
    bool        enabled = false;
    std::string name;
    std::string address;      // the multicast address it sends to
    std::string codec;        // L16, L24
    int         channels = 0; // how many entries the daemon's channel map has
    std::string io;           // "Audio Device" reads the ALSA card
    int         payload_type = 0;

    // Everything about the source except the on/off switch. The switch belongs
    // to the operator, so "make sure it is set up correctly" must not quietly
    // switch a stream back on that somebody deliberately turned off.
    bool matches_shape(int want_channels,
                       const std::string& want_address) const {
        return id == kAes67SourceId &&
               channels == want_channels &&
               codec == kAes67Codec &&
               payload_type == kAes67PayloadType &&
               (want_address.empty() || address == want_address);
    }
};

// ── Building what we send ────────────────────────────────────────────────────
// The channel map is the part that decides the sound: it names the ALSA
// playback channels, in order, that become the stream's channels. An eight
// channel source is map [0,1,2,3,4,5,6,7] — the first eight channels the player
// writes to the card.
//
// The count does NOT follow the feed, and that is deliberate. A network stream
// has a width that receivers configure against; resizing it every time a
// different event carries a different number of tracks would make every
// receiving console re-learn the stream, and several would simply drop it. So
// the width is fixed, and a feed with fewer channels leaves the remainder
// silent — which is what the ALSA path already does, and already says in the
// log.
inline int aes67_channels_to_map(int want_channels) {
    int want = want_channels > 0 ? want_channels : kAes67DefaultChannels;
    if (want > kAes67MaxChannels) want = kAes67MaxChannels;
    return want < 1 ? 1 : want;
}

// The card a device id names, if it names one. ALSA's identifiers come in three
// shapes — "hw:CARD=RAVENNA,DEV=0", "plughw:CARD=RAVENNA,DEV=0" and the
// card name on its own — and the comparison below has to see through all three
// without matching a card whose name merely contains this one.
//
// The trap is "RAVENNA" as a substring: "RAVENNA2" and "NOTRAVENNA" both
// contain it, and a box with either would have its sound device reported as
// taken over by the network output when it is not. So a CARD= form is read out
// and compared whole, and the bare form is compared whole as well.
inline bool aes67_device_is_card(const std::string& device,
                                 const std::string& name) {
    if (device.empty() || name.empty()) return false;

    const std::string key = "CARD=";
    const size_t at = device.find(key);
    if (at != std::string::npos) {
        size_t begin = at + key.size();
        size_t end = device.find_first_of(",:", begin);
        const std::string card = device.substr(begin, end - begin);
        return card == name;
    }
    // No CARD=: the whole string is the card, unless it is a plugin whose tail
    // is a device index rather than a name ("hw:0"). Those name a card by
    // number, which cannot be matched against a name, so they never match.
    if (device.find(':') != std::string::npos) return false;
    return device == name;
}

// Where a device sits on the list. -1 when it is not there.
inline int aes67_find_device(const std::vector<std::string>& ids,
                             const std::string& card) {
    for (size_t i = 0; i < ids.size(); ++i)
        if (aes67_device_is_card(ids[i], card)) return (int)i;
    return -1;
}

// The one decision both the reconciler and the settings page ask: which ALSA
// device should the sound be on?
//
// It exists because those two answering it separately is exactly how the
// setting and the sound came to disagree — the page offered a device while the
// reconciler was about to move the sound onto the AES67 card. One rule, two
// callers: while the network output is on the sound goes to the card the daemon
// reads, choosing that card's ALSA id out of what the box actually has, and
// otherwise the configured device stands.
//
// "out" is the card's id when one was chosen from the list. An empty list means
// nothing has been enumerated yet — a test, or a box where ALSA refused — and
// the card is then named directly rather than left unset, because a device that
// cannot be found is a device that can be found on the next pass.
inline std::string aes67_pick_alsa_device(
        const std::string& configured, const std::string& card,
        const std::vector<std::string>& alsa_ids, bool* out_found = nullptr) {
    if (card.empty()) {
        if (out_found) *out_found = false;
        return configured;
    }
    const int idx = aes67_find_device(alsa_ids, card);
    if (out_found) *out_found = idx >= 0 || alsa_ids.empty();
    if (idx >= 0) return alsa_ids[(size_t)idx];
    // Not enumerated. Naming the card is still the right device, and the next
    // enumeration — the one a page reload does — will replace it with whichever
    // id actually works.
    return "hw:CARD=" + card + ",DEV=0";
}

// The body a source is added or updated with (PUT /api/source/<id>). Every key
// the daemon parses is here on purpose: it reads them with get<>() and no
// default, so a body missing one is rejected rather than defaulted. The map is
// written out in full, as the array of channel indices it is.
inline std::string aes67_source_body(int id, int channels,
                                     const std::string& address,
                                     const std::string& name, bool enabled) {
    nlohmann::json map = nlohmann::json::array();
    for (int c = 0; c < channels; ++c) map.push_back(c);

    const nlohmann::json j{
        {"id", id},
        {"enabled", enabled},
        {"name", name},
        {"io", "Audio Device"},
        {"max_samples_per_packet", kAes67SamplesPerPacket},
        {"codec", kAes67Codec},
        {"address", address},
        {"ttl", kAes67Ttl},
        {"payload_type", kAes67PayloadType},
        {"dscp", kAes67Dscp},
        {"refclk_ptp_traceable", false},
        {"map", map},
    };
    return j.dump();
}

// What the address falls back to when the box has not been given one: a
// documentation-range multicast group, and the one the daemon's own sample
// configuration uses, so a box that has never been edited behaves as the
// project intends rather than inventing something.
inline std::string aes67_default_address() { return "239.1.0.1"; }

inline std::string aes67_address_or_default(const std::string& configured) {
    return configured.empty() ? aes67_default_address() : configured;
}

// ── Reading back what the daemon says ────────────────────────────────────────
// The daemon answers GET /api/streams with {"sources":[...],"sinks":[...]} and
// GET /api/sources with {"sources":[...]}. Both parse here. A body that will
// not parse leaves the vector empty and answers false, because "the daemon said
// something we do not understand" is a state to be reported — not an exception
// thrown at whoever happens to be reading the interface.
inline bool aes67_parse_sources(const std::string& json_text,
                                std::vector<Aes67Source>& out) {
    out.clear();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;

    const auto it = j.find("sources");
    if (it == j.end() || !it->is_array()) return false;

    for (const auto& s : *it) {
        if (!s.is_object()) continue;
        Aes67Source src;
        src.id           = aes67_detail::num(s, "id", -1);
        src.enabled      = aes67_detail::flag(s, "enabled", false);
        src.name         = aes67_detail::str(s, "name");
        src.address      = aes67_detail::str(s, "address");
        src.codec        = aes67_detail::str(s, "codec");
        src.io           = aes67_detail::str(s, "io");
        src.payload_type = aes67_detail::num(s, "payload_type", 0);
        // The map is the channel list, so its length IS the stream's width.
        const auto m = s.find("map");
        if (m != s.end() && m->is_array()) src.channels = (int)m->size();
        out.push_back(std::move(src));
    }
    return true;
}

// ── The SDP, which is what is actually on the wire ───────────────────────────
// The source list says what was asked for; the SDP says what is being sent —
// address, port, codec, width, and the clock it is referenced to. It is also
// what a console's engineer will ask to see, so it is parsed and shown rather
// than merely stored.
struct Aes67Sdp {
    std::string address;
    int         port = 0;
    std::string codec;        // "L24"
    int         sample_rate = 0;
    int         channels = 0;
    bool        ptp_referenced = false;   // an a=ts-refclk:ptp= line is present
    // Only true when both an address and a port were found: those two are the
    // difference between a stream and a configuration of one.
    bool        valid = false;
};

// A line at a time, on purpose: SDP is a line-oriented text format, and a
// regular expression would be harder to read than the thing it matches.
inline Aes67Sdp aes67_parse_sdp(const std::string& text) {
    Aes67Sdp out;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        std::string line = text.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // c=IN IP4 <address>[/<ttl>] — taken last, because a media-level line
        // overrides the session-level one when both are present.
        if (line.rfind("c=IN IP4 ", 0) == 0) {
            std::string a = line.substr(9);
            const size_t slash = a.find('/');
            if (slash != std::string::npos) a = a.substr(0, slash);
            if (!a.empty()) out.address = a;
        // m=audio <port> RTP/AVP <payload>
        } else if (line.rfind("m=audio ", 0) == 0) {
            std::string rest = line.substr(8);
            const size_t sp = rest.find(' ');
            if (sp != std::string::npos) rest = rest.substr(0, sp);
            try { out.port = std::stoi(rest); } catch (...) { out.port = 0; }
        // a=rtpmap:<payload> <codec>/<rate>/<channels>
        } else if (line.rfind("a=rtpmap:", 0) == 0) {
            const size_t sp = line.find(' ');
            if (sp == std::string::npos) continue;
            const std::string fmt = line.substr(sp + 1);
            const size_t s1 = fmt.find('/');
            if (s1 == std::string::npos) continue;
            out.codec = fmt.substr(0, s1);
            const size_t s2 = fmt.find('/', s1 + 1);
            try {
                out.sample_rate = std::stoi(fmt.substr(
                    s1 + 1,
                    s2 == std::string::npos ? std::string::npos : s2 - s1 - 1));
                if (s2 != std::string::npos)
                    out.channels = std::stoi(fmt.substr(s2 + 1));
            } catch (...) {
                // A malformed rtpmap leaves those three as "not known" rather
                // than failing the whole parse: the address and port still came
                // out, and those are what the operator needs to see.
            }
        // a=ts-refclk:ptp=IEEE1588-2008:<grandmaster>:<domain> — the source is
        // referenced to a PTP clock, which is what makes it AES67 rather than
        // merely RTP, and what a receiving console synchronises to.
        } else if (line.rfind("a=ts-refclk:ptp=", 0) == 0) {
            out.ptp_referenced = true;
        }
    }

    out.valid = !out.address.empty() && out.port != 0;
    return out;
}

// The http_port the daemon's configuration names. Read rather than assumed: the
// installer makes it 8081 because the player already holds 8080, and a box that
// was given a different port must still be reachable.
inline int aes67_daemon_port_from_conf(const std::string& text) {
    const size_t at = text.find("\"http_port\"");
    if (at == std::string::npos) return 0;
    size_t i = text.find(':', at);
    if (i == std::string::npos) return 0;

    int  port = 0;
    bool any = false;
    for (++i; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            port = port * 10 + (c - '0');
            any = true;
        } else if (any) {
            break;
        } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;   // something that is not a number at all
        }
    }
    return any ? port : 0;
}

// ── Finding the card, so the player can be pointed at it ─────────────────────
// The network stream carries what the player sends to the AES67 card — there is
// one output device, not two — so when the network audio output is switched on,
// the player has to be sending the sound there and nowhere else. That means the
// device name has to be built, and the card's ALSA id read back from the kernel
// rather than written down: the driver never names its own card, so ALSA
// derives the id from the driver's shortname and truncates it to its
// fifteen-character limit, and the id is settable at module load, so a box
// could have registered it as anything at all.
//
// `/proc/asound/cards` lines look like:
//
//      3 [RAVENNA        ]: MergingRavennaALSA - Merging RAVENNA
//
// The id is matched against the whole line rather than the bracket alone,
// because the id is only one of the places the card's name appears — and the
// bracketed field is space-padded, which is not part of the id.
//
// An empty answer means no card matched, and the caller must treat that as
// "cannot be done": pointing the player at a device that does not exist would
// silence the room for a stream that could never have worked.
// The bracketed field of one /proc/asound/cards line, trimmed. Empty when the
// line has no card in it.
inline std::string aes67_bracket_id(const std::string& line) {
    const size_t open = line.find('[');
    if (open == std::string::npos) return {};
    const size_t close = line.find(']', open);
    if (close == std::string::npos) return {};

    std::string id = line.substr(open + 1, close - open - 1);
    const size_t first = id.find_first_not_of(" \t");
    if (first == std::string::npos) return {};      // empty brackets
    id.erase(0, first);
    while (!id.empty() && (id.back() == ' ' || id.back() == '\t'))
        id.pop_back();
    return id;
}

// Every card id the kernel lists, in the order it lists them.
inline std::vector<std::string> aes67_card_ids(const std::string& cards_text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= cards_text.size()) {
        const size_t eol = cards_text.find('\n', pos);
        const std::string line = cards_text.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? cards_text.size() + 1 : eol + 1;

        const std::string id = aes67_bracket_id(line);
        if (!id.empty()) out.push_back(id);
    }
    return out;
}

// Is a card of exactly this name registered? Whole-id comparison, so a second
// card whose id merely contains the first ("RAVENNA2") is not mistaken for it.
inline bool aes67_card_present(const std::string& cards_text,
                               const std::string& name) {
    if (name.empty()) return false;
    for (const auto& id : aes67_card_ids(cards_text))
        if (id == name) return true;
    return false;
}

inline std::string aes67_card_id_from_cards(const std::string& cards_text,
                                            const std::string& name) {
    if (name.empty()) return {};

    size_t pos = 0;
    while (pos <= cards_text.size()) {
        const size_t eol = cards_text.find('\n', pos);
        const std::string line = cards_text.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? cards_text.size() + 1 : eol + 1;

        const std::string id = aes67_bracket_id(line);
        if (id.empty()) continue;

        // The bracketed id first — that IS the card's name. Only then the rest
        // of the line, which is where a driver's own long name lives (the
        // kernel's id is a truncation of it, so the id can be absent and the
        // name still present).
        if (id == name || line.find(name) != std::string::npos) return id;
    }
    return {};
}

// ── What the player can see about the daemon ─────────────────────────────────
// One pass's worth of answers, gathered so the interface can say what is true
// rather than what should be true. Every field is something an operator can act
// on, and each is separately knowable: the daemon can be installed and not
// running, running and not locked to a clock, locked with the source switched
// off, or switched on with the player writing to the wrong device — four
// different faults that all sound like silence.
struct Aes67State {
    bool installed = false;        // the daemon is installed on this box
    bool service_active = false;   // ...and running
    bool rest_reachable = false;   // ...and answering on its HTTP port
    int  port = 0;                 // the port it was asked on

    bool ptp_known = false;        // its clock state could be read
    bool ptp_locked = false;       // the clock is locked, so audio can flow
    std::string ptp_status_word;   // as the daemon reported it
    std::string ptp_gmid;          // the grandmaster it locked to
    double      ptp_jitter = 0.0;  // its own reported offset

    bool sources_known = false;    // the source list could be read
    bool source_present = false;   // our source exists
    bool source_enabled = false;   // ...and is switched on
    bool source_correct = false;   // ...and is the shape this player means
    int  source_channels = 0;
    std::string source_address;    // what it says it sends to
    std::string source_name;

    bool sdp_valid = false;        // the daemon published an SDP for it
    int  sdp_port = 0;
    std::string sdp_codec;
    int  sdp_channels = 0;
    bool sdp_ptp = false;
    std::string sdp_text;          // verbatim, for an engineer to read

    bool card_present = false;     // the card the daemon reads is registered
    bool player_on_card = false;   // ...and the player is writing to it

    // A sentence, when something could not be found out at all — as opposed to
    // found out to be wrong, which the fields above already say.
    std::string error;

    // Whether the sound can actually be expected to leave the box: everything
    // that has to be true, in one answer, so the interface does not have to
    // re-derive the conclusion in a second place and get it differently.
    bool carrying_audio() const {
        return service_active && rest_reachable && ptp_locked &&
               source_present && source_enabled && card_present && player_on_card;
    }
};

// ── Keeping a stream up ──────────────────────────────────────────────────────
// The switch in the interface is a one-shot: it starts the daemon, writes the
// source and points the player at the card, once, and whatever happens next is
// whatever happens next. That is not enough for an appliance. A card that was
// not registered yet, a daemon that came up a moment too late, a source that an
// engineer edited from Merging's own page — each of them leaves a box that is
// switched on and silent, and the only repair on offer was to go back to the
// settings page and press Apply again.
//
// So the box watches its own stream. What it must *not* do is decide on its own
// to put something back on air: a stream that was switched off stays off until a
// person switches it on. Those two are the whole of the design below, and they
// are here rather than in the loop that uses them so they can be checked without
// a daemon, a Pi or a card.
enum class Aes67Action {
    None,          // nothing to do, and nothing wrong
    StartService,  // the daemon should be running and enabled but is not
    EnsureSource,  // our source is missing, or is not the shape we publish
    RepointCard,   // the sound is going somewhere other than the AES67 card
    WaitForClock,  // nothing can be fixed from here: the clock is not locked
};

// The one decision. `manage` is the operator's switch, `source_correct` is
// Aes67Source::matches_shape against the width and address this box publishes.
//
// Read in order, it says:
//   1. Not managing, or not installed: nothing to keep up, and nothing to say.
//   2. Managing but the daemon is not running: start it (idempotently).
//   3. Running but not answering on its port: no conclusion can be drawn from
//      silence, so wait rather than start repairing what may be fine.
//   4. Clock not locked: audio cannot flow yet, and nothing here can lock it.
//      Waiting is the correct action, not an error.
//   5. No source at all, while managing: a box that is switched on should have
//      one, even between services — that is what receivers subscribe to.
//   6. The source is there and switched OFF: leave it alone. This is the line
//      that keeps a deliberate stop from being undone by a repair loop.
//   7. There and switched on but the wrong shape, or the sound is going
//      elsewhere: put it right. Both are what "switched on and silent" looks
//      like from here.
inline Aes67Action aes67_converge_action(const Aes67State& s, bool manage,
                                         bool source_correct) {
    if (!manage || !s.installed) return Aes67Action::None;
    if (!s.service_active)       return Aes67Action::StartService;
    if (!s.rest_reachable)       return Aes67Action::None;
    if (!s.ptp_known || !s.ptp_locked) return Aes67Action::WaitForClock;
    if (!s.source_present)       return Aes67Action::EnsureSource;
    if (!s.source_enabled)       return Aes67Action::None;
    if (!source_correct)         return Aes67Action::EnsureSource;
    if (s.card_present && !s.player_on_card) return Aes67Action::RepointCard;
    return Aes67Action::None;
}

// The same decision in words, for the log and the interface. "None" is
// deliberately the empty string: a tick that did nothing has nothing to say.
const char* aes67_action_word(Aes67Action a);

// Everything above, in one pass. `want_channels` and `want_address` are what
// THIS player means to publish, so the answer can say whether the source that
// exists is the source that was intended.
// `player_web_port` is the port the player's OWN interface is on. It is here
// only so the one failure that looks like a daemon fault can be named for what
// it is: a daemon configured on that same port starts, binds nothing, reports
// itself active and answers every request with silence. Told just the port, an
// operator has no way to tell that apart from a broken daemon.
Aes67State aes67_probe(const std::string& alsa_device, int want_channels,
                       const std::string& want_address,
                       int player_web_port = 0);

// Create our source, or correct it if it exists with the wrong shape. Never
// touches whether it is switched on: that is the operator's, and repairing a
// configuration must not quietly put a stream back on air. Empty on success.
std::string aes67_ensure_source(int channels, const std::string& address,
                                const std::string& name, bool enabled);

// Switch the stream on or off, leaving every other part of it as it is. The
// daemon's PUT replaces the whole source, so the existing one is read first and
// only this field is changed. Empty on success.
std::string aes67_set_source_enabled(bool enabled);

// Start or stop the daemon, and set whether it starts at boot. Empty on success.
std::string aes67_set_service(bool start, bool enable);

// The port the daemon is on, read from its configuration on this box.
int aes67_daemon_port();

// The device string the player has to be using for the sound to reach the
// network: `plughw:CARD=<id>`, built from the card this box actually has. Empty
// when that card is not registered — which the caller must treat as "this
// cannot be done", never as "carry on with whatever device is selected".
std::string aes67_card_device(const std::string& name);

} // namespace multisite_player
