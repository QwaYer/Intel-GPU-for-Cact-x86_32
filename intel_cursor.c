/*
 * intel_cursor.c — gen9 (SKL universal) hardware cursor.
 *
 * Legal sizes are 64/128/256 square, ARGB/XRGB 32bpp, buffer a 4 KiB-aligned
 * GGTT graphics address.  The cursor fetch also needs an enabled watermark
 * level (CUR_WM_EN) and a valid DDB window (CUR_BUF_CFG), or it is dropped.
 * Arm order: WM/DDB -> CUR_FBC_CTL -> CUR_CTL -> CUR_POS -> CUR_BASE.  Offsets
 * and encoding come from Linux i915 (see STAGE4_LINUX_NOTES.md).
 */

#include "intel_i915.h"
#include "intel_regs.h"

#define INTEL_CURSOR_PIPE   0
#define INTEL_CURSOR_DDB    8u   /* blocks reserved for the cursor */

/* The cursor needs its own slice of the pipe's data buffer; take it from the
 * top of the 892-block pool, above plane 1's permanent allocation. */
static uint32_t cursor_last_gpa;

static uint32_t cursor_position(int32_t x, int32_t y) {
    uint32_t v = 0;
    if (x < 0) v |= CURSOR_POS_X_SIGN | ((uint32_t)(-x) & CURSOR_POS_X_MASK);
    else       v |= (uint32_t)x & CURSOR_POS_X_MASK;
    if (y < 0) v |= CURSOR_POS_Y_SIGN | (((uint32_t)(-y) & 0x7fffu) << 16);
    else       v |= ((uint32_t)y & 0x7fffu) << 16;
    return v;
}

static void cursor_watermarks(void) {
    intel_write(CUR_WM(INTEL_CURSOR_PIPE, 0), CUR_WM_EN | INTEL_CURSOR_DDB);
    for (int l = 1; l < 8; l++)
        intel_write(CUR_WM(INTEL_CURSOR_PIPE, l), 0);
    intel_write(CUR_WM_TRANS(INTEL_CURSOR_PIPE), 0);
    intel_write(CUR_BUF_CFG(INTEL_CURSOR_PIPE),
                CUR_BUF_END(INTEL_DDB_TOTAL) | CUR_BUF_START(INTEL_DDB_TOTAL - INTEL_CURSOR_DDB));
}

int intel_cursor_set(struct drm_crtc *crtc, struct drm_gem_object *obj,
                     uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y) {
    (void)crtc;
    (void)hot_x;
    (void)hot_y;
    int pipe = INTEL_CURSOR_PIPE;

    if (!obj) {                                  /* hide */
        intel_write(CUR_CTL(pipe), MCURSOR_MODE_DISABLE);
        intel_write(CUR_POS(pipe), 0);
        intel_write(CUR_BASE(pipe), 0);
        (void)intel_read(CUR_BASE(pipe));
        cursor_last_gpa = 0;
        return 0;
    }

    uint32_t mode;
    if      (w == 64 && h == 64)   mode = MCURSOR_MODE_64_ARGB_AX;
    else if (w == 128 && h == 128) mode = MCURSOR_MODE_128_ARGB_AX;
    else if (w == 256 && h == 256) mode = MCURSOR_MODE_256_ARGB_AX;
    else {
        printk("i915: cursor %ux%u unsupported (64/128/256 square only)\n",
               (unsigned)w, (unsigned)h);
        return -22;
    }

    uint32_t gpa = intel_gtt_alloc(w * h * 4u);
    if (intel_ggtt_map_gem(gpa, obj, w * h * 4u) != 0)
        return -12;

    cursor_watermarks();
    intel_write(CUR_FBC_CTL(pipe), 0);           /* square: no FBC height */
    intel_write(CUR_CTL(pipe), mode | MCURSOR_PIPE_GAMMA_ENABLE);
    intel_write(CUR_POS(pipe), cursor_position(0, 0));
    intel_write(CUR_BASE(pipe), gpa);            /* arms */
    (void)intel_read(CUR_BASE(pipe));

    cursor_last_gpa = gpa;
    return 0;
}

int intel_cursor_move(struct drm_crtc *crtc, int32_t x, int32_t y) {
    (void)crtc;
    int pipe = INTEL_CURSOR_PIPE;
    if (!cursor_last_gpa)                        /* hidden: nothing to move */
        return 0;
    intel_write(CUR_POS(pipe), cursor_position(x, y));
    intel_write(CUR_BASE(pipe), cursor_last_gpa);   /* re-arm */
    (void)intel_read(CUR_BASE(pipe));
    return 0;
}
