/*
 * intel_edid.c — EDID 1.x reader (see intel_edid.h).
 *
 * Two sources of timings are read:
 *
 *   - Detailed Timing Descriptors (18 bytes each), four in the base block and
 *     more in a CEA-861 extension.  These carry the full timing (clock,
 *     blanking, sync offsets/widths, polarities), so they are used as-is.  The
 *     first base-block DTD is normally the sink's preferred mode.
 *   - Standard timings (8 x 2 bytes): the bytes only name a resolution and a
 *     refresh rate, so the blanking has to come from somewhere.  A small table
 *     of the common DMT/CEA timings is consulted; anything not in it is
 *     synthesised with nominal blanking and marked non-preferred.
 *
 * Established-timing bits are deliberately ignored: they only name a handful of
 * legacy modes that every modern sink also lists as a standard timing or DTD.
 */

#include <stddef.h>
#include "intel_edid.h"

/* ------------------------------------------------------------------ helpers */

static void edid_memset(void *dst, int c, unsigned n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--)
        *p++ = (uint8_t)c;
}

static void edid_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++) != '\0')
        ;
}

/* EDID descriptors pad text with spaces and terminate with a newline. */
static void edid_copy_text(char *dst, unsigned dst_len, const uint8_t *src,
                           unsigned src_len) {
    unsigned n = 0;
    while (n + 1 < dst_len && n < src_len && src[n] != '\n' && src[n] != '\0')
        n++;
    while (n > 0 && src[n - 1] == ' ')
        n--;
    for (unsigned i = 0; i < n; i++)
        dst[i] = (char)src[i];
    dst[n] = '\0';
}

static int edid_same_mode(const intel_edid_mode_t *a, const intel_edid_mode_t *b) {
    return a->clock_khz == b->clock_khz && a->hactive == b->hactive &&
           a->vactive == b->vactive && a->interlaced == b->interlaced;
}

static int edid_add(intel_edid_t *out, const intel_edid_mode_t *m) {
    if (m->hactive == 0 || m->vactive == 0 || m->clock_khz == 0)
        return 0;
    for (int i = 0; i < out->num_modes; i++)
        if (edid_same_mode(&out->modes[i], m))
            return 0;
    if (out->num_modes >= INTEL_EDID_MAX_MODES)
        return 0;
    out->modes[out->num_modes++] = *m;
    return 1;
}

/* ------------------------------------------------------- detailed timings */

static void edid_parse_dtd(const uint8_t *d, intel_edid_mode_t *m) {
    edid_memset(m, 0, sizeof(*m));
    m->clock_khz = (uint32_t)(d[0] | (d[1] << 8)) * 10u;

    m->hactive      = (uint16_t)(d[2] | ((d[4] >> 4) << 8));
    m->hblank       = (uint16_t)(d[3] | ((d[4] & 0x0F) << 8));
    m->hsync_offset = (uint16_t)(d[5] | (((d[8] >> 6) & 0x03) << 8));
    m->hsync_width  = (uint16_t)(d[6] | (((d[8] >> 4) & 0x03) << 8));

    m->vactive      = (uint16_t)(d[9]  | ((d[11] >> 4) << 8));
    m->vblank       = (uint16_t)(d[10] | ((d[11] & 0x0F) << 8));
    m->vsync_offset = (uint16_t)((d[7] >> 4) | (((d[8] >> 2) & 0x03) << 4));
    m->vsync_width  = (uint16_t)((d[7] & 0x0F) | ((d[8] & 0x03) << 4));

    m->width_mm  = (uint16_t)(d[12] | ((d[14] >> 4) << 8));
    m->height_mm = (uint16_t)(d[13] | ((d[14] & 0x0F) << 8));

    m->interlaced = (d[17] & 0x80) ? 1 : 0;
    if (((d[17] >> 3) & 0x03) == 0x03) {
        /* Digital separate sync: bit 2 is V polarity, bit 1 is H polarity. */
        m->vsync_positive = (d[17] & 0x04) ? 1 : 0;
        m->hsync_positive = (d[17] & 0x02) ? 1 : 0;
    } else {
        /* Digital composite / bipolar / analog: polarity is not carried. */
        m->hsync_positive = 0;
        m->vsync_positive = 0;
    }
}

/* Append the DTDs of a block whose DTDs start at `first_dtd`; stop at the
 * first zero pixel clock or when `count` descriptors have been consumed. */
static void edid_parse_dtds(const uint8_t *block, int first_dtd, int count,
                            intel_edid_t *out) {
    for (int i = 0; i < count; i++) {
        const uint8_t *d = block + first_dtd + i * 18;
        if (d[0] == 0 && d[1] == 0)
            break;
        intel_edid_mode_t m;
        edid_parse_dtd(d, &m);
        edid_add(out, &m);
    }
}

/* ------------------------------------------------------------ common DMT/CEA
 *
 * Exact timings for the resolutions that EDID standard-timing slots name most
 * often; DMT (VESA) and CEA where the mode is a CEA one.  Where a resolution
 * has more than one commonly advertised blanking (CVT vs DMT), the DMT/first
 * value is used.  `refresh` is the nominal refresh the standard-timing byte
 * encodes.
 */
typedef struct {
    uint16_t hactive, vactive, refresh;
    uint32_t clock_khz;
    uint16_t hblank, hsync_offset, hsync_width;
    uint16_t vblank, vsync_offset, vsync_width;
    uint8_t  hpos, vpos;   /* sync polarity, 1 = positive */
} edid_known_t;

static const edid_known_t edid_known[] = {
    {  640,  480, 60,  25175,  160,  16,  96,  45, 10, 2, 0, 0 },
    {  800,  600, 60,  40000,  256,  40, 128,  28,  1, 4, 1, 1 },
    { 1024,  768, 60,  65000,  320,  24, 136,  38,  3, 6, 0, 0 },
    { 1280,  720, 60,  74250,  370, 110,  40,  30,  5, 5, 1, 1 },
    { 1280, 1024, 60, 108000,  408,  48, 112,  42,  1, 3, 1, 1 },
    { 1366,  768, 60,  85500,  426,  70, 143,  30,  3, 3, 1, 1 },
    { 1440,  900, 60, 106500,  464,  80, 152,  34,  3, 6, 0, 0 },
    { 1600,  900, 60, 108000,  480,  24,  32,  40,  1, 3, 1, 1 },
    { 1680, 1050, 60, 146250,  560, 104, 176,  39,  3, 6, 0, 0 },
    { 1920, 1080, 60, 148500,  280,  88,  44,  45,  4, 5, 1, 1 },
    { 1920, 1200, 60, 193250,  640, 136, 200,  43,  3, 6, 0, 0 },
    { 2560, 1440, 60, 241500,  800,  96, 128,  45,  3, 5, 1, 1 },
    { 3840, 2160, 60, 594000,  560, 176,  88,  90,  8, 10, 1, 1 },
};

static int edid_known_lookup(uint16_t hactive, uint16_t vactive, uint16_t refresh,
                             intel_edid_mode_t *out) {
    for (unsigned i = 0; i < sizeof(edid_known) / sizeof(edid_known[0]); i++) {
        const edid_known_t *k = &edid_known[i];
        if (k->hactive != hactive || k->vactive != vactive || k->refresh != refresh)
            continue;
        edid_memset(out, 0, sizeof(*out));
        out->clock_khz     = k->clock_khz;
        out->hactive       = k->hactive;
        out->hblank        = k->hblank;
        out->hsync_offset  = k->hsync_offset;
        out->hsync_width   = k->hsync_width;
        out->vactive       = k->vactive;
        out->vblank        = k->vblank;
        out->vsync_offset  = k->vsync_offset;
        out->vsync_width   = k->vsync_width;
        out->hsync_positive = k->hpos;
        out->vsync_positive = k->vpos;
        return 1;
    }
    return 0;
}

/* Standard-timing aspect ratio bits → vertical active for a given horizontal. */
static uint16_t edid_std_vactive(uint16_t hactive, uint8_t aspect) {
    switch (aspect) {
    case 0: return (uint16_t)((uint32_t)hactive * 10 / 16); /* 16:10 */
    case 1: return (uint16_t)((uint32_t)hactive * 3 / 4);   /* 4:3   */
    case 2: return (uint16_t)((uint32_t)hactive * 4 / 5);   /* 5:4   */
    default: return (uint16_t)((uint32_t)hactive * 9 / 16); /* 16:9  */
    }
}

/* Nominal blanking so a standard timing always yields a usable mode even when
 * the timing is not in the table above.  Mirrors the "typical" VESA ratios. */
static void edid_synth_timing(uint16_t hactive, uint16_t vactive, uint16_t refresh,
                              intel_edid_mode_t *m) {
    edid_memset(m, 0, sizeof(*m));
    uint16_t hfront = (uint16_t)(hactive / 40);
    uint16_t hsync  = (uint16_t)(hactive / 20);
    uint16_t htotal = (uint16_t)(hactive + hfront + hsync + hactive / 10);
    uint16_t vfront = 3, vsync = 6;
    uint16_t vtotal = (uint16_t)(vactive + vfront + vsync + vactive / 40);
    m->hactive       = hactive;
    m->hblank        = (uint16_t)(htotal - hactive);
    m->hsync_offset  = hfront;
    m->hsync_width   = hsync;
    m->vactive       = vactive;
    m->vblank        = (uint16_t)(vtotal - vactive);
    m->vsync_offset  = vfront;
    m->vsync_width   = vsync;
    m->hsync_positive = 0;
    m->vsync_positive = 0;
    m->clock_khz = (uint32_t)htotal * (uint32_t)vtotal * refresh / 1000u;
}

/* -------------------------------------------------------------- block walk */

static void edid_parse_standard(const uint8_t *b, intel_edid_t *out) {
    for (int i = 0; i < 8; i++) {
        const uint8_t *st = b + 38 + i * 2;
        if (st[0] == 0x00 && st[1] == 0x00)
            continue;                    /* unused slot          */
        if (st[0] == 0x01)
            continue;                    /* DMT ID form, no res  */
        uint16_t hactive = (uint16_t)((st[0] + 31) * 8);
        uint16_t refresh = (uint16_t)((st[1] & 0x3F) + 60);
        uint16_t vactive = edid_std_vactive(hactive, (uint8_t)(st[1] >> 6));

        intel_edid_mode_t m;
        if (edid_known_lookup(hactive, vactive, refresh, &m))
            edid_add(out, &m);
        else {
            edid_synth_timing(hactive, vactive, refresh, &m);
            edid_add(out, &m);
        }
    }
}

/* CEA-861 extension: short video descriptors and a second DTD block.  Only the
 * DTDs are taken (the SVDs name modes already covered by the base block or the
 * table above, without timings). */
static void edid_parse_cea(const uint8_t *b, intel_edid_t *out) {
    if (b[0] != 0x02)
        return;
    out->has_cea861 = 1;
    int dtd_off = b[2];
    if (dtd_off >= 4 && dtd_off < 127) {
        int space = 127 - dtd_off;
        int count = space / 18;
        edid_parse_dtds(b, dtd_off, count, out);
    }
}

/* -------------------------------------------------------------------- entry */

int intel_edid_parse(const uint8_t *buf, int len, intel_edid_t *out) {
    if (!buf || !out || len < 128)
        return -1;

    static const uint8_t magic[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    for (int i = 0; i < 8; i++)
        if (buf[i] != magic[i])
            return -1;

    edid_memset(out, 0, sizeof(*out));

    /* Manufacturer PNP id: three 5-bit letters packed big-endian. */
    uint16_t mfg = (uint16_t)((buf[8] << 8) | buf[9]);
    out->pnp_id[0] = (char)('A' + ((mfg >> 10) & 0x1F) - 1);
    out->pnp_id[1] = (char)('A' + ((mfg >> 5) & 0x1F) - 1);
    out->pnp_id[2] = (char)('A' + (mfg & 0x1F) - 1);
    out->pnp_id[3] = '\0';

    out->product_code = (uint16_t)(buf[10] | (buf[11] << 8));
    out->week         = buf[16];
    out->year         = (uint16_t)(1990 + buf[17]);
    out->version      = buf[18];
    out->revision     = buf[19];

    out->digital = (buf[20] & 0x80) ? 1 : 0;
    if (out->digital) {
        out->input_bits = (uint8_t)((buf[20] >> 4) & 0x07) + 4; /* 4..11 */
        out->interface  = (uint8_t)(buf[20] & 0x0F);
    }
    out->width_mm  = (uint16_t)(buf[21] * 10);   /* EDID stores cm */
    out->height_mm = (uint16_t)(buf[22] * 10);

    int ext_count = buf[126];
    int raw_len = 128;
    if (ext_count > INTEL_EDID_MAX_EXT)
        ext_count = INTEL_EDID_MAX_EXT;
    if (len > INTEL_EDID_RAW_MAX)
        len = INTEL_EDID_RAW_MAX;

    /* Base-block descriptors, four slots; text descriptors name the monitor. */
    for (int i = 0; i < 4; i++) {
        const uint8_t *d = buf + 54 + i * 18;
        if (d[0] != 0 || d[1] != 0) {
            intel_edid_mode_t m;
            edid_parse_dtd(d, &m);
            if (i == 0)
                m.name[0] = '\0';
            edid_add(out, &m);
            continue;
        }
        /* Monitor descriptor: 0x00 0x00 <pad=0> <tag> … */
        if (d[3] == 0xFC) {
            for (int j = 0; j < 4; j++)
                if (d[5 + j] == 0x0A) { /* terminate at first newline slot */
                    d = buf + 54 + i * 18;
                    break;
                }
            if (out->num_modes > 0)
                edid_copy_text(out->modes[0].name, sizeof(out->modes[0].name),
                               d + 5, 13);
        }
    }

    edid_parse_standard(buf, out);

    for (int i = 0; i < ext_count; i++) {
        int off = 128 * (i + 1);
        if (off + 128 > len)
            break;
        raw_len = off + 128;
        if (buf[off] == 0x02)
            edid_parse_cea(buf + off, out);
    }

    if (raw_len <= INTEL_EDID_RAW_MAX) {
        for (int i = 0; i < raw_len; i++)
            out->raw[i] = buf[i];
        out->raw_len = raw_len;
    }

    /* If the sink named the preferred mode only through a monitor descriptor
     * name (rare, and it happens on cheap panels), the name is still useful even
     * when no mode was added. */
    if (out->num_modes == 0) {
        edid_strcpy(out->pnp_id, "UNKN");
        return -1;
    }

    out->valid = 1;
    return 0;
}
