// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// caption_bridge.h — captions onto the wire, without a caption track.
//
// THE ROUTE, and why it is this one.
//
// libobs already splices CEA-708 into the encoded video bitstream as an SEI
// message: `add_caption()` in obs-output.c runs inside `send_interleaved()`,
// which is the same path that hands packets to our own output's
// `encoded_packet` callback. So a caption submitted to OUR output object is
// inside the H.264/HEVC/AV1 packets BEFORE we ever see them.
//
// That is worth stating plainly, because it is what makes this cheap: the CMAF
// muxer stores encoded bytes and never inspects them, the manifest describes
// video and audio, the satellite's decoder hands the same bytes to FFmpeg, and
// the relay remuxes with `-c copy`. Every one of those carries SEI without
// knowing it exists. A separate text track would have needed new work in all
// five. This needs none.
//
// WHAT THIS CLASS IS FOR. `obs_output_output_caption_text2` has to be called on
// the output that is encoding, and nothing calls it on ours. LocalVocal's
// "Stream Captions" is hardwired to `obs_frontend_get_streaming_output()` with
// no picker, so it can never reach us — and when OBS is not also streaming,
// that returns null and the captions are dropped with no error at all.
//
// So the bridge takes captions from a SOURCE instead, which is the one
// integration point every caption generator has in common:
//
//   * A text source's text. LocalVocal's `send_caption_to_source()` sets the
//     "text" setting on a source the operator picks; so do the Google,
//     Deepgram, ElevenLabs and Speechmatics captioners. Watching a text source
//     therefore works with all of them and depends on none of their internals.
//   * A real CEA-708 producer, via `obs_source_add_caption_callback`. Free to
//     support, and the right thing if a capture card ever brings captions in.
//
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct obs_output;
typedef struct obs_output obs_output_t;
struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_source_cea_708;

namespace multisite_obs {

class CaptionBridge {
public:
    ~CaptionBridge();

    // `source_name` is the text source to watch; empty means captions are off
    // and nothing is started. Safe to call when the output has no captions
    // configured, which is the default.
    void start(obs_output_t* output, const std::string& source_name);
    void stop();

    bool running() const { return m_running.load(); }
    // How many captions have gone onto the wire this broadcast, for the dock.
    unsigned long long sent() const { return m_sent.load(); }

private:
    void watch_loop();
    void send(const std::string& text);
    static void on_cea708(void* param, obs_source_t* source,
                          const struct obs_source_cea_708* captions);

    obs_output_t*     m_output = nullptr;
    std::string       m_source_name;
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<unsigned long long> m_sent{0};
    // The last text put on the wire, so unchanged text is not sent again —
    // a text source holds its value between utterances and polling it would
    // otherwise repeat the same caption several times a second.
    std::mutex   m_last_mtx;
    std::string  m_last;
    // Said once per broadcast, not once per sentence: that captions are being
    // split is worth knowing and is not worth a line every time somebody
    // speaks.
    std::atomic<bool> m_said_split{false};
};

} // namespace multisite_obs
