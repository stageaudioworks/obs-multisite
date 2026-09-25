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

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
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

void on_page_flip(int, unsigned, unsigned, unsigned, void* user) {
    if (user) *static_cast<bool*>(user) = false;    // no longer pending
}

class DrmOutput : public VideoOutput {
public:
    ~DrmOutput() override { close(); }

    bool open(const Config& cfg, std::string& error) override;
    void close() override;
    bool ok() const override { return m_fd >= 0 && m_crtc_id != 0; }

    std::string description() const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_description;
    }
    void size(int& width, int& height) const override {
        width = m_mode.hdisplay;
        height = m_mode.vdisplay;
    }
    double refresh_hz() const override { return refresh_mhz(m_mode) / 1000.0; }

    void present(const multisite::DecodedVideoFrame& frame) override;
    void present_bgrx(int width, int height, int stride,
                      const uint8_t* pixels) override;
    void blank() override;

    std::vector<DisplayInfo> displays() const override;

    // Opening the card is what tells us whether this build can drive a
    // display at all.
    bool probe(const Config& cfg, std::string& error) { return open(cfg, error); }

private:
    bool pick_device(const Config& cfg, std::string& error);
    bool flip();                      // present the back buffer, then swap
    void wait_for_flip();
    Framebuffer& back() { return m_fb[m_back]; }

    mutable std::mutex m_mtx;
    int      m_fd = -1;
    uint32_t m_connector_id = 0;
    uint32_t m_crtc_id = 0;
    drmModeModeInfo m_mode{};
    drmModeCrtc* m_saved_crtc = nullptr;
    std::string m_card_path;
    std::string m_description = "no display output";

    Framebuffer m_fb[2];
    int  m_back = 0;
    bool m_flip_pending = false;
    bool m_first_present = true;

    SwsContext* m_sws = nullptr;
    int m_sws_src_w = 0, m_sws_src_h = 0;
    int m_dst_x = 0, m_dst_y = 0, m_dst_w = 0, m_dst_h = 0;
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

        drmModeConnector* chosen = nullptr;
        for (int i = 0; i < res->count_connectors; ++i) {
            drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c) continue;
            const bool wanted = cfg.connector.empty()
                                    ? (c->connection == DRM_MODE_CONNECTED &&
                                       c->count_modes > 0)
                                    : (connector_name(c) == cfg.connector);
            if (wanted && c->count_modes > 0) { chosen = c; break; }
            drmModeFreeConnector(c);
        }
        if (!chosen) {
            last = path + ": nothing is plugged into it";
            drmModeFreeResources(res);
            ::close(fd);
            continue;
        }

        // The mode the operator asked for, or the one the screen prefers.
        const drmModeModeInfo* picked = nullptr;
        if (cfg.out_width > 0 && cfg.out_height > 0) {
            for (int i = 0; i < chosen->count_modes; ++i) {
                const drmModeModeInfo& m = chosen->modes[i];
                if (m.hdisplay != cfg.out_width || m.vdisplay != cfg.out_height)
                    continue;
                if (cfg.out_fps > 0 &&
                    std::abs(refresh_mhz(m) / 1000 - cfg.out_fps) > 1)
                    continue;
                picked = &m;
                break;
            }
            if (!picked)
                plog_warn("the screen will not do %dx%d at %d Hz — using what "
                          "it prefers instead",
                          cfg.out_width, cfg.out_height, cfg.out_fps);
        }
        if (!picked) {
            for (int i = 0; i < chosen->count_modes; ++i)
                if (chosen->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                    picked = &chosen->modes[i];
                    break;
                }
        }
        if (!picked) picked = &chosen->modes[0];

        // A CRTC to drive it with: the connector's current encoder if it has
        // one, otherwise the first one the connector will accept.
        uint32_t crtc_id = 0;
        if (chosen->encoder_id) {
            if (drmModeEncoder* enc = drmModeGetEncoder(fd, chosen->encoder_id)) {
                crtc_id = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        if (!crtc_id) {
            for (int i = 0; i < chosen->count_encoders && !crtc_id; ++i) {
                drmModeEncoder* enc = drmModeGetEncoder(fd, chosen->encoders[i]);
                if (!enc) continue;
                for (int c = 0; c < res->count_crtcs; ++c) {
                    if (enc->possible_crtcs & (1u << c)) {
                        crtc_id = res->crtcs[c];
                        break;
                    }
                }
                drmModeFreeEncoder(enc);
            }
        }
        if (!crtc_id) {
            last = path + ": no display controller is free for that output";
            drmModeFreeConnector(chosen);
            drmModeFreeResources(res);
            ::close(fd);
            continue;
        }

        m_fd = fd;
        m_card_path = path;
        m_connector_id = chosen->connector_id;
        m_crtc_id = crtc_id;
        m_mode = *picked;
        m_saved_crtc = drmModeGetCrtc(fd, crtc_id);

        char desc[160];
        std::snprintf(desc, sizeof(desc), "%s %dx%d @ %.2f Hz",
                      connector_name(chosen).c_str(), m_mode.hdisplay,
                      m_mode.vdisplay, refresh_mhz(m_mode) / 1000.0);
        m_description = desc;

        drmModeFreeConnector(chosen);
        drmModeFreeResources(res);
        return true;
    }

    error = last.empty() ? "no display could be opened" : last;
    return false;
}

bool DrmOutput::open(const Config& cfg, std::string& error) {
    close();
    if (!pick_device(cfg, error)) return false;

    // Becoming DRM master is what lets this process set the mode. On a box
    // with no desktop it normally succeeds; if the console still holds it,
    // say so plainly rather than failing at the first page flip.
    if (drmSetMaster(m_fd) != 0 && errno != EINVAL && errno != EACCES)
        plog_debug("drmSetMaster: %s", strerror(errno));

    for (int i = 0; i < 2; ++i) {
        if (!create_framebuffer(m_fd, m_mode.hdisplay, m_mode.vdisplay,
                                m_fb[i], error)) {
            close();
            return false;
        }
    }

    if (drmModeSetCrtc(m_fd, m_crtc_id, m_fb[0].fb_id, 0, 0, &m_connector_id, 1,
                       &m_mode) != 0) {
        error = std::string("could not set the display mode: ") + strerror(errno);
        if (errno == EACCES)
            error += ". Something else owns the screen — on Raspberry Pi OS "
                     "with a desktop installed, stop the display manager.";
        close();
        return false;
    }
    m_back = 1;
    m_first_present = true;
    plog_info("display claimed: %s on %s", m_description.c_str(),
              m_card_path.c_str());
    return true;
}

void DrmOutput::close() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0) return;
    wait_for_flip();

    // Put the console back the way it was, so a stopped event does not
    // leave a black screen that looks like broken hardware.
    if (m_saved_crtc) {
        drmModeSetCrtc(m_fd, m_saved_crtc->crtc_id, m_saved_crtc->buffer_id,
                       m_saved_crtc->x, m_saved_crtc->y, &m_connector_id, 1,
                       &m_saved_crtc->mode);
        drmModeFreeCrtc(m_saved_crtc);
        m_saved_crtc = nullptr;
    }
    for (auto& fb : m_fb) fb.destroy(m_fd);
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    m_sws_src_w = m_sws_src_h = 0;
    drmDropMaster(m_fd);
    ::close(m_fd);
    m_fd = -1;
    m_crtc_id = 0;
    m_description = "no display output";
}

void DrmOutput::wait_for_flip() {
    if (!m_flip_pending || m_fd < 0) return;
    drmEventContext ev{};
    ev.version = 2;
    ev.page_flip_handler = on_page_flip;

    // A bounded wait. If the driver never reports the flip — which happens if
    // the screen is unplugged mid-event — playback must carry on rather than
    // block the delivery thread for ever.
    pollfd pfd{};
    pfd.fd = m_fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, 100);
    if (rc > 0) {
        // The handler clears the flag through the user pointer we passed in.
        drmHandleEvent(m_fd, &ev);
    }
    m_flip_pending = false;
}

bool DrmOutput::flip() {
    wait_for_flip();
    m_flip_pending = true;
    if (drmModePageFlip(m_fd, m_crtc_id, back().fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, &m_flip_pending) != 0) {
        m_flip_pending = false;
        // Falling back to a mode set keeps a picture on the screen on drivers
        // or states where a flip is refused — notably right after a mode
        // change, and on the very first frame.
        if (drmModeSetCrtc(m_fd, m_crtc_id, back().fb_id, 0, 0, &m_connector_id,
                           1, &m_mode) != 0)
            return false;
    }
    m_back ^= 1;
    return true;
}

void DrmOutput::present(const multisite::DecodedVideoFrame& frame) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0 || frame.width <= 0 || frame.height <= 0) return;

    // Fit the picture inside the screen without changing its shape.
    if (frame.width != m_sws_src_w || frame.height != m_sws_src_h) {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
        const double scale = std::min((double)m_mode.hdisplay / frame.width,
                                      (double)m_mode.vdisplay / frame.height);
        m_dst_w = std::max(2, (int)(frame.width * scale)) & ~1;
        m_dst_h = std::max(2, (int)(frame.height * scale)) & ~1;
        m_dst_x = (m_mode.hdisplay - m_dst_w) / 2;
        m_dst_y = (m_mode.vdisplay - m_dst_h) / 2;

        m_sws = sws_getContext(frame.width, frame.height, AV_PIX_FMT_YUV420P,
                               m_dst_w, m_dst_h, AV_PIX_FMT_BGRA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            plog_error("could not set up the display scaler");
            m_sws_src_w = m_sws_src_h = 0;
            return;
        }
        m_sws_src_w = frame.width;
        m_sws_src_h = frame.height;
        m_first_present = true;      // the bars around it need painting again
        plog_info("showing %dx%d as %dx%d on a %dx%d screen", frame.width,
                  frame.height, m_dst_w, m_dst_h, m_mode.hdisplay,
                  m_mode.vdisplay);
    }
    if (!m_sws) return;

    // With two buffers, the back one is still on screen until the flip queued
    // last time has happened. Drawing into it before then paints over the
    // picture being scanned out: on a 30 Hz output with 30 fps content the
    // two lock in step and every frame tears at the same line (seen through
    // an HDMI capture; on 60 Hz the flip lands long before the next frame).
    wait_for_flip();
    Framebuffer& fb = back();
    if (!fb.map) return;

    // The bars only need clearing when the letterbox changes, not every frame:
    // at 1080p that is eight megabytes of pointless writes thirty times a
    // second on a box that has better things to do.
    if (m_first_present) {
        std::memset(fb.map, 0, (size_t)fb.size);
        m_first_present = false;
    }

    uint8_t* dst = fb.map + (size_t)m_dst_y * fb.pitch + (size_t)m_dst_x * 4;
    uint8_t* dst_planes[4] = { dst, nullptr, nullptr, nullptr };
    int dst_stride[4] = { (int)fb.pitch, 0, 0, 0 };
    const uint8_t* src[4] = { frame.plane[0], frame.plane[1], frame.plane[2],
                              nullptr };
    const int src_stride[4] = { frame.stride[0], frame.stride[1],
                                frame.stride[2], 0 };
    sws_scale(m_sws, src, src_stride, 0, frame.height, dst_planes, dst_stride);
    flip();
}

void DrmOutput::present_bgrx(int width, int height, int stride,
                             const uint8_t* pixels) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0 || !pixels) return;
    wait_for_flip();   // not into the buffer still on screen (present)
    Framebuffer& fb = back();
    if (!fb.map) return;

    std::memset(fb.map, 0, (size_t)fb.size);
    const int rows = std::min(height, (int)m_mode.vdisplay);
    const int bytes = std::min(stride, (int)fb.pitch);
    for (int y = 0; y < rows; ++y)
        std::memcpy(fb.map + (size_t)y * fb.pitch,
                    pixels + (size_t)y * stride, (size_t)bytes);
    (void)width;
    m_first_present = true;      // a decoded frame after this repaints the bars
    flip();
}

void DrmOutput::blank() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_fd < 0) return;
    wait_for_flip();   // not into the buffer still on screen (present)
    Framebuffer& fb = back();
    if (!fb.map) return;
    std::memset(fb.map, 0, (size_t)fb.size);
    m_first_present = true;
    flip();
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
