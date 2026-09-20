// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// caption_bridge.h — captions that are already in the feed, put on the wire.
//
// WHAT CARRIES THEM. Not a caption track: CEA-708 inside the video bitstream as
// an SEI message. libobs already does that part — `add_caption()` in
// obs-output.c runs inside `send_interleaved()`, the same path that hands
// packets to our output's `encoded_packet` callback, so a caption given to our
// output is inside the H.264/HEVC/AV1 bytes before we ever see them.
//
// That is what makes this cheap. The muxer writes packets verbatim with no
// bitstream filter, the output copies them, the manifest describes video and
// audio and never looks inside, the satellite hands the same bytes to FFmpeg,
// and the relay remuxes with `-c copy`. Every stage carries SEI without knowing
// it exists. A caption track would have meant new work in all five.
//
// WHERE THEY COME FROM. Whatever already has them — that is the point, and it
// is why the default is Automatic. A source that carries captions announces
// them through `obs_source_output_cea708`, and this subscribes to every source
// so it does not matter which plugin is doing it. DeckLink does today, parsing
// CDP out of SDI VANC; anything else may tomorrow, and nothing here needs to
// know about it.
//
// Those captions are forwarded as DATA, byte for byte, via `obs_output_caption`
// — never turned into text and re-encoded. Whatever the upstream encoder made
// is what goes out.
//
// THE TEXT FALLBACK, and why it is not automatic. A captioning plugin that
// cannot emit CEA-708 — LocalVocal, and the Google, Deepgram, ElevenLabs and
// Speechmatics captioners — writes its words into a text source instead. Naming
// that source explicitly switches this on for it. It is deliberately NOT part
// of Automatic: a scene is full of text sources that are not captions, and
// captioning a lower third or a countdown clock would be worse than captioning
// nothing.
//
// A NOTE ON WHAT OBS WILL CARRY. The wire format is a CEA-708 wrapper around a
// CEA-608 payload. That is what broadcast and YouTube mean by closed captions,
// but it is not DTVCC 708: `add_caption()` discards every packet whose cc_type
// is not 0, keeping 608 field 1 alone. An SDI feed carrying true 708 services
// loses them, and that is OBS's behaviour rather than ours.
//
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct obs_output;
typedef struct obs_output obs_output_t;
struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_weak_source;
typedef struct obs_weak_source obs_weak_source_t;
struct obs_source_cea_708;
struct calldata;
typedef struct calldata calldata_t;

namespace multisite_obs {

// The stored value of the encoder's caption setting when it means "listen to
// everything". Anything else is a source name, and empty is off.
inline const char* kCaptionAuto() { return "\x01auto"; }

class CaptionBridge {
public:
    ~CaptionBridge();

    // `setting` is kCaptionAuto(), a source name, or empty for no captions.
    void start(obs_output_t* output, const std::string& setting);
    void stop();

    bool running() const { return m_running.load(); }
    unsigned long long sent() const { return m_sent.load(); }

private:
    void subscribe(obs_source_t* src);
    void unsubscribe_all();
    void watch_text_loop();
    void send_text(const std::string& text);

    static void on_cea708(void* param, obs_source_t* source,
                          const struct obs_source_cea_708* captions);
    // A source that appears mid-broadcast — a card re-plugged, a scene
    // collection loading late — has to be picked up too, or Automatic would
    // only ever mean "whatever existed at Go Live".
    static void on_source_created(void* param, calldata_t* cd);

    // ATOMIC, and cleared before the callbacks are torn down. It is written by
    // stop() on the UI thread and read by the OBS graphics thread inside
    // on_cea708; a plain pointer there is a check-then-use race that hands a
    // caption to a freed output. Same family as the QPointer receivers fixed in
    // the docks, and the same afternoon.
    std::atomic<obs_output_t*> m_output{nullptr};
    std::atomic<bool>          m_automatic{false};
    std::string       m_text_source;         // empty unless one was named
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<unsigned long long> m_sent{0};
    std::atomic<bool> m_said_embedded{false};
    std::atomic<bool> m_said_split{false};

    // Weak references, because a source can be destroyed while we hold one and
    // removing a callback from freed memory is a crash in the middle of an
    // event. Weak refs answer "is it still there?" safely.
    std::mutex                       m_subs_mtx;
    std::vector<obs_weak_source_t*>  m_subs;

    std::mutex   m_last_mtx;
    std::string  m_last;
};

} // namespace multisite_obs
