/*
 * i915_mod.c — Intel GPU (Gen9.5 / Coffee Lake "UHD Graphics 630") for Cact kmod.
 * Lives under Projects/Intel-GPU-for-Cact-x86_32 (outside the kernel tree).
 *
 * This is the module entry point and the DRM-facing layer:
 *
 *   pci_driver_probe()
 *     └─ map the GTTMMADR BAR, identify the display version, bring up the
 *        display power wells, probe the DDI outputs (EDID over GMBUS/DDC or
 *        DP AUX), and publish a DRM/KMS device whose connectors carry the
 *        modes the sinks report.
 *
 * Scanning out (DPLL/DDI/pipe/plane + GTT) is a later stage: set_config()
 * deliberately reports "not supported" so nothing pretends the display has been
 * taken over.  The module is display-only — the render engine (GuC/HuC,
 * execbuf) is out of scope.
 *
 * Build: meson setup build-meson --cross-file cross/i686-cact-clang.ini \
 *            -Dkern_root=../CactKernel-x86_32 -Dlocal_repo=../LocalRepoCactOS-x86_32
 *        ninja -C build-meson          -> build-meson/i915.cctk
 *        ninja -C build-meson stage    -> <local_repo>/lib/i915.cctk
 * Load (root): modload /usr/lib/modules/i915.cctk   (or just: modload i915.cctk)
 *   The .cctk must be HMAC signed; the LocalRepo signs and packs it into
 *   cctkfs.img, after which `ninja -C CactKernel-x86_32/build-meson iso-full`
 *   produces a bootable image.
 */

#include <stddef.h>
#include "intel_i915.h"
#include "intel_edid.h"

/* ── manifest ─────────────────────────────────────────────────────────────
 *
 * The kernel's pci_peek_module_manifest() reads these from the .cctk: a vendor
 * id plus a zero-terminated device-id list.  Bound by VID/DID rather than by
 * class (03:00 means "VGA"): a class manifest would also match every other
 * graphics function on the bus.
 *
 * The ids are the Coffee Lake / Coffee Lake-H GT2 (UHD Graphics 630) set; the
 * i3-8100 reports 0x3E91.  0x3E90/0x3E93 (GT1) are included so a UHD 610 part
 * binds to the same display code — the display block is identical.
 */
const uint16_t cact_pci_vendor_id = 0x8086;

const uint16_t cact_pci_device_ids[] = {
    0x3E90, 0x3E91, 0x3E92, 0x3E93, 0x3E94, 0x3E96, 0x3E98, 0x3E9A, 0x3E9B,
    0,
};

/* Errno the kernel's DRM uapi uses (Linux numbering). */
#define INTEL_EOPNOTSUPP  95

/* ── module state ──────────────────────────────────────────────────────── */

intel_dev_t intel;

/* ── MMIO ──────────────────────────────────────────────────────────────── */

static inline uint32_t intel_get_kernel_pd(void) {
    uint32_t pd;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(pd));
    return pd;
}

/* Map the GTTMMADR BAR uncached: a display register must never be cached. */
static int intel_mmio_map(uint32_t base, uint32_t size) {
    if (!base || size < 0x1000)
        return -1;
    uint32_t pd = intel_get_kernel_pd();
    for (uint32_t off = 0; off < size; off += 0x1000)
        vmm_map((uint32_t *)(uintptr_t)pd, base + off, base + off,
                INTEL_PAGE_PRESENT | INTEL_PAGE_RW | INTEL_PAGE_PCD | INTEL_PAGE_PWT);
    return 0;
}

/* ── DRM: modes from an EDID timing ─────────────────────────────────────── */

static void intel_mode_from_edid(const intel_edid_mode_t *e,
                                 struct drm_display_mode *m,
                                 uint32_t type, const char *name) {
    memset(m, 0, sizeof(*m));
    m->clock       = e->clock_khz;
    m->hdisplay    = e->hactive;
    m->hsync_start = (uint16_t)(e->hactive + e->hsync_offset);
    m->hsync_end   = (uint16_t)(e->hactive + e->hsync_offset + e->hsync_width);
    m->htotal      = (uint16_t)(e->hactive + e->hblank);
    m->hskew       = 0;
    m->vdisplay    = e->vactive;
    m->vsync_start = (uint16_t)(e->vactive + e->vsync_offset);
    m->vsync_end   = (uint16_t)(e->vactive + e->vsync_offset + e->vsync_width);
    m->vtotal      = (uint16_t)(e->vactive + e->vblank);
    m->vscan       = 0;
    m->flags = (e->hsync_positive ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
               (e->vsync_positive ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC) |
               (e->interlaced ? DRM_MODE_FLAG_INTERLACE : 0);
    m->type  = type;
    if (name && name[0])
        snprintf(m->name, DRM_MODE_NAME_LEN, "%s", name);
    else
        snprintf(m->name, DRM_MODE_NAME_LEN, "%ux%u", (unsigned)e->hactive,
                 (unsigned)e->vactive);
}

/* ── DRM: ops ───────────────────────────────────────────────────────────── */

static const uint32_t intel_plane_formats[] = {
    DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888,
    DRM_FORMAT_ABGR8888, DRM_FORMAT_RGB565,
};

static int intel_drm_load(struct drm_device *dev) {
    if (drm_mode_crtc_init(dev, 0, "pipe-a") != 0)
        return -12;

    struct drm_crtc *crtc = drm_crtc_find(dev, 1);
    if (!crtc)
        return -12;
    intel_irq_set_crtc(crtc);

    if (drm_mode_plane_init(dev, 0, DRM_PLANE_TYPE_PRIMARY, 1, intel_plane_formats,
                            (int)(sizeof(intel_plane_formats) /
                                  sizeof(intel_plane_formats[0])), 0) != 0)
        return -12;

    int idx = 0;
    for (int i = 0; i < INTEL_MAX_PORTS; i++) {
        intel_port_t *p = &intel.ports[i];
        if (!p->present)
            continue;

        if (drm_mode_encoder_init(dev, idx, DRM_MODE_ENCODER_TMDS, 1, 0) != 0)
            return -12;
        if (drm_mode_connector_init(dev, idx, p->connector_type, p->type_id) != 0)
            return -12;

        struct drm_encoder   *enc  = drm_encoder_find(dev, (uint32_t)(idx + 1));
        struct drm_connector *conn = drm_connector_find(dev, (uint32_t)(idx + 1));
        if (!enc || !conn)
            return -12;

        conn->status = DRM_MODE_CONNECTED;

        intel_edid_t edid;
        const char *src = p->via_aux ? "AUX" : "GMBUS";
        if (p->edid_len >= 128 &&
            intel_edid_parse(p->edid, p->edid_len, &edid) == 0) {
            conn->mm_width  = edid.width_mm;
            conn->mm_height = edid.height_mm;
            drm_connector_set_edid(conn, p->edid, edid.raw_len);

            for (int m = 0; m < edid.num_modes; m++) {
                struct drm_display_mode dm;
                uint32_t type = DRM_MODE_TYPE_DRIVER;
                if (m == 0)
                    type |= DRM_MODE_TYPE_PREFERRED;
                intel_mode_from_edid(&edid.modes[m], &dm, type, edid.modes[m].name);
                drm_connector_add_mode(conn, &dm);
            }

            printk("\x01" "6" "  %-11s : DDI-%c %s (%s): %s, %u mode(s)\n",
                   "i915", (char)('A' + i), p->is_dp ? "DP" : "HDMI", src,
                   edid.pnp_id[0] ? edid.pnp_id : "EDID",
                   (unsigned)conn->count_modes);
        } else {
            printk("\x01" "4" "  %-11s : DDI-%c %s (%s): no EDID\n",
                   "i915", (char)('A' + i), p->is_dp ? "DP" : "HDMI", src);
        }

        drm_connector_attach_encoder(conn, enc);
        drm_encoder_attach_crtc(enc, crtc);

        idx++;
    }

    return 0;
}

static void intel_drm_unload(struct drm_device *dev) {
    (void)dev;
}

/* First connected DDI, or -1 when nothing is attached. */
static int intel_connected_port(void) {
    for (int i = 0; i < INTEL_MAX_PORTS; i++)
        if (intel.ports[i].present)
            return i;
    return -1;
}

/*
 * Program a client framebuffer onto the primary plane.  CDCLK/DPLL are still
 * inherited from firmware, so this drives the port that is already up; the
 * pixel clock itself is not (yet) reprogrammed on gen9.
 */
static int intel_drm_set_config(struct drm_device *dev, struct drm_mode_set *set) {
    (void)dev;
    if (!set || !set->crtc)
        return -22;

    int port = intel_connected_port();
    if (port < 0)
        return -INTEL_EOPNOTSUPP;
    int t    = intel_modeset_transcoder_for_port(port);
    int pipe = (t >= 0) ? t : (port <= 2 ? port : 0);

    /* drmModeSetCrtc(…, fb = 0, …) disables the output. */
    if (!set->fb) {
        intel_modeset_disable(pipe);
        return 0;
    }

    struct drm_framebuffer *fb = set->fb;
    if (fb->modifier != 0)                 /* only linear scanout for now */
        return -INTEL_EOPNOTSUPP;

    int cpp, order_rgbx = 0;
    switch (fb->format) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
        cpp = 4; break;
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        cpp = 4; order_rgbx = 1; break;
    case DRM_FORMAT_RGB565:
        cpp = 2; break;
    default:
        return -INTEL_EOPNOTSUPP;
    }
    if (!fb->obj || (fb->pitch & 63u))
        return -INTEL_EOPNOTSUPP;

    if (intel_ggtt_map_gem(INTEL_SCANOUT_GPA, fb->obj, fb->pitch * fb->height) != 0)
        return -12;

    intel_crtc_mode_t m;
    memset(&m, 0, sizeof(m));
    m.port = port;
    m.pipe = pipe;
    m.pixel_clock_khz = set->mode.clock;
    m.hdisplay    = set->mode.hdisplay;
    m.hsync_start = set->mode.hsync_start;
    m.hsync_end   = set->mode.hsync_end;
    m.htotal      = set->mode.htotal;
    m.vdisplay    = set->mode.vdisplay;
    m.vsync_start = set->mode.vsync_start;
    m.vsync_end   = set->mode.vsync_end;
    m.vtotal      = set->mode.vtotal;
    m.hsync_positive = (set->mode.flags & DRM_MODE_FLAG_PHSYNC) ? 1 : 0;
    m.vsync_positive = (set->mode.flags & DRM_MODE_FLAG_PVSYNC) ? 1 : 0;
    m.gpa = INTEL_SCANOUT_GPA;
    m.width = fb->width;
    m.height = fb->height;
    m.pitch = fb->pitch;
    m.cpp = cpp;
    m.order_rgbx = order_rgbx;

    if (intel_modeset_enable(&m) != 0)
        return -INTEL_EOPNOTSUPP;

    printk("\x01" "6" "  %-11s : modeset DDI-%c %ux%u @ %u kHz\n", "i915",
           (char)('A' + port), (unsigned)m.width, (unsigned)m.height,
           (unsigned)m.pixel_clock_khz);
    return 0;
}

/*
 * PAGE_FLIP.  The core has already installed the new framebuffer in crtc->fb;
 * all that is left is to re-point the primary plane at its GGTT-mapped pages.
 */
static int intel_drm_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
                               uint32_t flags, void *user_data) {
    (void)crtc;
    (void)flags;
    (void)user_data;
    if (!fb || !fb->obj || fb->modifier != 0 || (fb->pitch & 63u))
        return -INTEL_EOPNOTSUPP;

    int cpp, order_rgbx = 0;
    switch (fb->format) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
        cpp = 4; break;
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
        cpp = 4; order_rgbx = 1; break;
    case DRM_FORMAT_RGB565:
        cpp = 2; break;
    default:
        return -INTEL_EOPNOTSUPP;
    }

    int port = intel_connected_port();
    int t    = (port >= 0) ? intel_modeset_transcoder_for_port(port) : -1;
    int pipe = (t >= 0) ? t : 0;

    uint32_t gpa = intel_gtt_alloc(fb->pitch * fb->height);
    if (intel_ggtt_map_gem(gpa, fb->obj, fb->pitch * fb->height) != 0)
        return -12;

    return intel_modeset_flip(pipe, gpa, fb->width, fb->height,
                              fb->pitch, cpp, order_rgbx);
}

static const drm_driver_ops_t intel_drm_ops = {
    .name       = "i915",
    .desc       = "Intel Gen9.5 (Coffee Lake) display engine",
    .major      = 0,
    .minor      = 1,
    .patchlevel = 0,
    .load       = intel_drm_load,
    .unload     = intel_drm_unload,
    .set_config = intel_drm_set_config,
    .page_flip  = intel_drm_page_flip,
    .enable_vblank  = intel_irq_enable_vblank,
    .disable_vblank = intel_irq_disable_vblank,
    .cursor_set = intel_cursor_set,
    .cursor_move = intel_cursor_move,
};

/* ── probe / remove ─────────────────────────────────────────────────────── */

static void intel_reset_state(void) {
    memset(&intel, 0, sizeof(intel));
    intel.msix_vector = -1;
    for (int i = 0; i < INTEL_MAX_PORTS; i++)
        intel.ports[i].ddc_pin = -1;
}

static int intel_pci_setup(pci_device_t *pdev) {
    intel.bus     = pdev->bus;
    intel.dev     = pdev->dev;
    intel.fn      = pdev->fn;
    intel.vendor_id = pdev->vendor_id;
    intel.device_id = pdev->device_id;
    intel.revision  = pdev->revision;

    /* BAR0 is GTTMMADR (registers + GTT), BAR2 is the GMADR aperture.  A
     * 32-bit kernel cannot address a BAR above 4 GiB. */
    if (pdev->bars[0].base && !pdev->bars[0].is_io && pdev->bars[0].base <= 0xFFFFFFFFu) {
        intel.mmio      = (uint32_t)pdev->bars[0].base;
        intel.mmio_size = (uint32_t)pdev->bars[0].size;
    }
    if (pdev->bars[2].base && !pdev->bars[2].is_io && pdev->bars[2].base <= 0xFFFFFFFFu) {
        intel.gmadr      = (uint32_t)pdev->bars[2].base;
        intel.gmadr_size = (uint32_t)pdev->bars[2].size;
    }
    if (!intel.mmio)
        return -1;
    if (intel.mmio_size > 0x1000000)      /* the register window is 16 MiB */
        intel.mmio_size = 0x1000000;

    /* Decode memory space and become a bus master (the display engine reads the
     * scanout buffer directly in later stages). */
    intel.saved_cmd_dw = pci_read_config_dword(intel.bus, intel.dev, intel.fn, 0x04);
    uint32_t cmd = intel.saved_cmd_dw | 0x06u;
    pci_write_config_dword(intel.bus, intel.dev, intel.fn, 0x04, cmd);

    if (intel_mmio_map(intel.mmio, intel.mmio_size) != 0)
        return -1;
    return 0;
}

static void intel_detach(void) {
    if (intel.drm) {
        drm_dev_unregister(intel.drm);
        drm_dev_free(intel.drm);
        intel.drm = NULL;
    }
    if (intel.attached)
        intel_display_power_off();
    intel_irq_fini();
    if (intel.msix_vector >= 0) {
        msidev_unregister(intel.msix_vector);
        intel.msix_vector = -1;
    }
    if (intel.saved_cmd_dw)
        pci_write_config_dword(intel.bus, intel.dev, intel.fn, 0x04,
                               intel.saved_cmd_dw);
    intel_reset_state();
}

int pci_driver_probe(pci_device_t *pdev) {
    if (!pdev)
        return -1;
    intel_reset_state();

    if (intel_pci_setup(pdev) != 0) {
        printk("\x01" "3" "i915: BAR0 (GTTMMADR) missing — cannot attach\n");
        return -1;
    }

    if (intel_display_identify() != 0) {
        printk("\x01" "3" "i915: not a recognised Intel display (MMIO %08x)\n",
               (unsigned)intel.mmio);
        intel_detach();
        return -1;
    }
    if (intel_display_power_on() != 0) {
        printk("\x01" "3" "i915: display power-up failed\n");
        intel_detach();
        return -1;
    }
    intel.attached = 1;

    /* Register the display-interrupt MSI handler.  Non-fatal: without it the
     * core still synthesises vblanks. */
    (void)intel_irq_init(pdev);

    if (intel_probe_ports() != 0 || intel.num_ports == 0) {
        printk("\x01" "4" "i915: no connected display on DDI A-D\n");
        /* Keep the device: a monitor plugged in later still needs the driver. */
    }

    intel.drm = drm_dev_create(&intel_drm_ops, NULL);
    if (!intel.drm) {
        printk("\x01" "3" "i915: DRM device registration failed\n");
        intel_detach();
        return -1;
    }

    /* Take the display over: keep the console visible through our own
     * pipe/plane by scanning the kernel's boot framebuffer via the GGTT. */
    int handoff_port = intel_connected_port();
    if (handoff_port >= 0) {
        uint32_t w = 0, h = 0;
        if (intel_modeset_handoff_console(handoff_port, &w, &h) == 0)
            printk("\x01" "6" "  %-11s : console handoff on DDI-%c %ux%u — scanout live\n",
                   "i915", (char)('A' + handoff_port), (unsigned)w, (unsigned)h);
    }

    printk("\x01" "6" "i915: %04x:%04x display ver %d, %d port(s) — /dev/dri ready\n",
           (unsigned)intel.vendor_id, (unsigned)intel.device_id,
           intel.display_ver, intel.num_ports);
    return 0;
}

void pci_driver_remove(pci_device_t *dev) {
    (void)dev;
    intel_detach();
    printk("\x01" "6" "i915: unloaded\n");
}
