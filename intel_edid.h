#ifndef INTEL_EDID_H
#define INTEL_EDID_H

/*
 * A small EDID 1.x reader, independent of the kernel's DRM types: it turns a
 * raw EDID blob (base block plus extensions) into a flat list of timings and a
 * little identity.  The Intel module then converts each timing into a
 * struct drm_display_mode.  Keeping it separate makes it testable and keeps the
 * register-level code out of the parsing.
 *
 * Only what a scanout decision needs is extracted: detailed timing descriptors
 * (base block and CEA-861 extension), the 8 standard timings, and the
 * established-timing bits for the resolutions a monitor still commonly
 * advertises.  Everything is expressed in pixels/clock as the display engine
 * programs them, not as EDID byte fields.
 */

#include <stdint.h>

#define INTEL_EDID_MAX_MODES   32
#define INTEL_EDID_MAX_EXT     4
#define INTEL_EDID_RAW_MAX     (128 * (1 + INTEL_EDID_MAX_EXT))

typedef struct {
    uint32_t clock_khz;         /* pixel clock, kHz                     */
    uint16_t hactive;           /* visible width in pixels              */
    uint16_t hblank;            /* horizontal blanking                  */
    uint16_t hsync_offset;      /* front porch                          */
    uint16_t hsync_width;       /* sync pulse width                     */
    uint16_t vactive;
    uint16_t vblank;
    uint16_t vsync_offset;
    uint16_t vsync_width;
    uint8_t  hsync_positive;    /* 1 = positive polarity                */
    uint8_t  vsync_positive;
    uint8_t  interlaced;
    uint16_t width_mm;          /* image size, 0 when unknown           */
    uint16_t height_mm;
    char     name[14];          /* DTD monitor descriptor, else ""      */
} intel_edid_mode_t;

typedef struct {
    uint8_t  valid;
    uint8_t  version;
    uint8_t  revision;
    char     pnp_id[4];         /* manufacturer, 3 letters + NUL         */
    uint16_t product_code;
    uint16_t year;
    uint8_t  week;
    uint8_t  digital;           /* 1 = digital input                     */
    uint8_t  input_bits;        /* colour depth bits, digital only       */
    uint8_t  interface;         /* digital interface code, 0 undefined   */
    uint16_t width_mm;
    uint16_t height_mm;
    uint8_t  has_cea861;
    int      raw_len;
    uint8_t  raw[INTEL_EDID_RAW_MAX];

    int      num_modes;
    intel_edid_mode_t modes[INTEL_EDID_MAX_MODES];

    /* Raw bytes length of the base block + every extension actually read;
     * this is what a driver publishes as the connector's EDID blob. */
} intel_edid_t;

/* Parse `buf[0..len)`.  Returns 0 on a valid base block (extensions, if the
 * base block declares them, are parsed only when their bytes are present).
 * Modes are de-duplicated by (clock, hactive, vactive, interlaced). */
int intel_edid_parse(const uint8_t *buf, int len, intel_edid_t *out);

#endif /* INTEL_EDID_H */
