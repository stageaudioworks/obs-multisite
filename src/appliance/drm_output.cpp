// SPDX-License-Identifier: GPL-3.0-or-later
//
// drm_output.cpp — the appliance drives its own HDMI output.
//
// There is no desktop on this box. The player claims the display through KMS,
// sets the mode itself, and page-flips between two buffers. That is what makes
// it an appliance rather than a program running on a computer: nothing else
// owns the screen, there is no compositor to negotiate with, the output
// resolution and frame rate are exactly what was asked for, and there is no
// desktop to be left in the wrong state by a volunteer with a mouse.
//
// Frames arrive as I420 at whatever size the main site encoded. They are
// scaled and converted straight into a scan-out buffer with libswscale, which
// is NEON-accelerated on ARM. Letterboxing is done here rather than by
// stretching: an event delivered in one shape and shown in another looks
// wrong in a way a congregation notices.
//
#include "video_output.h"
#include "log.h"
#include "output_heads.h"
#include "../core/tile_crop.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace multisite_player {

namespace {

std::string connector_type_name(uint32_t type) {
    switch (type) {
    case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_eDP:         return "eDP";
    case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
    case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
    case DRM_MODE_CONNECTOR_VGA:         return "VGA";
    case DRM_MODE_CONNECTOR_Composite:   return "Composite";
    case DRM_MODE_CONNECTOR_Unknown:     return "Unknown";
    default:                             return "Connector";
    }
}

std::string connector_name(const drmModeConnector* c) {
    return connector_type_name(c->connector_type) + "-" +
           std::to_string(c->connector_type_id);
}

// Refresh in millihertz from the timing, so 59.94 Hz is not reported as 60.
int refresh_mhz(const drmModeModeInfo& m) {
    if (m.htotal == 0 || m.vtotal == 0) return m.vrefresh * 1000;
    uint64_t num = (uint64_t)m.clock * 1000000ULL;
    uint64_t den = (uint64_t)m.htotal * (uint64_t)m.vtotal;
    if (m.flags & DRM_MODE_FLAG_INTERLACE) num *= 2;
    if (m.flags & DRM_MODE_FLAG_DBLSCAN)   den *= 2;
    if (m.vscan > 1) den *= m.vscan;
    return den ? (int)(num / den) : 0;
}

// One scan-out buffer. Dumb buffers are the plain, universally supported
// allocation: no GBM, no EGL, no driver-specific path to go wrong on a Pi.
struct Framebuffer {
    uint32_t handle = 0;
    uint32_t fb_id = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    uint8_t* map = nullptr;

    void destroy(int fd) {
        if (map) { ::munmap(map, size); map = nullptr; }
        if (fb_id) { drmModeRmFB(fd, fb_id); fb_id = 0; }
        if (handle) {
            drm_mode_destroy_dumb req{};
            req.handle = handle;
            drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &req);
            handle = 0;
        }
    }
};

bool create_framebuffer(int fd, int width, int height, Framebuffer& out,
                        std::string& error) {
    drm_mode_create_dumb create{};
    create.width = (uint32_t)width;
    create.height = (uint32_t)height;
    create.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        error = std::string("cannot allocate a display buffer: ") + strerror(errno);
        return false;
    }
    out.handle = create.handle;
    out.pitch  = create.pitch;
    out.size   = create.size;

    if (drmModeAddFB(fd, (uint32_t)width, (uint32_t)height, 24, 32, out.pitch,
                     out.handle, &out.fb_id) != 0) {
        error = std::string("the driver refused the display buffer: ") +
                strerror(errno);
        out.destroy(fd);
        return false;
    }

    drm_mode_map_dumb map_req{};
    map_req.handle = out.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0) {
        error = std::string("cannot map the display buffer: ") + strerror(errno);
        out.destroy(fd);
        return false;
    }
    void* addr = ::mmap(nullptr, out.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fd, (off_t)map_req.offset);
    if (addr == MAP_FAILED) {
        error = std::string("cannot map the display buffer: ") + strerror(errno);
        out.destroy(fd);
        return false;
    }
    out.map = static_cast<uint8_t*>(addr);
    std::memset(out.map, 0, (size_t)out.size);
    return true;
}

// A page flip's completion, with when it landed (the vblank it was latched
// on, in CLOCK_MONOTONIC): the screens' flips of one frame are compared.
struct FlipWait {
    bool     pending = false;
    uint64_t at_ns = 0;
};

void on_page_flip(int, unsigned, unsigned sec, unsigned usec, void* user) {
    if (auto* w = static_cast<FlipWait*>(user)) {
        w->pending = false;     // no longer pending
        w->at_ns = (uint64_t)sec * 1000000000ULL + (uint64_t)usec * 1000ULL;
    }
}

// One screen: a connector, the display controller driving it, its mode, its
// two buffers and its own scaler. The player has one of these per screen, all
// on the one DRM device (obs-multisite#29); one screen is the case that has
// always been.
struct Head {
    std::string want;              // the connector asked for; "" = the first connected
    int         tile = -1;         // which tile of a composited feed; -1 = all of it
    uint32_t    connector_id = 0;
    std::string name;              // "HDMI-A-2"
    uint32_t    crtc_id = 0;
    drmModeModeInfo mode{};
    drmModeCrtc* saved_crtc = nullptr;
    bool        active = false;    // plugged in, mode set, being driven
    Framebuffer fb[2];
    int         back = 0;
    FlipWait    flip;
    bool        first_present = true;
    bool        drawn = false;     // drawn into back() this round, so it flips
    SwsContext* sws = nullptr;
    int sws_src_w = 0, sws_src_h = 0;
    int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
    std::string description = "no display output";

    Framebuffer& back_fb() { return fb[back]; }
};

class DrmOutput : public VideoOutput {
public:
    ~DrmOutput() override { close(); }

    bool open(const Config& cfg, std::string& error) override;
    void close() override;
    bool ok() const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_fd < 0) return false;
        for (const auto& h : m_heads) if (h.active) return true;
        return false;
    }

    std::string description() const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::string d;
        for (const auto& h : m_heads) {
            if (!h.active) continue;
            if (!d.empty()) d += " + ";
            d += h.description;
        }
        if (m_heads.size() > 1) {
            for (const auto& h : m_heads)
                if (!h.active) d += (d.empty() ? "" : "; ") + h.name + " not connected";
        }
        return d.empty() ? "no display output" : d;
    }
    void size(int& width, int& height) const override { head_size(0, width, height); }
    void head_size(int i, int& width, int& height) const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (i < 0 || i >= (int)m_heads.size()) { width = 1920; height = 1080; return; }
        width = m_heads[(size_t)i].mode.hdisplay ? m_heads[(size_t)i].mode.hdisplay : 1920;
        height = m_heads[(size_t)i].mode.vdisplay ? m_heads[(size_t)i].mode.vdisplay : 1080;
    }
    double refresh_hz() const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_heads.empty() ? 0 : refresh_mhz(m_heads[0].mode) / 1000.0;
    }

    void present(const multisite::DecodedVideoFrame& frame) override;
    void present_tiles(const multisite::DecodedVideoFrame& frame,
                       const multisite::TileLayout& layout) override;
    void present_bgrx(int width, int height, int stride,
                      const uint8_t* pixels) override;
    void present_bgrx_on(int head, int width, int height, int stride,
                         const uint8_t* pixels) override;
    void blank() override;

    int heads() const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        return (int)m_heads.size();
    }
    void recheck() override;
    std::vector<OutputHeadStatus> head_status() const override;
    double flip_spread_ms() const override { return m_spread_ms.load(); }
    double flip_spread_worst_ms() const override { return m_spread_worst_ms.load(); }

    std::vector<DisplayInfo> displays() const override;

    // Opening the card is what tells us whether this build can drive a
    // display at all.
    bool probe(const Config& cfg, std::string& error) { return open(cfg, error); }

private:
    bool pick_device(const Config& cfg, std::string& error);
    bool activate(Head& h, std::string& error);
    void deactivate(Head& h, const char* why);
    void draw(Head& h, const multisite::DecodedVideoFrame& frame);
    void draw_bgrx(Head& h, int height, int stride, const uint8_t* pixels);
    void wait_for_flips();          // every screen's queued flip, bounded
    void flip_drawn();              // flip every screen drawn this round

    mutable std::mutex m_mtx;
    int      m_fd = -1;
    std::string m_card_path;
    std::vector<Head> m_heads;
    int m_want_w = 0, m_want_h = 0, m_want_fps = 0;
    bool m_round_had_many = false;   // this round flipped more than one screen
    std::atomic<double> m_spread_ms{0}, m_spread_worst_ms{0};
};

bool DrmOutput::pick_device(const Config& cfg, std::string& error) {
    std::vector<std::string> cards;
    if (!cfg.drm_card.empty()) {
        cards.push_back(cfg.drm_card);
    } else {
        // Try every card. A Pi exposes more than one DRM device (the display
        // driver and the 3D core), and only one of them has a connector.
        DIR* dir = ::opendir("/dev/dri");
        if (dir) {
            while (dirent* e = ::readdir(dir)) {
                const std::string name = e->d_name;
                if (name.rfind("card", 0) == 0)
                    cards.push_back("/dev/dri/" + name);
            }
            ::closedir(dir);
        }
        std::sort(cards.begin(), cards.end());
    }
    if (cards.empty()) {
        error = "this box has no display devices (/dev/dri is empty). "
                "On Raspberry Pi OS, check that a KMS driver is enabled in "
                "/boot/firmware/config.txt.";
        return false;
    }

    // The screens asked for: the list, or the one connector as ever.
    std::vector<OutputSpec> specs;
    if (cfg.outputs.size() >= 2) specs = cfg.outputs;
    else specs.push_back({cfg.connector, -1});

    std::string last;
    for (const auto& path : cards) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            last = path + ": " + strerror(errno);
            if (errno == EACCES)
                last += " (the player needs to be in the 'video' group)";
            continue;
        }

        drmModeRes* res = drmModeGetResources(fd);
        if (!res) { last = path + ": not a display device"; ::close(fd); continue; }

        std::vector<Head> heads;
        std::vector<uint32_t> possible;
        std::vector<int> current;
        for (const auto& spec : specs) {
            Head h;
            h.want = spec.connector;
            h.tile = spec.tile;
            uint32_t mask = 0;
            int cur = -1;
            for (int i = 0; i < res->count_connectors && !h.connector_id; ++i) {
                drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
                if (!c) continue;
                const bool wanted = spec.connector.empty()
                                        ? (c->connection == DRM_MODE_CONNECTED &&
                                           c->count_modes > 0)
                                        : (connector_name(c) == spec.connector);
                if (wanted) {
                    h.connector_id = c->connector_id;
                    h.name = connector_name(c);
                    for (int e = 0; e < c->count_encoders; ++e) {
                        if (drmModeEncoder* enc = drmModeGetEncoder(fd, c->encoders[e])) {
                            mask |= enc->possible_crtcs;
                            if (enc->encoder_id == c->encoder_id && enc->crtc_id)
                                for (int k = 0; k < res->count_crtcs; ++k)
                                    if (res->crtcs[k] == enc->crtc_id) cur = k;
                            drmModeFreeEncoder(enc);
                        }
                    }
                }
                drmModeFreeConnector(c);
            }
            heads.push_back(std::move(h));
            possible.push_back(mask);
            current.push_back(cur);
        }
        // One screen: it must be there, as ever. Several: each is looked for,
        // and one not plugged in yet is driven when it is.
        if (specs.size() == 1 && !heads[0].connector_id) {
            last = path + ": nothing is plugged into it";
            drmModeFreeResources(res);
            ::close(fd);
            continue;
        }
        bool any = false;
        for (const auto& h : heads) any = any || h.connector_id;
        if (!any) {
            last = path + ": none of the connectors asked for is on it";
            drmModeFreeResources(res);
            ::close(fd);
            continue;
        }

        const std::vector<int> crtcs = assign_crtcs(possible, current, res->count_crtcs);
        for (size_t i = 0; i < heads.size(); ++i) {
            if (crtcs[i] >= 0) heads[i].crtc_id = res->crtcs[crtcs[i]];
            else if (heads[i].connector_id)
                plog_warn("%s: no display controller is free to drive it", heads[i].name.c_str());
        }
        if (specs.size() == 1 && !heads[0].crtc_id) {
            last = path + ": no display controller is free for that output";
            drmModeFreeResources(res);
            ::close(fd);
            continue;
        }

        m_fd = fd;
        m_card_path = path;
        m_heads = std::move(heads);
        drmModeFreeResources(res);
        return true;
    }

    error = last.empty() ? "no display could be opened" : last;
    return false;
}

// Set a screen's mode and give it buffers. The mode is the one asked for, or
// the one the screen prefers.
bool DrmOutput::activate(Head& h, std::string& error) {
    if (!h.connector_id || !h.crtc_id) { error = h.name + ": not driveable"; return false; }
    drmModeConnector* c = drmModeGetConnector(m_fd, h.connector_id);
    if (!c) { error = h.name + ": the connector went away"; return false; }
    if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
        drmModeFreeConnector(c);
        error = h.name + ": nothing is plugged into it";
        return false;
    }
    const drmModeModeInfo* picked = nullptr;
    if (m_want_w > 0 && m_want_h > 0) {
        for (int i = 0; i < c->count_modes; ++i) {
            const drmModeModeInfo& m = c->modes[i];
            if (m.hdisplay != m_want_w || m.vdisplay != m_want_h) continue;
            if (m_want_fps > 0 && std::abs(refresh_mhz(m) / 1000 - m_want_fps) > 1) continue;
            picked = &m;
            break;
        }
        if (!picked)
            plog_warn("%s will not do %dx%d at %d Hz — using what it prefers instead",
                      h.name.c_str(), m_want_w, m_want_h, m_want_fps);
    }
    if (!picked)
        for (int i = 0; i < c->count_modes; ++i)
            if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { picked = &c->modes[i]; break; }
    if (!picked) picked = &c->modes[0];
    const drmModeModeInfo mode = *picked;
    drmModeFreeConnector(c);

    if (!h.saved_crtc) h.saved_crtc = drmModeGetCrtc(m_fd, h.crtc_id);
    if (h.fb[0].map == nullptr || h.mode.hdisplay != mode.hdisplay || h.mode.vdisplay != mode.vdisplay) {
        for (auto& fb : h.fb) fb.destroy(m_fd);
        for (auto& fb : h.fb)
            if (!create_framebuffer(m_fd, mode.hdisplay, mode.vdisplay, fb, error)) {
                for (auto& f : h.fb) f.destroy(m_fd);
                return false;
            }
    }
    h.mode = mode;
    if (drmModeSetCrtc(m_fd, h.crtc_id, h.fb[0].fb_id, 0, 0, &h.connector_id, 1, &h.mode) != 0) {
        error = std::string("could not set the display mode on ") + h.name + ": " + strerror(errno);
        if (errno == EACCES)
            error += ". Something else owns the screen — on Raspberry Pi OS "
                     "with a desktop installed, stop the display manager.";
        return false;
    }
    h.back = 1;
    h.first_present = true;
    h.flip.pending = false;
    h.active = true;
    char desc[160];
    std::snprintf(desc, sizeof(desc), "%s %dx%d @ %.2f Hz", h.name.c_str(), h.mode.hdisplay,
                  h.mode.vdisplay, refresh_mhz(h.mode) / 1000.0);
    h.description = desc;
    return true;
}

void DrmOutput::deactivate(Head& h, const char* why) {
    if (!h.active) return;
    h.active = false;
    h.flip.pending = false;
    h.drawn = false;
    plog_warn("%s: %s — the other screens carry on, and it is driven again when it is back",
              h.name.c_str(), why);
}

bool DrmOutput::open(const Config& cfg, std::string& error) {
    close();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_want_w = cfg.out_width;
    m_want_h = cfg.out_height;
    m_want_fps = cfg.out_fps;
    if (!pick_device(cfg, error)) return false;

    // Becoming DRM master is what lets this process set the mode. On a box
    // with no desktop it normally succeeds; if the console still holds it,
    // say so plainly rather than failing at the first page flip.
    if (drmSetMaster(m_fd) != 0 && errno != EINVAL && errno != EACCES)
        plog_debug("drmSetMaster: %s", strerror(errno));

    int active = 0;
    std::string first_error;
    for (auto& h : m_heads) {
        std::string e;
        if (activate(h, e)) {
            ++active;
            plog_info("display claimed: %s on %s%s", h.description.c_str(), m_card_path.c_str(),
                      m_heads.size() > 1 ? (" (tile " + std::to_string(h.tile) + ")").c_str() : "");
        } else {
            if (first_error.empty()) first_error = e;
            if (m_heads.size() > 1) plog_warn("%s", e.c_str());
        }
    }
    if (m_heads.size() == 1 && !active) {
        // One screen that cannot be driven is the failure it always was.
        error = first_error;
        // Undone here, under the lock this holds: nothing was set on screen.
        for (auto& h : m_heads) {
            for (auto& fb : h.fb) fb.destroy(m_fd);
            if (h.saved_crtc) drmModeFreeCrtc(h.saved_crtc);
        }
        m_heads.clear();
        drmDropMaster(m_fd);
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    if (m_heads.size() > 1)
        plog_info("%d of %zu screens driven from one decode", active, m_heads.size());
    return true;
}

void DrmOutput::close() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0) return;
    wait_for_flips();

    // Put the console back the way it was, so a stopped event does not
    // leave a black screen that looks like broken hardware.
    for (auto& h : m_heads) {
        if (h.saved_crtc) {
            drmModeSetCrtc(m_fd, h.saved_crtc->crtc_id, h.saved_crtc->buffer_id,
                           h.saved_crtc->x, h.saved_crtc->y, &h.connector_id, 1,
                           &h.saved_crtc->mode);
            drmModeFreeCrtc(h.saved_crtc);
            h.saved_crtc = nullptr;
        }
        for (auto& fb : h.fb) fb.destroy(m_fd);
        if (h.sws) { sws_freeContext(h.sws); h.sws = nullptr; }
    }
    m_heads.clear();
    drmDropMaster(m_fd);
    ::close(m_fd);
    m_fd = -1;
}

void DrmOutput::wait_for_flips() {
    if (m_fd < 0) return;
    drmEventContext ev{};
    ev.version = 2;
    ev.page_flip_handler = on_page_flip;

    // A bounded wait. If the driver never reports a flip — which happens if
    // a screen is unplugged mid-event — playback must carry on rather than
    // block the delivery thread for ever.
    const auto pending = [&] {
        for (const auto& h : m_heads) if (h.active && h.flip.pending) return true;
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (pending()) {
        const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd pfd{};
        pfd.fd = m_fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, left) <= 0) break;
        // The handler clears each screen's flag through the user pointer.
        drmHandleEvent(m_fd, &ev);
    }
    if (m_round_had_many && !pending()) {
        // How far apart the screens' flips of the last frame landed.
        uint64_t lo = UINT64_MAX, hi = 0;
        for (const auto& h : m_heads)
            if (h.active && h.flip.at_ns) { lo = std::min(lo, h.flip.at_ns); hi = std::max(hi, h.flip.at_ns); }
        if (hi >= lo && lo != UINT64_MAX) {
            const double ms = (double)(hi - lo) / 1e6;
            m_spread_ms = ms;
            if (ms > m_spread_worst_ms.load()) m_spread_worst_ms = ms;
        }
    }
    m_round_had_many = false;
    for (auto& h : m_heads) h.flip.pending = false;
}

void DrmOutput::flip_drawn() {
    int flipped = 0;
    for (auto& h : m_heads) {
        if (!h.active || !h.drawn) continue;
        h.drawn = false;
        h.flip.pending = true;
        h.flip.at_ns = 0;
        if (drmModePageFlip(m_fd, h.crtc_id, h.back_fb().fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, &h.flip) != 0) {
            h.flip.pending = false;
            // Falling back to a mode set keeps a picture on the screen on
            // drivers or states where a flip is refused — notably right after
            // a mode change, and on the very first frame.
            if (drmModeSetCrtc(m_fd, h.crtc_id, h.back_fb().fb_id, 0, 0, &h.connector_id,
                               1, &h.mode) != 0) {
                if (m_heads.size() > 1) deactivate(h, "it stopped taking pictures");
                continue;
            }
        }
        h.back ^= 1;
        ++flipped;
    }
    m_round_had_many = flipped > 1;
}

// One screen's share of a frame: scaled into its back buffer, letterboxed to
// keep its shape.
void DrmOutput::draw(Head& h, const multisite::DecodedVideoFrame& frame) {
    if (!h.active || frame.width <= 0 || frame.height <= 0) return;
    // Fit the picture inside the screen without changing its shape.
    if (frame.width != h.sws_src_w || frame.height != h.sws_src_h) {
        if (h.sws) { sws_freeContext(h.sws); h.sws = nullptr; }
        const double scale = std::min((double)h.mode.hdisplay / frame.width,
                                      (double)h.mode.vdisplay / frame.height);
        h.dst_w = std::max(2, (int)(frame.width * scale)) & ~1;
        h.dst_h = std::max(2, (int)(frame.height * scale)) & ~1;
        h.dst_x = (h.mode.hdisplay - h.dst_w) / 2;
        h.dst_y = (h.mode.vdisplay - h.dst_h) / 2;

        h.sws = sws_getContext(frame.width, frame.height, AV_PIX_FMT_YUV420P,
                               h.dst_w, h.dst_h, AV_PIX_FMT_BGRA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!h.sws) {
            plog_error("could not set up the display scaler for %s", h.name.c_str());
            h.sws_src_w = h.sws_src_h = 0;
            return;
        }
        h.sws_src_w = frame.width;
        h.sws_src_h = frame.height;
        h.first_present = true;      // the bars around it need painting again
        plog_info("showing %dx%d as %dx%d on %s's %dx%d screen", frame.width,
                  frame.height, h.dst_w, h.dst_h, h.name.c_str(), h.mode.hdisplay,
                  h.mode.vdisplay);
    }
    if (!h.sws) return;

    // With two buffers, the back one is still on screen until the flip queued
    // last time has happened, and the caller has waited for it: drawing into
    // it before then paints over the picture being scanned out (on a 30 Hz
    // output with 30 fps content the two lock in step and every frame tears
    // at the same line, seen through an HDMI capture).
    Framebuffer& fb = h.back_fb();
    if (!fb.map) return;

    // The bars only need clearing when the letterbox changes, not every frame:
    // at 1080p that is eight megabytes of pointless writes thirty times a
    // second on a box that has better things to do.
    if (h.first_present) {
        std::memset(fb.map, 0, (size_t)fb.size);
        h.first_present = false;
    }

    uint8_t* dst = fb.map + (size_t)h.dst_y * fb.pitch + (size_t)h.dst_x * 4;
    uint8_t* dst_planes[4] = { dst, nullptr, nullptr, nullptr };
    int dst_stride[4] = { (int)fb.pitch, 0, 0, 0 };
    const uint8_t* src[4] = { frame.plane[0], frame.plane[1], frame.plane[2],
                              nullptr };
    const int src_stride[4] = { frame.stride[0], frame.stride[1],
                                frame.stride[2], 0 };
    sws_scale(h.sws, src, src_stride, 0, frame.height, dst_planes, dst_stride);
    h.drawn = true;
}

void DrmOutput::draw_bgrx(Head& h, int height, int stride, const uint8_t* pixels) {
    if (!h.active) return;
    Framebuffer& fb = h.back_fb();
    if (!fb.map) return;
    std::memset(fb.map, 0, (size_t)fb.size);
    if (pixels) {
        const int rows = std::min(height, (int)h.mode.vdisplay);
        const int bytes = std::min(stride, (int)fb.pitch);
        for (int y = 0; y < rows; ++y)
            std::memcpy(fb.map + (size_t)y * fb.pitch,
                        pixels + (size_t)y * stride, (size_t)bytes);
    }
    h.first_present = true;      // a decoded frame after this repaints the bars
    h.drawn = true;
}

void DrmOutput::present(const multisite::DecodedVideoFrame& frame) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0) return;
    wait_for_flips();
    for (auto& h : m_heads) draw(h, frame);
    flip_drawn();
}

void DrmOutput::present_tiles(const multisite::DecodedVideoFrame& frame,
                              const multisite::TileLayout& layout) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0) return;
    // Every screen's previous flip first, then every screen drawn from the one
    // frame, then every flip queued back to back: the halves of one frame go
    // up together, each on its own screen's next vblank.
    wait_for_flips();
    for (auto& h : m_heads) {
        if (h.tile >= 0 && layout.is_split() && h.tile < layout.count())
            draw(h, multisite::tile_view(frame, layout, h.tile));
        else
            draw(h, frame);   // a 1x1 feed, or no tile: the whole picture
    }
    flip_drawn();
}

void DrmOutput::present_bgrx(int width, int height, int stride,
                             const uint8_t* pixels) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0 || !pixels) return;
    (void)width;
    wait_for_flips();   // not into the buffer still on screen (present)
    for (auto& h : m_heads) draw_bgrx(h, height, stride, pixels);
    flip_drawn();
}

void DrmOutput::present_bgrx_on(int head, int width, int height, int stride,
                                const uint8_t* pixels) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0 || !pixels || head < 0 || head >= (int)m_heads.size()) return;
    (void)width;
    wait_for_flips();
    draw_bgrx(m_heads[(size_t)head], height, stride, pixels);
    flip_drawn();
}

void DrmOutput::blank() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0) return;
    wait_for_flips();   // not into the buffer still on screen (present)
    for (auto& h : m_heads) draw_bgrx(h, 0, 0, nullptr);
    flip_drawn();
}

void DrmOutput::recheck() {
    std::lock_guard<std::mutex> lk(m_mtx);
    // One screen is left as it always was: its failures are the player's to
    // report. With several, each comes and goes without the others.
    if (m_fd < 0 || m_heads.size() < 2) return;
    for (auto& h : m_heads) {
        if (!h.connector_id) continue;
        drmModeConnector* c = drmModeGetConnector(m_fd, h.connector_id);
        const bool present = c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0;
        if (c) drmModeFreeConnector(c);
        if (h.active && !present) {
            deactivate(h, "unplugged");
        } else if (!h.active && present) {
            std::string e;
            if (activate(h, e)) plog_info("%s is back: %s", h.name.c_str(), h.description.c_str());
        }
    }
}

std::vector<OutputHeadStatus> DrmOutput::head_status() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<OutputHeadStatus> out;
    if (m_heads.size() < 2) return out;
    for (const auto& h : m_heads) {
        OutputHeadStatus s;
        s.connector = h.name.empty() ? h.want : h.name;
        s.tile = h.tile;
        s.active = h.active;
        if (h.active) {
            char m[64];
            std::snprintf(m, sizeof m, "%dx%d @ %.2f Hz", h.mode.hdisplay, h.mode.vdisplay,
                          refresh_mhz(h.mode) / 1000.0);
            s.mode = m;
        }
        out.push_back(s);
    }
    return out;
}

std::vector<DisplayInfo> DrmOutput::displays() const {
    std::vector<DisplayInfo> out;
    // Enumerated on demand, and from every card rather than only the one in
    // use: somebody may plug a screen into the other socket without rebooting.
    std::vector<std::string> cards;
    DIR* dir = ::opendir("/dev/dri");
    if (dir) {
        while (dirent* e = ::readdir(dir)) {
            const std::string name = e->d_name;
            if (name.rfind("card", 0) == 0) cards.push_back("/dev/dri/" + name);
        }
        ::closedir(dir);
    }
    std::sort(cards.begin(), cards.end());

    for (const auto& path : cards) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        if (!res) { ::close(fd); continue; }
        for (int i = 0; i < res->count_connectors; ++i) {
            drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c) continue;
            DisplayInfo d;
            d.connector = connector_name(c);
            d.connected = c->connection == DRM_MODE_CONNECTED;
            for (int m = 0; m < c->count_modes; ++m) {
                OutputMode mode;
                mode.width = c->modes[m].hdisplay;
                mode.height = c->modes[m].vdisplay;
                mode.refresh_mhz = refresh_mhz(c->modes[m]);
                mode.preferred = (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) != 0;
                d.modes.push_back(mode);
            }
            drmModeFreeConnector(c);
            out.push_back(std::move(d));
        }
        drmModeFreeResources(res);
        ::close(fd);
    }
    return out;
}

} // namespace

VideoOutput* make_drm_output() {
    // The caller opens it properly with the real config; this only answers
    // "can this box drive a display at all", so a machine with none falls back
    // to the null output instead of failing to start.
    return new DrmOutput();
}

} // namespace multisite_player
