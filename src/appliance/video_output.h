// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// video_output.h — where the picture goes.
//
// The appliance drives its display directly rather than through a desktop, so
// this is the whole of the video back end: set a mode, then hand it frames.
// Two implementations:
//
//   DrmOutput   — Linux KMS. Owns the HDMI connector, sets the output
//                 resolution and frame rate itself, and page-flips.
//   NullOutput  — accepts frames and discards them. This is what lets the
//                 engine and the whole web UI be built and exercised on a
//                 development machine that has no KMS at all.
//
// Frames arrive as I420 from the decoder. A splash or a holding slide arrives
// as packed 8-bit BGRX, because that is what a text renderer naturally
// produces and what every KMS driver can scan out.
//
#include "config.h"
#include "../core/cmaf_decoder.h"
#include "../core/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace multisite_player {

// One mode a connected display will accept. Refresh is in millihertz so
// 59.94 Hz is not silently rounded to 60 — a campus feeding a broadcast chain
// cares about the difference.
struct OutputMode {
    int  width = 0;
    int  height = 0;
    int  refresh_mhz = 0;
    bool preferred = false;         // the display's own first choice
};

// A DecodedVideoFrame carries plane pointers into its own buffer, so any copy
// leaves them pointing at the original. Anything that has copied a frame must
// put them back before handing it to an output.
inline void fix_planes(multisite::DecodedVideoFrame& f) {
    uint8_t* base = f.data.data();
    size_t off = 0;
    for (int i = 0; i < 3; ++i) {
        f.plane[i] = base + off;
        off += (size_t)f.stride[i] * (size_t)(i == 0 ? f.height : (f.height + 1) / 2);
    }
}

// One screen of several, as /api/status reports it (obs-multisite#29).
struct OutputHeadStatus {
    std::string connector;
    int         tile = -1;
    bool        active = false;   // plugged in and being driven
    std::string mode;             // "1920x1080 @ 60.00 Hz", or "" while not active
};

struct DisplayInfo {
    std::string connector;          // e.g. "HDMI-A-1"
    bool        connected = false;
    std::string monitor_name;
    std::vector<OutputMode> modes;
};

class VideoOutput {
public:
    virtual ~VideoOutput() = default;

    // Claim the display and set the configured mode. A failure here is not
    // fatal to the appliance: it keeps receiving, and the web UI says why
    // nothing is on the screen.
    virtual bool open(const Config& cfg, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool ok() const = 0;

    // Plain language for the UI, e.g. "HDMI-A-1 1920x1080 @ 50.00 Hz".
    virtual std::string description() const = 0;

    // The mode currently being scanned out.
    virtual void size(int& width, int& height) const = 0;
    virtual double refresh_hz() const = 0;

    // Decoded picture, scaled and colour-converted as the driver requires.
    virtual void present(const multisite::DecodedVideoFrame& frame) = 0;

    // A pre-rendered screen: splash, holding slide, or a still. Packed BGRX,
    // 4 bytes per pixel.
    virtual void present_bgrx(int width, int height, int stride,
                              const uint8_t* pixels) = 0;

    // Show nothing at all. Used for the black idle mode and on shutdown, so a
    // stopped box does not leave the last frame of an event on a screen in
    // an empty room.
    virtual void blank() = 0;

    // What the attached displays can do, for the resolution picker in the UI.
    // Enumerated on demand rather than cached: somebody may plug a different
    // screen in without rebooting the box.
    virtual std::vector<DisplayInfo> displays() const = 0;

    // ── Several screens from the one player (obs-multisite#29) ───────────────
    // Everything below has a default that is exactly the one-screen output, so
    // an output that drives one screen implements none of it.

    // How many screens this output drives.
    virtual int heads() const { return 1; }
    // Each screen its own tile of the one decoded frame, flipped together.
    virtual void present_tiles(const multisite::DecodedVideoFrame& frame,
                               const multisite::TileLayout&) { present(frame); }
    // One screen's mode, and a pre-rendered screen for it alone (its identity
    // screen says which output it is).
    virtual void head_size(int, int& width, int& height) const { size(width, height); }
    virtual void present_bgrx_on(int, int width, int height, int stride,
                                 const uint8_t* pixels) { present_bgrx(width, height, stride, pixels); }
    // Called every few seconds: a screen unplugged stops being driven, and one
    // plugged back in is driven again, without disturbing the others.
    virtual void recheck() {}
    virtual std::vector<OutputHeadStatus> head_status() const { return {}; }
    // How far apart the screens' page flips of one frame landed, worst and
    // most recent, in milliseconds: each HDMI output runs on its own clock.
    virtual double flip_spread_ms() const { return 0; }
    virtual double flip_spread_worst_ms() const { return 0; }
};

// Discards everything, reports itself as working. The development and
// diagnostic path — and what a box with a broken display falls back to, so a
// missing screen never stops the feed being received.
class NullVideoOutput : public VideoOutput {
public:
    bool open(const Config& cfg, std::string& error) override;
    void close() override {}
    bool ok() const override { return true; }
    std::string description() const override { return m_desc; }
    void size(int& width, int& height) const override {
        width = m_width; height = m_height;
    }
    double refresh_hz() const override { return m_fps; }
    void present(const multisite::DecodedVideoFrame&) override {}
    void present_bgrx(int, int, int, const uint8_t*) override {}
    void blank() override {}
    std::vector<DisplayInfo> displays() const override { return {}; }

private:
    std::string m_desc = "no display output";
    int m_width = 1920, m_height = 1080;
    double m_fps = 50.0;
};

// The best output this build and this machine can provide. Returns a DRM/KMS
// output where one can be opened, and the null output otherwise.
#ifdef MULTISITE_HAVE_DRM
// Returns null when no usable display could be opened, so the caller can
// fall back rather than refuse to start.
VideoOutput* make_drm_output();
#endif

VideoOutput* make_video_output();

} // namespace multisite_player
