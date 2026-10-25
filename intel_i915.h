#ifndef INTEL_I915_H
#define INTEL_I915_H

/*
 * intel_i915.h — shared state and kernel entry points for the Intel GPU module.
 *
 * The module is a loadable PCI driver (pci_driver_probe / pci_driver_remove)
 * that talks to the kernel's generic DRM/KMS core: it discovers the display
 * outputs of a Gen9.5 (Coffee Lake, "UHD Graphics 630") part, publishes them as
 * a DRM device with connector modes, and — in later stages — programs the
 * display engine to scan out a client framebuffer.
 *
 * Register access goes through `mmio` (the GTTMMADR BAR, mapped uncached by the
 * module).  All offsets are the raw Intel MMIO offsets from intel_regs.h.
 */

#include <stdint.h>
#include "pci_enum.h"
#include "drm_drv.h"

/* ── kernel entry points (resolved through ksym at load time) ────────────── */

extern void     printk(const char *fmt, ...);
extern void     printk_hex(uint32_t v);
extern int      snprintf(char *buf, unsigned int size, const char *fmt, ...);
extern void    *kmalloc(uint32_t size);
extern void    *kmalloc_aligned(uint32_t size, uint32_t align);
extern void     kfree(void *p);
extern void    *memset(void *s, int c, uint32_t n);
extern void    *memcpy(void *dst, const void *src, uint32_t n);
extern int      memcmp(const void *a, const void *b, uint32_t n);
extern int      strcmp(const char *a, const char *b);
extern unsigned strlen(const char *s);

extern uint32_t pci_read_config_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
extern void     pci_write_config_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                                       uint32_t val);
extern uint8_t  inb(uint16_t port);
extern void     outb(uint16_t port, uint8_t val);
extern uint32_t inl(uint16_t port);
extern void     outl(uint16_t port, uint32_t val);

extern void     vmm_map(uint32_t *pd, uint32_t va, uint32_t pa, int flags);
extern uint32_t vmm_get_phys(uint32_t *pd, uint32_t virt);

extern int      msidev_register(pci_device_t *dev, void (*handler)(void));
extern void     msidev_unregister(int vec);
/* Must match the kernel's prototype exactly: a uint32_t here would put only
 * four bytes on the stack while the callee reads eight, so the delay argument
 * becomes garbage in its high half.  That is a hang, not a wrong value. */
extern void     ktime_busy_wait_us(uint64_t us);

/* DRM/KMS core. */
extern struct drm_device *drm_dev_create(const drm_driver_ops_t *ops, void *priv);
extern void  drm_dev_free(struct drm_device *dev);
extern int   drm_dev_register(struct drm_device *dev);
extern void  drm_dev_unregister(struct drm_device *dev);

/* ── page-table flags (vmm_map) ──────────────────────────────────────────── */

#define INTEL_PAGE_PRESENT 0x1
#define INTEL_PAGE_RW      0x2
#define INTEL_PAGE_PWT     0x8
#define INTEL_PAGE_PCD     0x10

/* ── the device ──────────────────────────────────────────────────────────── */

#define INTEL_MAX_PORTS      4      /* DDI A..D                          */
#define INTEL_EDID_BUF_SIZE  512    /* base block + up to 3 extensions   */

typedef struct {
    int      present;               /* a sink answered on this DDI       */
    int      is_dp;                 /* 1 = DisplayPort, 0 = HDMI/DVI     */
    int      ddc_pin;               /* GMBUS pin pair that worked, -1    */
    int      via_aux;               /* EDID came from DP AUX            */
    uint8_t  edid[INTEL_EDID_BUF_SIZE];
    int      edid_len;
    uint32_t connector_type;        /* DRM_MODE_CONNECTOR_*              */
    uint32_t type_id;
} intel_port_t;

/* One fully-resolved mode to program onto a pipe/port. */
typedef struct {
    int      port;                 /* DDI index 0..4                    */
    int      pipe;                 /* pipe == transcoder index 0..2     */
    uint32_t pixel_clock_khz;
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    int      hsync_positive, vsync_positive;
    uint32_t gpa;                  /* GGTT graphics address of the fb   */
    uint32_t width, height, pitch;
    int      cpp;                  /* bytes per pixel: 2 or 4           */
    int      order_rgbx;           /* 1 = BGRX, i.e. swap R and B       */
} intel_crtc_mode_t;

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor_id, device_id;
    uint8_t  revision;

    uint32_t mmio;                  /* BAR0 GTTMMADR, identity mapped    */
    uint32_t mmio_size;
    uint32_t gmadr;                 /* BAR2 aperture (later stages)      */
    uint32_t gmadr_size;

    uint32_t saved_cmd_dw;
    int      attached;
    int      msix_vector;

    int      display_ver;           /* display version (10 for Gen9.5)   */
    int      num_pipes;

    int          num_ports;
    intel_port_t ports[INTEL_MAX_PORTS];

    struct drm_device *drm;
} intel_dev_t;

extern intel_dev_t intel;

/* Register access.  The MMIO BAR is mapped uncached. */
static inline uint32_t intel_read(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(intel.mmio + reg);
}
static inline void intel_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(intel.mmio + reg) = val;
}
static inline uint32_t intel_read_mask(uint32_t reg, uint32_t mask) {
    return intel_read(reg) & mask;
}
static inline void intel_write_mask(uint32_t reg, uint32_t mask, uint32_t val) {
    uint32_t old = intel_read(reg);
    intel_write(reg, (old & ~mask) | (val & mask));
}

/* ── sub-modules ─────────────────────────────────────────────────────────── */

/* intel_display.c */
int  intel_display_identify(void);   /* chip / display version          */
int  intel_display_power_on(void);   /* display + DDI power wells       */
void intel_display_power_off(void);
int  intel_probe_ports(void);        /* fill intel.ports[]              */

/* intel_gmbus.c */
int  intel_gmbus_read_edid(int pin_pair, uint8_t *buf, int buf_len);
int  intel_aux_read_edid(int ddi, uint8_t *buf, int buf_len);

/* ── stage 3: scanout ────────────────────────────────────────────────────── */

/* GGTT window this driver owns for scanout buffers: a 256 KiB-aligned graphics
 * address inside the GMADR aperture, so the mapping can also be checked from
 * the CPU (GMADR is a CPU window onto the GGTT). */
#define INTEL_SCANOUT_GPA   0x08000000u

/* intel_modeset.c */
int  intel_ggtt_map_gem (uint32_t gpa, struct drm_gem_object *obj, uint32_t size);
void intel_ggtt_flush(void);
int  intel_modeset_transcoder_for_port(int port);   /* -1 when none active */
/* Take over `port` scanning out the kernel's boot framebuffer, keeping the
 * firmware's DPLL/CDCLK and the active transcoder's timings.  Returns 0 and
 * fills w/h on success. */
int  intel_modeset_handoff_console(int port, uint32_t *w, uint32_t *h);
int  intel_modeset_enable(const intel_crtc_mode_t *m);
void intel_modeset_disable(int pipe);
/* Page flip: re-point the primary plane at a new GGTT address. */
int  intel_modeset_flip(int pipe, uint32_t gpa, uint32_t width, uint32_t height,
                        uint32_t pitch, int cpp, int order_rgbx);
/* Bump-allocate a GGTT window for a scanout buffer (4 KiB aligned). */
uint32_t intel_gtt_alloc(uint32_t size);

/* intel_irq.c — vblank interrupt. */
int  intel_irq_init(pci_device_t *pdev);      /* register the MSI handler */
void intel_irq_fini(void);
void intel_irq_set_crtc(struct drm_crtc *crtc);
int  intel_irq_enable_vblank(struct drm_device *dev, struct drm_crtc *crtc);
void intel_irq_disable_vblank(struct drm_device *dev, struct drm_crtc *crtc);

/* intel_cursor.c — hardware cursor (SKL universal, single pipe). */
int  intel_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *obj,
                      uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y);
int  intel_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y);

#endif /* INTEL_I915_H */
