// SPDX-License-Identifier: GPL-3.0-or-later
//
// alsa_output.cpp — production audio out of the box.
//
// The appliance carries whatever the feed carries, which is more than a stereo
// listener mix — a campus's own console or router decides what those channels
// mean, and this program's job is to transport them. So
// this opens the device with the channel count the FEED carries and holds onto
// as many of the feed's channels as the card will accept.
//
// The device is an ordinary ALSA one — HDMI on the low-cost tier, a de-embedder
// at the campus recovering its eight channels of LPCM. Two things about how it
// is opened come from cards that are not ordinary: a card may take only
// integer samples, which is what `pcm_convert.h` is for, and it may grant a far
// smaller buffer than was asked for, which `open()` reports rather than hides.
//
#include "audio_output.h"
#include "log.h"
#include "sysinfo.h"   // to report why sound broke up, rather than guess
#include "pcm_convert.h"
#include "idle_keepalive.h"   // the cushion, and the buffer that holds it

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_player {

namespace {

// Monotonic nanoseconds, for the one question the keep-alive thread asks: has
// the thread that delivers audio written anything lately?
uint64_t steady_ns() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
}

class AlsaOutput : public AudioOutput {
public:
    ~AlsaOutput() override { close(); }

    bool open(const Config& cfg, int sample_rate, int channels,
              std::string& error) override;
    void close() override;
    bool ok() const override { return m_pcm != nullptr; }

    std::string description() const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_description;
    }

    void write(const multisite::DecodedAudioFrame& frame) override;
    double delay_s() const override;
    void flush() override;
    std::vector<AudioDevice> devices() const override;

private:
    bool recover(int err);
    // Hands `frames` of already-formatted samples to the card, recovering from
    // an under-run without losing the rest of the buffer.
    bool write_frames(const uint8_t* data, snd_pcm_uframes_t frames);

    // Keeps the card fed while nothing is playing. An ALSA playback device that
    // is not written to runs dry, and a dry device is not merely silent: it
    // stops producing samples, so whatever is reading it — on this box, the
    // AES67 daemon — has nothing to hand on, and the receivers downstream
    // declare the source offline. Nothing here is audible; it is the difference
    // between a stream that exists and one that has been switched off as far as
    // the rest of the network is concerned.
    void keep_fed();

    mutable std::mutex m_mtx;
    snd_pcm_t*  m_pcm = nullptr;
    int         m_rate = 48000;
    int         m_channels = 2;
    // Which of float/S32/S16 the card agreed to. Float is the decoder's own
    // layout and costs nothing; the others are converted on the way out.
    PcmOutFormat m_format = PcmOutFormat::Float32;
    std::string m_description = "no audio output";
    // Rate-limit the complaint: a card that keeps under-running must not fill
    // the log faster than it fills its buffer.
    long long   m_xruns = 0;
    long long   m_logged_xruns = 0;
    // Channels the feed carries, when the device would not take them all.
    int         m_source_channels = 0;
    // Scratch for the converted samples, kept between writes so the thread that
    // presents the picture is not also reallocating a buffer every frame.
    std::vector<uint8_t> m_bytes;

    // ── Keeping the card fed while it is idle (see keep_fed()) ───────────────
    // What the card granted, in frames, so the top-up knows how much is a
    // period's worth and how much the buffer holds in total.
    snd_pcm_uframes_t m_period_frames = 0;
    snd_pcm_uframes_t m_buffer_frames = 0;
    // How much audio to keep queued while nothing is playing, decided once when
    // the card is opened (idle_cushion_frames) and used by both the buffer sized
    // below and the top-up that writes from it. It used to be recomputed in
    // keep_fed() while the buffer was sized from the period, and the two
    // disagreeing on a 1 ms-period card is what read past the end of the heap.
    snd_pcm_uframes_t m_idle_cushion = 0;
    // The cushion's worth of silence in the card's own format, built once when
    // the card is opened. Silence is zero bytes in every one of them, which is
    // why this does not need to know which format was agreed — but it does have
    // to be as long as the largest write keep_fed() will make from it.
    std::vector<uint8_t> m_silence;
    std::thread m_idle;
    std::atomic<bool> m_idle_stop{false};
    // When the delivery thread last wrote real audio. The idle announcement is
    // keyed on this rather than on the buffer level, because the level dips
    // routinely during normal playback and saying "idle" then would be wrong.
    std::atomic<uint64_t> m_last_write_ns{0};
};

bool AlsaOutput::open(const Config& cfg, int sample_rate, int channels,
                      std::string& error) {
    close();
    std::lock_guard<std::mutex> lk(m_mtx);
    error.clear();

    const std::string device = cfg.alsa_device.empty() ? "default"
                                                       : cfg.alsa_device;
    int rc = snd_pcm_open(&m_pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        error = "cannot open " + device + ": " + snd_strerror(rc);
        m_pcm = nullptr;
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(m_pcm, hw);
    snd_pcm_hw_params_set_access(m_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);

    // Ask for what the decoder produces, then for signed integers if the card
    // will not have it. Float arrives with no conversion at all, which is why
    // it is first; a card that takes only integers would otherwise end the open
    // here with a message an operator could do nothing about. test_format() is
    // used rather than set_format() so a refusal cannot disturb the parameters
    // the retry is built on.
    static const struct { snd_pcm_format_t alsa; PcmOutFormat ours; } kFormats[] = {
        { SND_PCM_FORMAT_FLOAT_LE, PcmOutFormat::Float32 },
        { SND_PCM_FORMAT_S32_LE,   PcmOutFormat::S32     },
        { SND_PCM_FORMAT_S16_LE,   PcmOutFormat::S16     },
    };
    bool format_ok = false;
    for (const auto& cand : kFormats) {
        if (snd_pcm_hw_params_test_format(m_pcm, hw, cand.alsa) == 0) {
            snd_pcm_hw_params_set_format(m_pcm, hw, cand.alsa);
            m_format = cand.ours;
            format_ok = true;
            break;
        }
    }
    if (!format_ok) {
        // Name what the card does offer. Being told the card takes none of
        // float, S32 or S16 is only useful if the next line says what it wants
        // instead, and that is one call away.
        snd_pcm_format_mask_t* mask = nullptr;
        snd_pcm_format_mask_alloca(&mask);
        snd_pcm_hw_params_get_format_mask(hw, mask);
        std::string offers;
        for (int f = 0; f <= SND_PCM_FORMAT_LAST; ++f) {
            if (snd_pcm_format_mask_test(mask, (snd_pcm_format_t)f)) {
                if (!offers.empty()) offers += ", ";
                offers += snd_pcm_format_name((snd_pcm_format_t)f);
            }
        }
        error = device + " takes none of the formats this player can send "
                "(float, 32-bit or 16-bit PCM). It offers: " +
                (offers.empty() ? "nothing ALSA recognises" : offers);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }
    if (m_format != PcmOutFormat::Float32)
        plog_info("%s takes no floating-point audio — sending %s instead",
                  device.c_str(), pcm_format_name(m_format));

    m_source_channels = channels;
    unsigned want = (unsigned)std::max(1, channels);
    rc = snd_pcm_hw_params_set_channels(m_pcm, hw, want);
    if (rc < 0) {
        unsigned got = want;
        if (snd_pcm_hw_params_set_channels_near(m_pcm, hw, &got) < 0 ||
            got == 0) {
            error = device + " will not take " + std::to_string(want) +
                    " channels: " + snd_strerror(rc);
            snd_pcm_close(m_pcm);
            m_pcm = nullptr;
            return false;
        }
        // This is worth shouting about. A campus that thinks it is receiving
        // a click track and is not will only find out during an event.
        plog_error("%s will only take %u channels but the feed carries %d — "
                   "the extra channels are NOT being played. Check the output "
                   "device, or use an HDMI de-embedder that takes all eight.",
                   device.c_str(), got, channels);
        want = got;
    }
    m_channels = (int)want;

    unsigned rate = (unsigned)sample_rate;
    rc = snd_pcm_hw_params_set_rate_near(m_pcm, hw, &rate, nullptr);
    if (rc < 0) {
        error = device + " will not run at " + std::to_string(sample_rate) +
                " Hz: " + snd_strerror(rc);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }
    if ((int)rate != sample_rate)
        plog_warn("%s is running at %u Hz, not the feed's %d Hz",
                  device.c_str(), rate, sample_rate);
    m_rate = (int)rate;

    // Half a second of buffer. Generous on purpose: the delivery thread paces
    // frames against a monotonic clock, and a card whose own clock runs a few
    // parts per million away from it needs somewhere for that difference to go
    // over a two-hour event.
    //
    // The figure wanted is kept separately because set_buffer_time_near writes
    // the *achievable* value back into the variable it is given; comparing that
    // against itself later would report that every card gave exactly what was
    // asked for, on the one install where it matters.
    const unsigned buffer_us_asked = 500000;
    unsigned buffer_us = buffer_us_asked;
    snd_pcm_hw_params_set_buffer_time_near(m_pcm, hw, &buffer_us, nullptr);
    unsigned period_us = 40000;
    snd_pcm_hw_params_set_period_time_near(m_pcm, hw, &period_us, nullptr);

    rc = snd_pcm_hw_params(m_pcm, hw);
    if (rc < 0) {
        error = std::string("the sound card refused those settings: ") +
                snd_strerror(rc);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(m_pcm, sw);
    snd_pcm_uframes_t buffer_size = 0, period_size = 0;
    snd_pcm_get_params(m_pcm, &buffer_size, &period_size);

    // What the card actually granted, rather than what was asked for. A driver
    // is free to clamp both — one virtual card pins its period size to a single
    // millisecond and its period count to a handful, so the half-second
    // requested below becomes a few milliseconds and cannot be made larger.
    // Nothing said so, and the consequence is not obvious: the delivery thread
    // also presents the picture, so any stall longer than the buffer under-runs
    // the card. A box that reports "broken up" thousands of times is usually
    // being told this, not that its power supply is weak.
    {
        const unsigned rate = (unsigned)(m_rate > 0 ? m_rate : 1);
        const double granted_ms = (double)buffer_size * 1000.0 / (double)rate;
        const double period_ms  = (double)period_size  * 1000.0 / (double)rate;
        const double asked_ms   = (double)buffer_us_asked / 1000.0;
        m_description = std::string(device) + ", " +
                        std::to_string(m_channels) +
                        (m_channels == 1 ? " channel at " : " channels at ") +
                        std::to_string(m_rate) + " Hz, " +
                        pcm_format_name(m_format);
        if (granted_ms < asked_ms / 2.0) {
            plog_warn("%s gave a %.0f ms buffer, not the %.0f ms asked for "
                      "(%.0f ms of it per period, %d periods). The thread that "
                      "writes audio also presents the picture, so anything that "
                      "stalls it longer than that gaps the sound.",
                      device.c_str(), granted_ms, asked_ms, period_ms,
                      (int)(buffer_size / (period_size ? period_size : 1)));
        }
    }

    // Start once there is a period banked, so the first write does not play
    // out into a half-empty buffer and under-run immediately.
    snd_pcm_sw_params_set_start_threshold(m_pcm, sw, period_size);
    snd_pcm_sw_params_set_avail_min(m_pcm, sw, period_size);
    snd_pcm_sw_params(m_pcm, sw);

    rc = snd_pcm_prepare(m_pcm);
    if (rc < 0) {
        error = std::string("could not start the sound card: ") + snd_strerror(rc);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

    m_description += ", " + std::to_string((int)(buffer_size * 1000ULL /
                                    (unsigned)(m_rate > 0 ? m_rate : 1))) +
                     " ms buffer";
    m_xruns = m_logged_xruns = 0;
    m_bytes.clear();

    // Begin keeping the card fed. Started here rather than when the first frame
    // arrives, because the interval this covers — the box sitting idle with the
    // sound on the network — is one the first frame never arrives in.
    m_period_frames = period_size;
    m_buffer_frames = buffer_size;
    const size_t frame_bytes = (size_t)m_channels *
                               (size_t)pcm_bytes_per_sample(m_format);
    // The cushion and the buffer that holds it, from the one place that decides
    // both. Sizing the buffer from the PERIOD, as this did, is the fault this
    // header exists to prevent: the buffer has to hold the largest write
    // keep_fed() will make, and that write is the cushion. The two numbers were
    // one clamp apart on a card with a one millisecond period, and what came out
    // of the gap was heap read as full-scale float — digital noise at 0 dBFS on
    // the stream, from a function whose entire job is to be inaudible.
    m_idle_cushion = (snd_pcm_uframes_t)idle_cushion_frames(
        (uint32_t)(m_rate > 0 ? m_rate : 48000), (uint64_t)period_size,
        (uint64_t)buffer_size);
    const size_t silence_bytes = silence_buffer_bytes(
        frame_bytes, (uint32_t)(m_rate > 0 ? m_rate : 48000),
        (uint64_t)period_size, (uint64_t)buffer_size);
    if (m_idle_cushion > 0 && silence_bytes > 0) {
        m_silence.assign(silence_bytes, 0);
        // Asked once, here, rather than trusted: if the top-up ever wants more
        // than the buffer holds, that is a miss the log should carry and the
        // card should merely go quiet for — never a read past the end of the
        // heap, which is what nobody can see from the outside.
        if (silence_bytes < frame_bytes * (size_t)m_idle_cushion)
            plog_error("the silence buffer came out short (%zu bytes for %zu "
                       "frames of %zu) — the card will not be topped up while "
                       "idle, rather than risk feeding it uninitialised memory",
                       silence_bytes, (size_t)m_idle_cushion, frame_bytes);
        m_idle_stop.store(false, std::memory_order_release);
        m_idle = std::thread(&AlsaOutput::keep_fed, this);
    } else {
        // The card would not say how big its buffer is. Nothing can be topped
        // up without that, so the stream behaves as it did before: silent when
        // the picture stops. Said once, rather than left to be worked out from
        // a receiver going offline.
        plog_warn("%s did not report a usable buffer size, so the sound will "
                  "not be kept alive while the box is idle", device.c_str());
    }
    return true;
}

// Lets go of the card. Reachable from the delivery loop and from the interface
// (an operator switching audio off), so it takes m_mtx: that serialises two
// callers.
//
// The keep-alive thread is stopped and joined BEFORE the lock is taken, and it
// has to be that way round: that thread takes the same lock on every pass, so
// joining it while holding the lock would be waiting for a thread that is
// waiting for us.
void AlsaOutput::close() {
    m_idle_stop.store(true, std::memory_order_release);
    if (m_idle.joinable()) m_idle.join();

    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_pcm) {
        snd_pcm_drop(m_pcm);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
    }
    m_period_frames = 0;
    m_buffer_frames = 0;
    m_idle_cushion = 0;
    m_silence.clear();
    m_description = "no audio output";
}

bool AlsaOutput::recover(int err) {
    // An under-run means the box did not keep up — worth counting, because it
    // is the audible symptom of a machine that is thermally throttled or doing
    // too much. Recovery is silent; the count is not.
    if (err == -EPIPE) ++m_xruns;
    const int rc = snd_pcm_recover(m_pcm, err, 1 /* silent */);
    if (rc < 0) {
        plog_error("sound card stopped: %s", snd_strerror(rc));
        return false;
    }
    if (m_xruns - m_logged_xruns >= 10) {
        m_logged_xruns = m_xruns;
        // Say what the box reports rather than guessing at it. This used to
        // blame heat or power unconditionally, and the one time it fired in
        // the field it did so straight after five decoder restarts inside
        // thirteen seconds — somebody scrubbing the timeline, not a hot
        // heatsink. The Pi publishes both conditions, so ask.
        const SystemInfo sys = system_info();
        const char* why =
              (sys.under_voltage && sys.throttled)
                  ? " — the power supply is not keeping up and the board is "
                    "throttling"
            : sys.under_voltage
                  ? " — the board reports under-voltage, so suspect the power "
                    "supply or cable"
            : sys.throttled
                  ? " — the board is throttling, so suspect cooling"
            : " — the board reports neither under-voltage nor throttling, so "
              "this is not the hardware. If the card's buffer is smaller than "
              "a decoded frame (the log says so as it opens), suspect that "
              "first; otherwise it is the feed or a burst of seeking";
        plog_warn("sound has broken up %lld times%s", m_xruns, why);
    }
    return true;
}

void AlsaOutput::write(const multisite::DecodedAudioFrame& frame) {
    if (frame.frames == 0 || frame.interleaved.empty()) return;

    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm || frame.frames == 0 || frame.interleaved.empty()) return;

    // Told to the keep-alive thread, which uses it to tell "the box is idle"
    // apart from "the box is playing and the buffer happens to be low".
    m_last_write_ns.store(steady_ns(), std::memory_order_relaxed);

    // Straight through only when the card took float *and* the feed's channel
    // count already matches the device: the decoder's own buffer is then
    // exactly what ALSA is waiting for, and copying every sample on the thread
    // that also presents the picture is worth avoiding.
    if (m_format == PcmOutFormat::Float32 && frame.channels == m_channels) {
        write_frames(reinterpret_cast<const uint8_t*>(frame.interleaved.data()),
                     frame.frames);
        return;
    }

    // Otherwise convert: to integers, or to the channel count the card would
    // take, or both. The policy is the one the float path already used — play
    // what the card will take rather than nothing at all, having said so in the
    // log when whole channels are being dropped.
    pcm_convert(frame.interleaved.data(), frame.frames, frame.channels,
                m_channels, m_format, m_bytes);
    const size_t frame_bytes = (size_t)m_channels *
                               (size_t)pcm_bytes_per_sample(m_format);
    if (frame_bytes > 0)
        write_frames(m_bytes.data(), m_bytes.size() / frame_bytes);
}

bool AlsaOutput::write_frames(const uint8_t* data, snd_pcm_uframes_t frames) {
    if (!m_pcm) return false;
    const size_t frame_bytes = (size_t)m_channels *
                               (size_t)pcm_bytes_per_sample(m_format);
    if (frame_bytes == 0) return false;

    snd_pcm_uframes_t remaining = frames;
    while (remaining > 0) {
        const snd_pcm_sframes_t wrote = snd_pcm_writei(m_pcm, data, remaining);
        if (wrote < 0) {
            if (!recover((int)wrote)) {
                snd_pcm_close(m_pcm);
                m_pcm = nullptr;
                return false;
            }
            continue;
        }
        data += (size_t)wrote * frame_bytes;
        remaining -= (snd_pcm_uframes_t)wrote;
    }
    return true;
}

// ── Keeping the card fed while nothing is playing ────────────────────────────
//
// The delivery thread writes audio only when there is a frame to deliver, so
// while the box is idle — stopped, held, waiting for the feed, showing the
// splash — the card is open and never written to. An ALSA playback device in
// that state drains and under-runs, and it does not merely go quiet: it stops
// producing samples. On this box that is visible, because the AES67 daemon
// reads this card and publishes what it reads. A card that has stopped
// producing gives it nothing to send, the stream stops, and every receiver
// downstream drops the source as offline. So the card is topped up with
// silence.
//
// The rule is "top up to one period", deliberately, rather than "keep the
// buffer full":
//
//   • A period is enough that the device never runs dry.
//   • It is little enough that when playback starts again at most a period of
//     silence is in front of the sound — inside what the device buffers anyway.
//   • It does not compete with real audio. While the delivery thread is feeding
//     the card the buffer stays far fuller than a period, so this fires only in
//     the gaps: it fills them rather than padding them.
//
// The write is made only when the whole top-up fits in the free space, and that
// is what keeps it from blocking. It holds the same lock the delivery thread
// does, so a blocking write here would stall the thread that presents the
// picture — the fault this whole class was rearranged to avoid once already.
void AlsaOutput::keep_fed() {
    using namespace std::chrono;

    // Sampled once: the card is closed — which joins this thread — before it is
    // ever reopened, so nothing here can change underneath.
    const snd_pcm_uframes_t period = m_period_frames;
    const snd_pcm_uframes_t buffer = m_buffer_frames;
    if (period == 0 || buffer == 0 || period > buffer) return;

    const unsigned rate = (unsigned)(m_rate > 0 ? m_rate : 48000);

    // How much audio to keep in the card's buffer while nothing is playing.
    //
    // Decided when the card was opened, by the same function that sized the
    // buffer of zeros this writes from, and deliberately NOT recomputed here.
    // The two were computed separately once, and on a card with a one
    // millisecond period they disagreed by a factor of twenty: this kept a
    // twenty-millisecond cushion while the buffer held one period of it, so
    // every top-up read past the end of its own buffer and fed the card heap
    // read as float — full-scale noise, on the stream, permanently. One number,
    // one place, is the whole fix; the explanation lives in idle_keepalive.h,
    // and tests/test_idle_keepalive.cpp is what stops it drifting back apart.
    const snd_pcm_uframes_t target = m_idle_cushion;
    if (target == 0 || target > buffer) return;

    // The most this thread may ever ask for, in frames. The buffer of zeros was
    // sized to hold exactly a cushion, so a write larger than this would read
    // past the end of it — the fault above. Checked rather than assumed: if the
    // two ever drift apart again the card goes quiet for one under-run, which is
    // audible as a gap in silence and nothing worse, instead of being fed
    // uninitialised memory.
    const size_t frame_bytes = (size_t)m_channels *
                               (size_t)pcm_bytes_per_sample(m_format);
    snd_pcm_uframes_t max_write = target;
    if (frame_bytes > 0) {
        const snd_pcm_uframes_t holds =
            (snd_pcm_uframes_t)(m_silence.size() / frame_bytes);
        if (holds < max_write) max_write = holds;
    }

    // A quarter of the cushion between checks, so four of them can be missed
    // before the card runs dry.
    const long long sleep_us = idle_sleep_us((uint64_t)target, rate);

    // So the idle state is announced once each time it actually happens, rather
    // than once per top-up. A box playing normally also tops the card up
    // occasionally — the delivery thread keeps only about one frame ahead of
    // the device — and saying so every few seconds is noise in a log an
    // operator has to read.
    bool announced = false;

    while (!m_idle_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(microseconds(sleep_us));
        if (m_idle_stop.load(std::memory_order_acquire)) break;

        // Idle means the delivery thread has not written anything for a while.
        // That, not the buffer level, is what the operator cares about: the
        // level dips routinely during normal playback.
        const bool idle =
            steady_ns() - m_last_write_ns.load(std::memory_order_relaxed) >
            1000000000ULL;
        if (!idle) announced = false;

        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_pcm || m_silence.empty()) continue;

        const snd_pcm_sframes_t avail = snd_pcm_avail_update(m_pcm);
        // A negative answer is the device saying something is wrong with it: an
        // under-run, or a card that has gone away. Recovering that is the
        // delivery thread's job on the next real frame, and there is nothing
        // useful to do from here — forcing it would fight that recovery.
        if (avail <= 0) continue;

        const snd_pcm_sframes_t queued = (snd_pcm_sframes_t)buffer - avail;
        // Cushion still in hand: the delivery thread is feeding it and there is
        // nothing to add.
        if (queued >= (snd_pcm_sframes_t)target) continue;

        const snd_pcm_uframes_t want =
            (snd_pcm_uframes_t)((snd_pcm_sframes_t)target - queued);
        if (want == 0 || want > (snd_pcm_uframes_t)avail) continue;
        // Never more than the buffer of zeros actually holds. A cushion is the
        // largest write this thread makes, so this can only bite if the two have
        // drifted apart — and if they have, skipping the top-up is a gap in
        // silence rather than the noise that reading past the buffer produces.
        if (want > max_write) continue;

        if (write_frames(m_silence.data(), want) && idle && !announced) {
            announced = true;
            plog_info("sound card idle — writing silence so the output keeps "
                      "running and anything reading it stays in step");
        }
    }
}

double AlsaOutput::delay_s() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return 0.0;
    snd_pcm_sframes_t frames = 0;
    if (snd_pcm_delay(m_pcm, &frames) < 0 || frames < 0) return 0.0;
    return (double)frames / (double)m_rate;
}

void AlsaOutput::flush() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return;
    // After a jump, whatever is buffered belongs to where playback used to be.
    snd_pcm_drop(m_pcm);
    snd_pcm_prepare(m_pcm);
}

std::vector<AudioDevice> AlsaOutput::devices() const {
    std::vector<AudioDevice> out;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) != 0) return out;

    for (void** h = hints; h && *h; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* io   = snd_device_name_get_hint(*h, "IOID");

        // Playback devices only, and not the dozens of plugin aliases: an
        // operator picking an output should see the sockets on the box, not
        // ALSA's internal plumbing.
        const bool playback = !io || std::strcmp(io, "Output") == 0;
        const std::string id = name ? name : "";
        const bool interesting =
            playback && !id.empty() &&
            (id == "default" || id.rfind("hw:", 0) == 0 ||
             id.rfind("plughw:", 0) == 0 || id.rfind("sysdefault:", 0) == 0);

        if (interesting) {
            AudioDevice d;
            d.id = id;
            std::string text = desc ? desc : id;
            // Hints put the human name on a second line; join it up.
            std::replace(text.begin(), text.end(), '\n', ' ');
            d.description = text;
            out.push_back(std::move(d));
        }
        if (name) ::free(name);
        if (desc) ::free(desc);
        if (io)   ::free(io);
    }
    snd_device_name_free_hint(hints);
    return out;
}

} // namespace

AudioOutput* make_alsa_output() { return new AlsaOutput(); }

} // namespace multisite_player
