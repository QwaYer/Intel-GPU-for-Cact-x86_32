/*
 * intel_modeset.c — stage 3: program the gen9 display pipeline to scan out.
 *
 * This makes the driver actually drive the display instead of only reporting
 * connector modes.  The blocks it programs:
 *
 *   - the GGTT translation table (the plane reads a *graphics* address, so
 *     every scanout buffer is first mapped into a GGTT window);
 *   - the display data buffer slice (a plane fetching from an unpowered DBUF
 *     underruns);
 *   - the transcoder timings, TRANSCONF and TRANS_DDI_FUNC_CTL;
 *   - the DDI encoder (HDMI buffer translation, I_boost, WA #1143, buffer
 *     enable);
 *   - the watermark / DDB allocation;
 *   - the primary plane, pointing at the mapped framebuffer.
 *
 * Milestone note: CDCLK and the WRPLL are *inherited* from firmware, which is
 * already driving this port.  `intel_modeset_enable()` therefore reproduces the
 * active transcoder's timings unchanged for the console handoff and keeps the
 * port's existing DDI clock select; programming CDCLK/DPLL from scratch is the
 * next step.  Register offsets/field encodings are ported from Linux i915
 * (see STAGE3_LINUX_NOTES.md for the file:line of each one).
 */

#include <stddef.h>
#include "intel_i915.h"
#include "intel_regs.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void cpu_flush_caches(void) {
    /* The gen9 GGTT PTE has no cache/snoop bits: the CPU must push its writes
     * to memory before the display engine reads the buffer. */
    __asm__ __volatile__("wbinvd" ::: "memory");
}

static int wait_set(uint32_t reg, uint32_t mask, int tries, uint32_t step_us) {
    for (int i = 0; i < tries; i++) {
        if (intel_read(reg) & mask)
            return 0;
        ktime_busy_wait_us(step_us);
    }
    return -1;
}

/* ── GGTT ────────────────────────────────────────────────────────────────── */

void intel_ggtt_flush(void) {
    intel_write(GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
    (void)intel_read(GFX_FLSH_CNTL_GEN6);
    intel_write(GFX_FLSH_CNTL_GEN6, 0);
}

/* Write the PTE for one 4 KiB graphics address.  gen9 PTE = phys | present,
 * 64-bit, at BAR0 + 0x800000 + (gpa >> 12) * 8. */
static void gtt_write_pte(uint32_t gpa, uint32_t phys) {
    volatile uint64_t *pte = (volatile uint64_t *)(uintptr_t)
        (intel.mmio + INTEL_GTT_PTE_BASE + (((gpa >> 12) & 0xfffffu) * 8u));
    *pte = (uint64_t)(phys & 0xfffff000u) | (uint64_t)INTEL_GTT_PTE_PRESENT;
}

int intel_ggtt_map_gem(uint32_t gpa, struct drm_gem_object *obj, uint32_t size) {
    if (!obj || (gpa & 0xfffu) || size == 0)
        return -1;
    uint32_t pages = (size + 4095u) / 4096u;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = drm_gem_page_phys(obj, i);
        if (!phys)
            return -1;
        gtt_write_pte(gpa + i * 4096u, phys);
    }
    intel_ggtt_flush();
    return 0;
}

/* GGTT window allocator for scanout buffers.  Each buffer gets its own window
 * so a flip never has to overwrite the mapping still being scanned; the bump
 * pointer walks a 64 MiB region above the fixed console window and wraps. */
static uint32_t gtt_bump = INTEL_SCANOUT_GPA + 0x2000000u;

uint32_t intel_gtt_alloc(uint32_t size) {
    uint32_t gpa  = gtt_bump;
    uint32_t step = (size + 0x3ffffu) & ~0x3ffffu;   /* 256 KiB granule */
    if (step == 0)
        step = 0x40000u;
    gtt_bump += step;
    if (gtt_bump >= INTEL_SCANOUT_GPA + 0x4000000u)
        gtt_bump = INTEL_SCANOUT_GPA + 0x2000000u;
    return gpa;
}

/* Page flip: re-point the primary plane at a new GGTT address.  PLANE_SURF
 * self-arms and latches at the next vblank, so this is tearing-free without a
 * private vblank wait; PLANE_CTL is rewritten first per the arm order. */
int intel_modeset_flip(int pipe, uint32_t gpa, uint32_t width, uint32_t height,
                       uint32_t pitch, int cpp, int order_rgbx) {
    if (pitch % 64u)
        return -1;

    uint32_t fmt = (cpp == 4) ? PLANE_CTL_FORMAT_XRGB_8888 : PLANE_CTL_FORMAT_RGB_565;
    uint32_t ctl = PLANE_CTL_ENABLE | fmt | PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_TILED_LINEAR;
    if (order_rgbx)
        ctl |= PLANE_CTL_ORDER_RGBX;

    intel_write(PLANE_STRIDE(pipe, 0), pitch / 64u);
    intel_write(PLANE_SIZE(pipe, 0), ((height - 1u) << 16) | (width - 1u));
    intel_write(PLANE_CTL(pipe, 0), ctl);
    intel_write(PLANE_SURF(pipe, 0), gpa & 0xfffff000u);
    return 0;
}

/* ── display data buffer ─────────────────────────────────────────────────── */

static int dbuf_enable(void) {
    intel_write_mask(DBUF_CTL_S0, DBUF_POWER_REQUEST, DBUF_POWER_REQUEST);
    (void)intel_read(DBUF_CTL_S0);
    ktime_busy_wait_us(10);
    return wait_set(DBUF_CTL_S0, DBUF_POWER_STATE, 30, 1000);
}

/* ── DDI encoder ─────────────────────────────────────────────────────────── */

/* DDI buffer translation entry 9 is the HDMI slot; the values are the
 * _skl_trans_hdmi[] entry-8 defaults (intel_ddi_buf_trans.c). */
static void ddi_hdmi_buffers(int port) {
    intel_write(DDI_BUF_TRANS_LO(port, 9), 0x80003015u);
    intel_write(DDI_BUF_TRANS_HI(port, 9), 0x000000CDu);
}

/* I_boost for the default HDMI buffer entry is 1 (intel_ddi.c _skl_ddi_set_iboost). */
static void ddi_iboost(int port) {
    uint32_t v = intel_read(DISPIO_CR_TX_BMU_CR0);
    v &= ~(BALANCE_LEG_MASK(port) | BALANCE_LEG_DISABLE(port));
    v |= (1u << BALANCE_LEG_SHIFT(port));
    intel_write(DISPIO_CR_TX_BMU_CR0, v);
}

/* WA #1143 (skl,kbl,cfl): pulse the DDI training-override chicken bits for the
 * transcoder the port is wired to (port A->EDP, B->A, C->B, D->C, E->A). */
static void ddi_chicken(int port) {
    static const uint32_t reg_for_port[5] = {
        CHICKEN_TRANS_EDP, CHICKEN_TRANS_A, CHICKEN_TRANS_B, CHICKEN_TRANS_C, CHICKEN_TRANS_A
    };
    uint32_t reg = reg_for_port[port];
    uint32_t en  = (port == 4) ? DDIE_TRAINING_OVERRIDE_ENABLE : DDI_TRAINING_OVERRIDE_ENABLE;
    uint32_t val = (port == 4) ? DDIE_TRAINING_OVERRIDE_VALUE  : DDI_TRAINING_OVERRIDE_VALUE;
    intel_write_mask(reg, en | val, en | val);
    (void)intel_read(reg);
    ktime_busy_wait_us(1);
    intel_write_mask(reg, en | val, 0);
}

static void ddi_buf_disable(int port) {
    uint32_t v = intel_read(DDI_BUF_CTL(port));
    if (!(v & DDI_BUF_CTL_ENABLE))
        return;
    intel_write(DDI_BUF_CTL(port), v & ~DDI_BUF_CTL_ENABLE);
    (void)wait_set(DDI_BUF_CTL(port), DDI_BUF_IS_IDLE, 1000, 10);
}

static void ddi_buf_enable(int port) {
    intel_write(DDI_BUF_CTL(port), DDI_BUF_CTL_ENABLE | DDI_BUF_TRANS_SELECT(9));
    (void)intel_read(DDI_BUF_CTL(port));
    /* intel_wait_ddi_buf_active(): gen9 waits a fixed 518 us. */
    ktime_busy_wait_us(518);
}

/* ── pipe / plane ────────────────────────────────────────────────────────── */

static void pipe_timings(int t, const intel_crtc_mode_t *m) {
    intel_write(TRANS_HTOTAL(t), ((uint32_t)(m->htotal - 1) << 16) | (uint32_t)(m->hdisplay - 1));
    intel_write(TRANS_HBLANK(t), ((uint32_t)(m->htotal - 1) << 16) | (uint32_t)(m->hdisplay - 1));
    intel_write(TRANS_HSYNC(t), ((uint32_t)(m->hsync_end - 1) << 16) | (uint32_t)(m->hsync_start - 1));
    intel_write(TRANS_VTOTAL(t), ((uint32_t)(m->vtotal - 1) << 16) | (uint32_t)(m->vdisplay - 1));
    intel_write(TRANS_VBLANK(t), ((uint32_t)(m->vtotal - 1) << 16) | (uint32_t)(m->vdisplay - 1));
    intel_write(TRANS_VSYNC(t), ((uint32_t)(m->vsync_end - 1) << 16) | (uint32_t)(m->vsync_start - 1));
    intel_write(TRANS_VSYNCSHIFT(t), 0);
    intel_write(TRANS_MULT(t), 0);
    intel_write(PIPESRC(t), ((uint32_t)(m->hdisplay - 1) << 16) | (uint32_t)(m->vdisplay - 1));
}

/* Plane watermark and DDB allocation.  Gen9 has 8 WM levels; level-0 lines are
 * ignored by hardware and every enabled plane must enable the same level, so
 * only level 0 is enabled (WaDisableTWM also forces PLANE_WM_TRANS = 0). */
static void watermarks(const intel_crtc_mode_t *m) {
    int pipe = m->pipe;

    /* WM_LINETIME = ceil(htotal * 8 * 1000 / pixel_rate), clamped to 0x1FF. */
    uint32_t linetime = ((uint32_t)m->htotal * 8000u + m->pixel_clock_khz - 1) / m->pixel_clock_khz;
    if (linetime > HSW_LINETIME_MASK)
        linetime = HSW_LINETIME_MASK;
    intel_write(WM_LINETIME(pipe), linetime);

    /* Give the single plane the bulk of the 892-block data buffer. */
    intel_write(PLANE_BUF_CFG(pipe, 0),
                PLANE_BUF_END(INTEL_DDB_PLANE_END) | PLANE_BUF_START(0));
    intel_write(PLANE_NV12_BUF_CFG(pipe, 0), 0);

    /* blocks = ceil(2us * pixel_rate * cpp / 512000) + 1, floor 4. */
    uint32_t blocks = (2u * m->pixel_clock_khz * (uint32_t)m->cpp + 512000u - 1u) / 512000u + 1u;
    if (blocks < 4)
        blocks = 4;
    if (blocks > PLANE_WM_BLOCKS_MASK)
        blocks = PLANE_WM_BLOCKS_MASK;

    for (int level = 0; level < 8; level++)
        intel_write(PLANE_WM(pipe, 0, level), level == 0 ? (PLANE_WM_EN | blocks) : 0u);
    intel_write(PLANE_WM_TRANS(pipe, 0), 0);
}

static void plane_program(const intel_crtc_mode_t *m) {
    int pipe = m->pipe, plane = 0;
    uint32_t fmt = (m->cpp == 4) ? PLANE_CTL_FORMAT_XRGB_8888 : PLANE_CTL_FORMAT_RGB_565;
    uint32_t ctl = PLANE_CTL_ENABLE | fmt | PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_TILED_LINEAR;
    if (m->order_rgbx)
        ctl |= PLANE_CTL_ORDER_RGBX;

    intel_write(PLANE_STRIDE(pipe, plane), m->pitch / 64u);
    intel_write(PLANE_POS(pipe, plane), 0);
    intel_write(PLANE_SIZE(pipe, plane), ((uint32_t)(m->height - 1) << 16) | (uint32_t)(m->width - 1));
    intel_write(PLANE_KEYVAL(pipe, plane), 0);
    intel_write(PLANE_KEYMSK(pipe, plane), 0);
    intel_write(PLANE_KEYMAX(pipe, plane), 0);
    intel_write(PLANE_OFFSET(pipe, plane), 0);
    intel_write(PLANE_AUX_DIST(pipe, plane), 0);
    intel_write(PLANE_AUX_OFFSET(pipe, plane), 0);
    /* PLANE_CTL self-arms the update, so it goes immediately before PLANE_SURF. */
    intel_write(PLANE_CTL(pipe, plane), ctl);
    intel_write(PLANE_SURF(pipe, plane), m->gpa);
}

/* ── public ──────────────────────────────────────────────────────────────── */

/* ── CDCLK and WRPLL, computed and programmed from scratch ───────────────── */

#define WRPLL_REF_KHZ   24000   /* SKL/CFL SSC reference */

static uint32_t dpll_ctl_reg(int id) {
    return (id == 1) ? LCPLL2_CTL : (id == 2) ? WRPLL_CTL0 : WRPLL_CTL1;
}

/* CDCLK_CTL freq-sel -> kHz (SKL/CFL table; VCO 8.1 or 8.64 GHz). */
static uint32_t cdclk_current_khz(void) {
    static const uint32_t tbl[4] = { 450000u, 540000u, 337500u, 675000u };
    return tbl[(intel_read(CDCLK_CTL) & CDCLK_FREQ_SEL_MASK) >> 26];
}

/* Smallest table frequency >= min (skl_calc_cdclk, VCO 8.1 GHz). */
static uint32_t cdclk_pick_khz(uint32_t min_khz) {
    if (min_khz > 540000u) return 675000u;
    if (min_khz > 450000u) return 540000u;
    if (min_khz > 337500u) return 450000u;
    return 337500u;
}

/* Full SKL/CFL CDCLK change: PCODE prepare, the Wa #1183 DIVMUX dance, PCODE
 * complete.  Only called when the inherited clock is too low. */
static void cdclk_program(uint32_t target_khz) {
    uint32_t sel = (target_khz > 540000u) ? CDCLK_FREQ_675_617
                 : (target_khz > 450000u) ? CDCLK_FREQ_540
                 : (target_khz > 337500u) ? CDCLK_FREQ_450_432
                 :                          CDCLK_FREQ_337_308;
    uint32_t decimal = (target_khz > 1000u) ? (target_khz - 1000u) / 500u : 0u;
    uint32_t ctl;

    intel_write(GEN6_PCODE_DATA, 0);
    intel_write(GEN6_PCODE_MAILBOX, GEN6_PCODE_READY |
                GEN6_PCODE_MB_COMMAND(SKL_PCODE_CDCLK_CONTROL) |
                GEN6_PCODE_MB_PARAM1(SKL_CDCLK_PREPARE_FOR_CHANGE));
    (void)wait_set(GEN6_PCODE_MAILBOX, GEN6_PCODE_READY, 30, 100);

    ctl  = intel_read(CDCLK_CTL);
    ctl &= ~(CDCLK_FREQ_SEL_MASK | CDCLK_FREQ_DECIMAL_MASK);
    ctl |= sel | (decimal & CDCLK_FREQ_DECIMAL_MASK);

    intel_write(CDCLK_CTL, ctl);
    (void)intel_read(CDCLK_CTL);
    intel_write(CDCLK_CTL, ctl | CDCLK_DIVMUX_CD_OVERRIDE);
    (void)intel_read(CDCLK_CTL);
    intel_write(CDCLK_CTL, ctl & ~(CDCLK_FREQ_SEL_MASK | CDCLK_FREQ_DECIMAL_MASK));
    intel_write(CDCLK_CTL, ctl);
    intel_write(CDCLK_CTL, ctl & ~CDCLK_DIVMUX_CD_OVERRIDE);
    (void)intel_read(CDCLK_CTL);

    intel_write(GEN6_PCODE_DATA, 0);
    intel_write(GEN6_PCODE_MAILBOX, GEN6_PCODE_READY |
                GEN6_PCODE_MB_COMMAND(SKL_PCODE_CDCLK_CONTROL) |
                GEN6_PCODE_MB_PARAM1(SKL_CDCLK_READY_FOR_CHANGE));
    (void)wait_set(GEN6_PCODE_MAILBOX, GEN6_PCODE_READY, 30, 100);
}

/* CDCLK only has to clear the pixel rate (no scaling, single-wide pipe). */
static void cdclk_ensure(uint32_t min_khz) {
    uint32_t cur = cdclk_current_khz();
    if (cur >= min_khz) {
        printk("i915: CDCLK %u kHz already >= %u kHz — kept\n", cur, min_khz);
        return;
    }
    uint32_t target = cdclk_pick_khz(min_khz);
    printk("i915: CDCLK %u kHz < %u kHz — programming %u kHz\n", cur, min_khz, target);
    cdclk_program(target);
}

/* Divider -> p0/p1/p2 (skl_wrpll_get_multipliers). */
static void wrpll_mults(unsigned p, unsigned *p0, unsigned *p1, unsigned *p2) {
    if ((p & 1u) == 0) {
        unsigned half = p / 2;
        if (half == 1 || half == 2 || half == 3 || half == 5) { *p0 = 2; *p1 = 1; *p2 = half; }
        else if ((half & 1u) == 0) { *p0 = 2; *p1 = half / 2; *p2 = 2; }
        else if (half % 3u == 0)   { *p0 = 3; *p1 = half / 3; *p2 = 2; }
        else                       { *p0 = 7; *p1 = half / 7; *p2 = 2; }
    } else if (p == 3 || p == 9)   { *p0 = 3; *p1 = 1; *p2 = p / 3; }
    else if (p == 5 || p == 7)     { *p0 = p; *p1 = 1; *p2 = 1; }
    else if (p == 15)              { *p0 = 3; *p1 = 1; *p2 = 5; }
    else if (p == 21)              { *p0 = 7; *p1 = 1; *p2 = 3; }
    else                           { *p0 = 7; *p1 = 1; *p2 = 5; }
}

static int64_t i64_abs(int64_t v) { return v < 0 ? -v : v; }

static void wrpll_try(int64_t *min_dev, int64_t *best_central, unsigned *best_p,
                      int64_t central, int64_t dco, unsigned p) {
    int64_t dev = (10000 * i64_abs(dco - central)) / central;
    /* DCO must be within +1% / -6% of the central frequency. */
    if (dco >= central) {
        if (dev < 100 && dev < *min_dev) { *min_dev = dev; *best_central = central; *best_p = p; }
    } else if (dev < 600 && dev < *min_dev) {
        *min_dev = dev; *best_central = central; *best_p = p;
    }
}

/* WRPLL dividers for an HDMI pixel clock (skl_ddi_calculate_wrpll). */
static int wrpll_calc(uint32_t clock_khz, uint32_t *cfgcr1, uint32_t *cfgcr2) {
    static const int64_t central[3] = { 8400000000LL, 9000000000LL, 9600000000LL };
    static const uint8_t even_div[] = { 4,6,8,10,12,14,16,18,20,24,28,30,32,36,40,42,44,
                                        48,52,54,56,60,64,66,68,70,72,76,78,80,84,88,90,92,96,98 };
    static const uint8_t odd_div[] = { 3,5,7,9,15,21,35 };
    int64_t afe = (int64_t)clock_khz * 1000LL * 5LL;   /* AFE = 5x pixel clock */
    int64_t min_dev = 0x7fffffffffffffffLL, best_central = 0;
    unsigned p = 0;

    for (int d = 0; d < 2; d++) {
        const uint8_t *list = d ? odd_div : even_div;
        int n = d ? (int)sizeof(odd_div) : (int)sizeof(even_div);
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < n; i++) {
                wrpll_try(&min_dev, &best_central, &p, central[c], (int64_t)list[i] * afe, list[i]);
                if (min_dev == 0)
                    goto done;
            }
        }
done:
        /* Prefer an even divider when one is available. */
        if (d == 0 && p)
            break;
    }
    if (!p)
        return -1;

    unsigned p0, p1, p2;
    p0 = p1 = p2 = 0;
    wrpll_mults(p, &p0, &p1, &p2);

    uint32_t cf   = (best_central == 9600000000LL) ? 0u
                  : (best_central == 9000000000LL) ? 1u : 3u;
    uint32_t pdiv = (p0 == 1) ? 0u : (p0 == 2) ? 1u : (p0 == 3) ? 2u : 4u;
    uint32_t kdiv = (p2 == 5) ? 0u : (p2 == 2) ? 1u : (p2 == 3) ? 2u : 3u;

    int64_t dco     = (int64_t)p0 * p1 * p2 * afe;
    int64_t ref_hz  = (int64_t)WRPLL_REF_KHZ * 1000LL;
    int64_t dco_int = dco / ref_hz;
    int64_t t       = dco / (int64_t)(WRPLL_REF_KHZ / 1000);
    int64_t frac    = ((t - dco_int * 1000000LL) * 0x8000LL) / 1000000LL;

    *cfgcr2 = DPLL_CFGCR2_QDIV_RATIO(p1) | DPLL_CFGCR2_QDIV_MODE(p1 == 1 ? 0u : 1u) |
              DPLL_CFGCR2_KDIV(kdiv) | DPLL_CFGCR2_PDIV(pdiv) |
              (cf & DPLL_CFGCR2_CENTRAL_FREQ_MASK);
    *cfgcr1 = DPLL_CFGCR1_FREQ_ENABLE |
              DPLL_CFGCR1_DCO_FRACTION((uint32_t)frac) |
              DPLL_CFGCR1_DCO_INTEGER((uint32_t)dco_int);
    return 0;
}

/* WRPLL currently assigned to a port (DDI_CLK_SEL), defaulting to DPLL1. */
static int dpll_for_port(int port) {
    uint32_t v = intel_read(DPLL_CTRL2);
    uint32_t sel = (v & DPLL_CTRL2_DDI_CLK_SEL_MASK(port)) >> (port * 3 + 1);
    return sel ? (int)sel : 1;
}

/* Program the HDMI WRPLL and wait for lock. */
static int dpll_enable_hdmi(int id, uint32_t clock_khz) {
    uint32_t cfgcr1, cfgcr2;
    if (wrpll_calc(clock_khz, &cfgcr1, &cfgcr2) != 0) {
        printk("i915: no WRPLL divider for %u kHz\n", (unsigned)clock_khz);
        return -1;
    }

    intel_write_mask(DPLL_CTRL1,
        DPLL_CTRL1_OVERRIDE(id) | DPLL_CTRL1_HDMI_MODE(id) |
        DPLL_CTRL1_SSC(id) | DPLL_CTRL1_LINK_RATE_MASK(id),
        DPLL_CTRL1_OVERRIDE(id) | DPLL_CTRL1_HDMI_MODE(id));
    (void)intel_read(DPLL_CTRL1);
    intel_write(DPLL_CFGCR1(id), cfgcr1);
    intel_write(DPLL_CFGCR2(id), cfgcr2);
    (void)intel_read(DPLL_CFGCR1(id));
    (void)intel_read(DPLL_CFGCR2(id));
    intel_write_mask(dpll_ctl_reg(id), LCPLL_PLL_ENABLE, LCPLL_PLL_ENABLE);

    if (wait_set(DPLL_STATUS, DPLL_LOCK(id), 500, 10) != 0) {
        printk("i915: DPLL%d did not lock\n", id);
        return -1;
    }
    printk("i915: DPLL%d locked for %u kHz (cfgcr1 %08x cfgcr2 %08x)\n",
           id, (unsigned)clock_khz, (unsigned)cfgcr1, (unsigned)cfgcr2);
    return 0;
}

/* Program CDCLK and the WRPLL for a mode and route the clock to the port and
 * transcoder.  Used by both the console handoff and set_config(). */
static void modeset_clocks(int port, int trans, uint32_t pixel_clock_khz) {
    int dpll = dpll_for_port(port);
    cdclk_ensure(pixel_clock_khz);
    if (dpll_enable_hdmi(dpll, pixel_clock_khz) == 0) {
        intel_write_mask(DPLL_CTRL2,
            DPLL_CTRL2_DDI_CLK_OFF(port) | DPLL_CTRL2_DDI_CLK_SEL_MASK(port),
            DPLL_CTRL2_DDI_CLK_SEL((uint32_t)dpll, port) | DPLL_CTRL2_DDI_SEL_OVERRIDE(port));
        intel_write(TRANS_CLK_SEL(trans), TRANS_CLK_SEL_PORT((uint32_t)port));
    }
}

int intel_modeset_transcoder_for_port(int port) {
    for (int t = 0; t < 3; t++) {
        uint32_t v = intel_read(TRANS_DDI_FUNC_CTL(t));
        if ((v & TRANS_DDI_FUNC_ENABLE) &&
            (((v & TRANS_DDI_PORT_MASK) >> 28) == (uint32_t)port))
            return t;
    }
    return -1;
}

/* Pipe timings, transcoder and DDI encoder — everything except watermarks and
 * the plane.  The transcoder's enable bit is only set (never cleared in
 * between): firmware may hold bits in TRANSCONF that must survive. */
static int pipe_encoder_enable(const intel_crtc_mode_t *m) {
    int pipe = m->pipe, port = m->port, trans = m->pipe;

    if (dbuf_enable() != 0) {
        printk("i915: display data buffer did not power up\n");
        return -1;
    }

    /* Stop the encoder while we reprogram it. */
    ddi_buf_disable(port);

    pipe_timings(trans, m);

    uint32_t tdfc = TRANS_DDI_FUNC_ENABLE | TRANS_DDI_SELECT_PORT((uint32_t)port) |
                    TRANS_DDI_MODE_SELECT_HDMI | TRANS_DDI_BPC_8;
    if (m->hsync_positive)
        tdfc |= TRANS_DDI_PHSYNC;
    if (m->vsync_positive)
        tdfc |= TRANS_DDI_PVSYNC;
    intel_write(TRANS_DDI_FUNC_CTL(trans), tdfc);
    intel_write_mask(TRANSCONF(pipe), TRANSCONF_ENABLE, TRANSCONF_ENABLE);

    ddi_hdmi_buffers(port);
    ddi_iboost(port);
    ddi_chicken(port);
    ddi_buf_enable(port);
    return 0;
}

int intel_modeset_enable(const intel_crtc_mode_t *m) {
    if (m->pitch % 64u) {
        printk("i915: pitch %u is not 64-byte aligned — cannot scan out\n",
               (unsigned)m->pitch);
        return -1;
    }
    modeset_clocks(m->port, m->pipe, m->pixel_clock_khz);
    if (pipe_encoder_enable(m) != 0)
        return -1;
    watermarks(m);
    plane_program(m);
    return 0;
}

void intel_modeset_disable(int pipe) {
    intel_write(PLANE_CTL(pipe, 0), 0);
    intel_write(PLANE_SURF(pipe, 0), 0);
    intel_write_mask(TRANS_DDI_FUNC_CTL(pipe), TRANS_DDI_FUNC_ENABLE, 0);
    intel_write_mask(TRANSCONF(pipe), TRANSCONF_ENABLE, 0);
}

int intel_modeset_handoff_console(int port, uint32_t *w, uint32_t *h) {
    int trans = intel_modeset_transcoder_for_port(port);
    if (trans < 0) {
        printk("i915: no active transcoder on DDI-%c to take over\n", (char)('A' + port));
        return -1;
    }

    /* Keep the plane the firmware is already scanning with: its buffer is a
     * GGTT graphics address (the kernel's boot-fb "address" is an aperture
     * address, not physical memory, so it cannot be re-mapped as pages). */
    uint32_t fw_ctl    = intel_read(PLANE_CTL(trans, 0));
    uint32_t fw_stride = intel_read(PLANE_STRIDE(trans, 0));
    if (!(fw_ctl & PLANE_CTL_ENABLE) || fw_stride == 0) {
        printk("i915: firmware plane not up on pipe %d — leaving display alone\n", trans);
        return 1;
    }

    intel_crtc_mode_t m;
    memset(&m, 0, sizeof(m));
    m.port = port;
    m.pipe = trans;

    /* Recover the active mode from the transcoder's own timings (value-1). */
    uint32_t ht = intel_read(TRANS_HTOTAL(trans));
    uint32_t hs = intel_read(TRANS_HSYNC(trans));
    uint32_t vt = intel_read(TRANS_VTOTAL(trans));
    uint32_t vs = intel_read(TRANS_VSYNC(trans));
    m.hdisplay    = (uint16_t)((ht & 0xffffu) + 1u);
    m.htotal      = (uint16_t)(((ht >> 16) & 0xffffu) + 1u);
    m.hsync_start = (uint16_t)((hs & 0xffffu) + 1u);
    m.hsync_end   = (uint16_t)(((hs >> 16) & 0xffffu) + 1u);
    m.vdisplay    = (uint16_t)((vt & 0xffffu) + 1u);
    m.vtotal      = (uint16_t)(((vt >> 16) & 0xffffu) + 1u);
    m.vsync_start = (uint16_t)((vs & 0xffffu) + 1u);
    m.vsync_end   = (uint16_t)(((vs >> 16) & 0xffffu) + 1u);
    if (m.hdisplay == 0 || m.vdisplay == 0 || m.htotal < m.hdisplay || m.vtotal < m.vdisplay) {
        printk("i915: transcoder %d timings look unprogrammed\n", trans);
        return -1;
    }
    m.pixel_clock_khz = ((uint32_t)m.htotal * (uint32_t)m.vtotal * 60u) / 1000u;
    if (m.pixel_clock_khz == 0)
        m.pixel_clock_khz = 148500;
    uint32_t tdfc = intel_read(TRANS_DDI_FUNC_CTL(trans));
    m.hsync_positive = (tdfc & TRANS_DDI_PHSYNC) ? 1 : 0;
    m.vsync_positive = (tdfc & TRANS_DDI_PVSYNC) ? 1 : 0;

    /* Snapshot the working plane so it can be put back exactly. */
    uint32_t sv_pos    = intel_read(PLANE_POS(trans, 0));
    uint32_t sv_size   = intel_read(PLANE_SIZE(trans, 0));
    uint32_t sv_keyval = intel_read(PLANE_KEYVAL(trans, 0));
    uint32_t sv_keymsk = intel_read(PLANE_KEYMSK(trans, 0));
    uint32_t sv_keymax = intel_read(PLANE_KEYMAX(trans, 0));
    uint32_t sv_off    = intel_read(PLANE_OFFSET(trans, 0));
    uint32_t sv_auxd   = intel_read(PLANE_AUX_DIST(trans, 0));
    uint32_t sv_auxo   = intel_read(PLANE_AUX_OFFSET(trans, 0));
    uint32_t sv_surf   = intel_read(PLANE_SURF(trans, 0));
    uint32_t sv_nv12   = intel_read(PLANE_NV12_BUF_CFG(trans, 0));
    uint32_t sv_ddb    = intel_read(PLANE_BUF_CFG(trans, 0));
    uint32_t sv_wmt    = intel_read(PLANE_WM_TRANS(trans, 0));
    uint32_t sv_wm[8];
    for (int l = 0; l < 8; l++)
        sv_wm[l] = intel_read(PLANE_WM(trans, 0, l));

    /* Clocks from scratch: CDCLK must clear the pixel rate, then program the
     * HDMI WRPLL and route it to the port / transcoder. */
    modeset_clocks(port, trans, m.pixel_clock_khz);

    cpu_flush_caches();
    if (pipe_encoder_enable(&m) != 0)
        return -1;

    /* Put the working plane back, in arm order (PLANE_CTL right before SURF). */
    intel_write(PLANE_STRIDE(trans, 0), fw_stride);
    intel_write(PLANE_POS(trans, 0), sv_pos);
    intel_write(PLANE_SIZE(trans, 0), sv_size);
    for (int l = 0; l < 8; l++)
        intel_write(PLANE_WM(trans, 0, l), sv_wm[l]);
    intel_write(PLANE_WM_TRANS(trans, 0), sv_wmt);
    intel_write(PLANE_BUF_CFG(trans, 0), sv_ddb);
    intel_write(PLANE_NV12_BUF_CFG(trans, 0), sv_nv12);
    intel_write(PLANE_KEYVAL(trans, 0), sv_keyval);
    intel_write(PLANE_KEYMSK(trans, 0), sv_keymsk);
    intel_write(PLANE_KEYMAX(trans, 0), sv_keymax);
    intel_write(PLANE_OFFSET(trans, 0), sv_off);
    intel_write(PLANE_AUX_DIST(trans, 0), sv_auxd);
    intel_write(PLANE_AUX_OFFSET(trans, 0), sv_auxo);
    intel_write(PLANE_CTL(trans, 0), fw_ctl | PLANE_CTL_ENABLE);
    intel_write(PLANE_SURF(trans, 0), sv_surf & 0xfffff000u);

    printk("i915: took over pipe %d (DDI-%c), preserved plane surf %08x\n",
           trans, (char)('A' + port), (unsigned)(sv_surf & 0xfffff000u));

    if (w) *w = m.hdisplay;
    if (h) *h = m.vdisplay;
    return 0;
}
