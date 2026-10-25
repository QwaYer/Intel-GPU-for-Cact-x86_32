#ifndef INTEL_REGS_H
#define INTEL_REGS_H

/*
 * intel_regs.h — the subset of Intel Gen9.5 (Coffee Lake / "UHD Graphics 630")
 * display registers this driver needs, with the upstream Linux name kept
 * alongside each value so it can be re-checked against the i915 source
 * (drivers/gpu/drm/i915/display/intel_display_regs.h and its siblings).
 *
 * All offsets are absolute **MMIO offsets from the GTTMMADR BAR** (i.e. already
 * including the south/PCH display base 0xc0000 where relevant); the module maps
 * the BAR at `intel.mmio` and accesses `intel.mmio + offset`.
 */

/* ── identification / fuses ─────────────────────────────────────────────── */

/* SKL_DFSM — "display fuse strap": which pipes are fused off. */
#define SKL_DFSM                        0x51000
#define SKL_DFSM_PIPE_A_DISABLE         (1u << 30)
#define SKL_DFSM_PIPE_B_DISABLE         (1u << 21)
#define SKL_DFSM_PIPE_C_DISABLE         (1u << 28)
#define SKL_DFSM_DISPLAY_PM_DISABLE     (1u << 27)

/* CFL is a DISPLAY_VER 9 part (skl_display in the i915 platform tables): the
 * version is not read from a register, it is a property of the device id. */
#define INTEL_DISPLAY_VER_CFL           9

/* ── display power ──────────────────────────────────────────────────────── */

/* The four power-well request registers and the per-well request/state bits.
 * Well `n` lives at bit (2n+1) of each register; its hardware state is bit
 * 2n of the driver register. */
#define HSW_PWR_WELL_CTL1               0x45400  /* BIOS  request      */
#define HSW_PWR_WELL_CTL2               0x45404  /* DRIVER request + state */
#define HSW_PWR_WELL_CTL3               0x45408  /* KVMR  request      */
#define HSW_PWR_WELL_CTL4               0x4540c  /* DEBUG request      */
#define HSW_PWR_WELL_REQ(idx)           (0x2u << ((idx) * 2))
#define HSW_PWR_WELL_STATE(idx)         (0x1u << ((idx) * 2))

/* Well indices (SKL/CFL). */
#define SKL_PW_CTL_IDX_MISC_IO          0
#define SKL_PW_CTL_IDX_DDI_A_E          1
#define SKL_PW_CTL_IDX_DDI_B            2
#define SKL_PW_CTL_IDX_DDI_C            3
#define SKL_PW_CTL_IDX_DDI_D            4
#define SKL_PW_CTL_IDX_PW_1             14
#define SKL_PW_CTL_IDX_PW_2             15

/* Power-gate fuse distribution status (SKL_FUSE_STATUS). */
#define SKL_FUSE_STATUS                 0x42000
#define SKL_FUSE_PG_DIST_STATUS(pg)     (1u << (27 - (pg)))
#define SKL_PG0                         0
#define SKL_PG1                         1
#define SKL_PG2                         2

/* DC (display power saving) states — must be off before touching the display. */
#define DC_STATE_EN                     0x45504
#define DC_STATE_UPTO_DC5_DC6_MASK      0x3u
#define DC_STATE_EN_DC9                 (1u << 3)

/* South display clock gating (workaround #0868: GMBUS needs its clock). */
#define SOUTH_DSPCLK_GATE_D             0xc2020
#define PCH_GMBUSUNIT_CLOCK_GATE_DISABLE (1u << 31)

/* ── GMBUS / DDC ────────────────────────────────────────────────────────── */

/* GMBUS MMIO base on CFL is PCH_DISPLAY_BASE (0xc0000). */
#define GMBUS0                          0xc5100
#define GMBUS1                          0xc5104
#define GMBUS2                          0xc5108
#define GMBUS3                          0xc510c
#define GMBUS4                          0xc5110
#define GMBUS5                          0xc5120

/* GMBUS0: port/pin select (low 3 bits) and clock rate. */
#define GMBUS_RATE_100KHZ               (0u << 8)
#define GMBUS_RATE_400KHZ               (2u << 8)
#define GMBUS_HOLD_EXT                  (1u << 7)

/* GMBUS1 command/status. */
#define GMBUS_SW_CLR_INT                (1u << 31)
#define GMBUS_SW_RDY                    (1u << 30)
#define GMBUS_ENT                       (1u << 29)
#define GMBUS_CYCLE_NONE                (0u << 25)
#define GMBUS_CYCLE_WAIT                (1u << 25)
#define GMBUS_CYCLE_INDEX               (2u << 25)
#define GMBUS_CYCLE_STOP                (4u << 25)
#define GMBUS_BYTE_COUNT_SHIFT          16
#define GMBUS_SLAVE_INDEX_SHIFT         8
#define GMBUS_SLAVE_ADDR_SHIFT          1
#define GMBUS_SLAVE_READ                (1u << 0)
#define GMBUS_SLAVE_WRITE               (0u << 0)

/* GMBUS2 status. */
#define GMBUS_INUSE                     (1u << 15)
#define GMBUS_HW_WAIT_PHASE             (1u << 14)
#define GMBUS_STALL_TIMEOUT             (1u << 13)
#define GMBUS_INT                       (1u << 12)
#define GMBUS_HW_RDY                    (1u << 11)
#define GMBUS_SATOER                    (1u << 10)
#define GMBUS_ACTIVE                    (1u << 9)

/* GMBUS4 interrupt mask. */
#define GMBUS_HW_RDY_EN                 (1u << 0)
#define GMBUS_HW_WAIT_EN                (1u << 1)
#define GMBUS_IDLE_EN                   (1u << 2)

/* GMBUS5 2-byte index enable. */
#define GMBUS_2BYTE_INDEX_EN            (1u << 31)

/* Pin selects.  CNP (Coffee Lake) PCH exposes exactly these four; the name in
 * parentheses is the upstream / VBT name. */
#define GMBUS_PIN_1                     1   /* dpb  */
#define GMBUS_PIN_2                     2   /* dpc  */
#define GMBUS_PIN_3                     3   /* misc */
#define GMBUS_PIN_4                     4   /* dpd  */

#define GMBUS_BYTE_COUNT_MAX_GEN9       511 /* GEN9_GMBUS_BYTE_COUNT_MAX */

/* ── hotplug (PCH south display) ────────────────────────────────────────── */

#define SDEISR                          0xc4000
#define SDE_PORTA_HOTPLUG_SPT           (1u << 24)
#define SDE_PORTB_HOTPLUG_CPT           (1u << 21)
#define SDE_PORTC_HOTPLUG_CPT           (1u << 22)
#define SDE_PORTD_HOTPLUG_CPT           (1u << 23)
#define SDE_PORTE_HOTPLUG_SPT           (1u << 25)

/* ── DDI (port) ─────────────────────────────────────────────────────────── */

#define DDI_BUF_CTL_A                   0x64000
#define DDI_BUF_CTL_B                   0x64100
#define DDI_BUF_CTL(port)               (DDI_BUF_CTL_A + (port) * 0x100)
#define DDI_BUF_CTL_ENABLE              (1u << 31)
#define DDI_BUF_IS_IDLE                 (1u << 7)

/* ── DP AUX channel ─────────────────────────────────────────────────────── */

#define DP_AUX_CH_CTL_A                 0x64010
#define DP_AUX_CH_CTL(aux_ch)           (DP_AUX_CH_CTL_A + (aux_ch) * 0x100)
#define DP_AUX_CH_DATA_A(i)             (0x64014 + (i) * 4)
#define DP_AUX_CH_DATA(aux_ch, i)       (DP_AUX_CH_DATA_A(i) + (aux_ch) * 0x100)

#define DP_AUX_CH_CTL_SEND_BUSY         (1u << 31)
#define DP_AUX_CH_CTL_DONE              (1u << 30)
#define DP_AUX_CH_CTL_INTERRUPT         (1u << 29)
#define DP_AUX_CH_CTL_TIME_OUT_ERROR    (1u << 28)
#define DP_AUX_CH_CTL_TIME_OUT_MASK     (3u << 26)
#define DP_AUX_CH_CTL_TIME_OUT_MAX      (3u << 26)
#define DP_AUX_CH_CTL_RECEIVE_ERROR     (1u << 25)
#define DP_AUX_CH_CTL_MESSAGE_SIZE_MASK (0x1fu << 20)
#define DP_AUX_CH_CTL_MESSAGE_SIZE(x)   (((x) & 0x1fu) << 20)
#define DP_AUX_CH_CTL_FW_SYNC_PULSE_MASK (0x1fu << 5)
#define DP_AUX_CH_CTL_FW_SYNC_PULSE(c)  ((((c) - 1) & 0x1fu) << 5)
#define DP_AUX_CH_CTL_SYNC_PULSE_MASK   (0x1fu << 0)
#define DP_AUX_CH_CTL_SYNC_PULSE(c)     (((c) - 1) & 0x1fu)

/* DP AUX request codes (high nibble of the first header byte). */
#define DP_AUX_I2C_WRITE                0x0
#define DP_AUX_I2C_READ                 0x1
#define DP_AUX_I2C_MOT                  0x4
#define DP_AUX_NATIVE_WRITE             0x8
#define DP_AUX_NATIVE_READ              0x9

/* SKL: the AUX clock is derived from CDCLK by the hardware. */
#define INTEL_AUX_FW_SYNC_LEN           18  /* precharge 10 + preamble 8 */
#define INTEL_AUX_SYNC_LEN              32  /* precharge 16 + preamble 16 */

/* ══ stage 3: scanout (CDCLK / DPLL / DDI / pipe / plane / GGTT) ═══════════
 *
 * Offsets and field layouts are ported from Linux i915 display/...; see
 * STAGE3_LINUX_NOTES.md for the file:line behind each one.  These are raw MMIO
 * offsets from the GTTMMADR BAR — the CPU display blocks at 0x4xxxx/0x6xxxx/
 * 0x7xxxx need no extra 0xc0000 south/PCH base.
 */

/* GGTT translation entries: gen8+ GTTMMADR is 16 MiB with the GTT at the half
 * (BAR0 + 8 MiB).  One 64-bit PTE per 4 KiB page; gen9 PTE = phys | present,
 * with no cache/snoop bits. */
#define INTEL_GTT_PTE_BASE              0x800000
#define INTEL_GTT_PTE_PRESENT           0x1u

/* GTT TLB flush after writing PTEs. */
#define GFX_FLSH_CNTL_GEN6              0x101008
#define GFX_FLSH_CNTL_EN                (1u << 0)

/* Display data buffer slice — must be powered or the plane underruns. */
#define DBUF_CTL_S0                     0x45008
#define DBUF_POWER_REQUEST              (1u << 31)
#define DBUF_POWER_STATE                (1u << 30)

/* Pipe linetime watermark. */
#define WM_LINETIME(pipe)               (0x45270 + (pipe) * 4)
#define HSW_LINETIME_MASK               0x1ffu

/* CHICKEN_TRANS — DDI training override (WA #1143) and frame-start delay. */
#define CHICKEN_TRANS_A                 0x420c0
#define CHICKEN_TRANS_B                 0x420c4
#define CHICKEN_TRANS_C                 0x420c8
#define CHICKEN_TRANS_EDP               0x420cc
#define CHICKEN_TRANS_D                 0x420d8
#define DDI_TRAINING_OVERRIDE_ENABLE    (1u << 19)
#define DDI_TRAINING_OVERRIDE_VALUE     (1u << 18)
#define DDIE_TRAINING_OVERRIDE_ENABLE   (1u << 17)
#define DDIE_TRAINING_OVERRIDE_VALUE    (1u << 16)
#define HSW_FRAME_START_DELAY_MASK      (3u << 27)
#define HSW_FRAME_START_DELAY(x)        ((x) << 27)

/* CDCLK. */
#define CDCLK_CTL                       0x46000
#define CDCLK_FREQ_SEL_MASK             (3u << 26)
#define CDCLK_FREQ_450_432              (0u << 26)
#define CDCLK_FREQ_540                  (1u << 26)
#define CDCLK_FREQ_337_308              (2u << 26)
#define CDCLK_FREQ_675_617              (3u << 26)
#define CDCLK_DIVMUX_CD_OVERRIDE        (1u << 19)
#define CDCLK_FREQ_DECIMAL_MASK         0x7ffu

/* DPLL0 / LCPLL + WRPLL control registers (enable is always bit 31). */
#define LCPLL1_CTL                      0x46010
#define LCPLL2_CTL                      0x46014
#define LCPLL_PLL_ENABLE                (1u << 31)
#define LCPLL_PLL_LOCK                  (1u << 30)
#define WRPLL_CTL0                      0x46040
#define WRPLL_CTL1                      0x46060

/* Transcoder clock select — which port feeds a transcoder (+4 per transcoder). */
#define TRANS_CLK_SEL_A                 0x46140
#define TRANS_CLK_SEL_DISABLED          (0u << 29)
#define TRANS_CLK_SEL_PORT(x)           (((x) + 1u) << 29)
#define TRANS_CLK_SEL(trans)            (TRANS_CLK_SEL_A + (trans) * 4)

/* Per-pipe gamma mode (0 = 8-bit LUT off, what a plain modeset wants). */
#define GAMMA_MODE(pipe)                (0x4a480 + (pipe) * 0x800)

/* DPLL_CTRL1/2 and the per-id CFGCR pair. */
#define DPLL_CTRL1                      0x6c058
#define DPLL_CTRL1_OVERRIDE(id)         (1u << ((id) * 6))
#define DPLL_CTRL1_LINK_RATE_MASK(id)   (7u << ((id) * 6 + 1))
#define DPLL_CTRL1_HDMI_MODE(id)        (1u << ((id) * 6 + 5))
#define DPLL_CTRL1_SSC(id)              (1u << ((id) * 6 + 4))
#define DPLL_CTRL1_LINK_RATE_810        2u
#define DPLL_CTRL1_LINK_RATE_1080       4u
#define DPLL_CTRL2                      0x6c05c
#define DPLL_CTRL2_DDI_CLK_OFF(port)    (1u << ((port) + 15))
#define DPLL_CTRL2_DDI_CLK_SEL_MASK(port)  (3u << ((port) * 3 + 1))
#define DPLL_CTRL2_DDI_CLK_SEL(clk, port)  ((clk) << ((port) * 3 + 1))
#define DPLL_CTRL2_DDI_SEL_OVERRIDE(port)  (1u << ((port) * 3))
#define DPLL_STATUS                     0x6c060
#define DPLL_LOCK(id)                   (1u << ((id) * 8))

#define DPLL_CFGCR1(id)                 ((id) == 1 ? 0x6c040 : (id) == 2 ? 0x6c048 : 0x6c050)
#define DPLL_CFGCR2(id)                 ((id) == 1 ? 0x6c044 : (id) == 2 ? 0x6c04c : 0x6c054)
#define DPLL_CFGCR1_FREQ_ENABLE         (1u << 31)
#define DPLL_CFGCR1_DCO_INTEGER_MASK    0x1ffu
#define DPLL_CFGCR1_DCO_INTEGER(x)      ((x) & 0x1ffu)
#define DPLL_CFGCR1_DCO_FRACTION_MASK   (0x7fffu << 9)
#define DPLL_CFGCR1_DCO_FRACTION(x)     (((x) & 0x7fffu) << 9)
#define DPLL_CFGCR2_QDIV_RATIO_MASK     (0xffu << 8)
#define DPLL_CFGCR2_QDIV_RATIO(x)       (((x) & 0xffu) << 8)
#define DPLL_CFGCR2_QDIV_MODE(x)        (((x) & 1u) << 7)
#define DPLL_CFGCR2_KDIV_MASK           (3u << 5)
#define DPLL_CFGCR2_KDIV(x)             (((x) & 3u) << 5)
#define DPLL_CFGCR2_PDIV_MASK           (7u << 2)
#define DPLL_CFGCR2_PDIV(x)             (((x) & 7u) << 2)
#define DPLL_CFGCR2_CENTRAL_FREQ_MASK   3u

/* DDI buffer translation + I_boost (there is no gen9 combo PHY). */
#define DISPIO_CR_TX_BMU_CR0            0x6c00c
#define BALANCE_LEG_SHIFT(port)         (8 + (port) * 3)
#define BALANCE_LEG_MASK(port)          (7u << (8 + (port) * 3))
#define BALANCE_LEG_DISABLE(port)       (1u << (23 + (port)))
#define DDI_BUF_TRANS_BASE(port)        (0x64e00 + (port) * 0x60)
#define DDI_BUF_TRANS_LO(port, i)       (DDI_BUF_TRANS_BASE(port) + (i) * 8)
#define DDI_BUF_TRANS_HI(port, i)       (DDI_BUF_TRANS_BASE(port) + (i) * 8 + 4)
#define DDI_BUF_TRANS_SELECT(n)         ((n) << 24)

/* Transcoder timing registers (base 0x60000 + trans*0x1000).  Every start/end
 * field is encoded value-1. */
#define TRANS_HTOTAL(t)                 (0x60000 + (t) * 0x1000)
#define TRANS_HBLANK(t)                 (0x60004 + (t) * 0x1000)
#define TRANS_HSYNC(t)                  (0x60008 + (t) * 0x1000)
#define TRANS_VTOTAL(t)                 (0x6000c + (t) * 0x1000)
#define TRANS_VBLANK(t)                 (0x60010 + (t) * 0x1000)
#define TRANS_VSYNC(t)                  (0x60014 + (t) * 0x1000)
#define PIPESRC(t)                      (0x6001c + (t) * 0x1000)
#define TRANS_VSYNCSHIFT(t)             (0x60028 + (t) * 0x1000)
#define TRANS_MULT(t)                   (0x6002c + (t) * 0x1000)

/* TRANSCONF (PIPECONF) lives at the PIPE base. */
#define TRANSCONF(pipe)                 (0x70008 + (pipe) * 0x1000)
#define TRANSCONF_ENABLE                (1u << 31)
#define PIPE_MISC(pipe)                 (0x70030 + (pipe) * 0x1000)
#define SKL_BOTTOM_COLOR(pipe)          (0x70034 + (pipe) * 0x1000)

/* TRANS_DDI_FUNC_CTL (base 0x60400 + trans*0x1000). */
#define TRANS_DDI_FUNC_CTL(t)           (0x60400 + (t) * 0x1000)
#define TRANS_DDI_FUNC_ENABLE           (1u << 31)
#define TRANS_DDI_SELECT_PORT(port)     ((port) << 28)
#define TRANS_DDI_PORT_MASK             (7u << 28)
#define TRANS_DDI_MODE_SELECT_MASK      (7u << 24)
#define TRANS_DDI_MODE_SELECT_HDMI      (0u << 24)
#define TRANS_DDI_MODE_SELECT_DVI       (1u << 24)
#define TRANS_DDI_MODE_SELECT_DP_SST    (2u << 24)
#define TRANS_DDI_BPC_MASK              (7u << 20)
#define TRANS_DDI_BPC_8                 (0u << 20)
#define TRANS_DDI_PVSYNC                (1u << 17)
#define TRANS_DDI_PHSYNC                (1u << 16)

/* Primary plane: base for pipe A / plane 1, +pipe*0x1000 +plane*0x100. */
#define PLANE_CTL(pipe, plane)          (0x70180 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_STRIDE(pipe, plane)       (0x70188 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_POS(pipe, plane)          (0x7018c + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_SIZE(pipe, plane)         (0x70190 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_KEYVAL(pipe, plane)       (0x70194 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_KEYMSK(pipe, plane)       (0x70198 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_SURF(pipe, plane)         (0x7019c + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_KEYMAX(pipe, plane)       (0x701a0 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_OFFSET(pipe, plane)       (0x701a4 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_AUX_DIST(pipe, plane)     (0x701c0 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_AUX_OFFSET(pipe, plane)   (0x701c4 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_WM(pipe, plane, level)    (0x70240 + (pipe) * 0x1000 + (plane) * 0x100 + (level) * 4)
#define PLANE_WM_TRANS(pipe, plane)     (0x70268 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_NV12_BUF_CFG(pipe, plane) (0x70278 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_BUF_CFG(pipe, plane)      (0x7027c + (pipe) * 0x1000 + (plane) * 0x100)

#define PLANE_CTL_ENABLE                (1u << 31)
#define PLANE_CTL_PIPE_GAMMA_ENABLE     (1u << 30)
#define PLANE_CTL_FORMAT_MASK           (0xfu << 24)
#define PLANE_CTL_FORMAT_XRGB_8888      (4u << 24)
#define PLANE_CTL_FORMAT_RGB_565        (14u << 24)
#define PLANE_CTL_ORDER_RGBX            (1u << 20)
#define PLANE_CTL_PLANE_GAMMA_DISABLE   (1u << 13)
#define PLANE_CTL_TILED_LINEAR          (0u << 10)
#define PLANE_WM_EN                     (1u << 31)
#define PLANE_WM_BLOCKS_MASK            0x1fffu
#define PLANE_WM_LINES_MASK             (0x1fffu << 14)
#define PLANE_BUF_END(v)                (((v) - 1u) << 16)
#define PLANE_BUF_START(v)              ((v) & 0xfffu)

/* gen9 total DDB: 892 blocks.  Split it the way firmware does: plane 0 gets
 * 0..446, plane 1 the rest, so a single-plane modeset never overlaps plane 1's
 * permanent allocation (an overlap makes the hardware drop the watermarks). */
#define INTEL_DDB_TOTAL                 892u
#define INTEL_DDB_PLANE_END             446u

/* ── PCODE mailbox (host bridge) — CDCLK changes go through it ───────────── */

#define GEN6_PCODE_MAILBOX              0x138124
#define GEN6_PCODE_READY                (1u << 31)
#define GEN6_PCODE_MB_PARAM1(x)         ((((uint32_t)(x)) & 0xffu) << 8)
#define GEN6_PCODE_MB_PARAM2(x)         ((((uint32_t)(x)) & 0xffu) << 16)
#define GEN6_PCODE_MB_COMMAND(x)        (((uint32_t)(x)) & 0xffu)
#define GEN6_PCODE_DATA                 0x138128

#define SKL_PCODE_CDCLK_CONTROL         0x7u
#define SKL_CDCLK_PREPARE_FOR_CHANGE    0x3u
#define SKL_CDCLK_READY_FOR_CHANGE      0x1u

/* ══ stage 4: vblank interrupts / page flip / hardware cursor ══════════════ */

/* Master display interrupt (gen8 style; gen9 uses this, not gen11's). */
#define GEN8_MASTER_IRQ                 0x44200
#define GEN8_MASTER_IRQ_CONTROL         (1u << 31)
#define GEN8_DE_PIPE_IRQ(pipe)          (1u << (16 + (pipe)))

/* Per-pipe display interrupt registers (pipe A: 0x44400; +0x100 per pipe).
 * GEN8_PIPE_A_ISR 0x44400, IMR 0x44404, IIR 0x44408, IER 0x4440c. */
#define GEN8_DE_PIPE_ISR(pipe)          (0x44400 + (pipe) * 0x100)
#define GEN8_DE_PIPE_IMR(pipe)          (0x44404 + (pipe) * 0x100)
#define GEN8_DE_PIPE_IIR(pipe)          (0x44408 + (pipe) * 0x100)
#define GEN8_DE_PIPE_IER(pipe)          (0x4440c + (pipe) * 0x100)
#define GEN8_PIPE_VBLANK                (1u << 0)
#define GEN8_PIPE_VSYNC                 (1u << 1)
#define GEN9_PIPE_PLANE1_FLIP_DONE      (1u << 3)
#define GEN9_PIPE_PLANE2_FAULT          (1u << 8)
#define GEN9_PIPE_CURSOR_FAULT          (1u << 11)
#define GEN8_PIPE_FIFO_UNDERRUN         (1u << 31)

/* Pipe scanline (vblank timestamping / pipe-stuck check). */
#define PIPEDSL(pipe)                   (0x70000 + (pipe) * 0x1000)
#define PIPEDSL_LINE_MASK               0xfffffu

/* Hardware cursor (SKL universal, +0x1000 per pipe). */
#define CUR_CTL(pipe)                   (0x70080 + (pipe) * 0x1000)
#define CUR_BASE(pipe)                  (0x70084 + (pipe) * 0x1000)
#define CUR_POS(pipe)                   (0x70088 + (pipe) * 0x1000)
#define CUR_FBC_CTL(pipe)               (0x700a0 + (pipe) * 0x1000)
#define CUR_SURFLIVE(pipe)              (0x700ac + (pipe) * 0x1000)
#define CUR_WM(pipe, level)             (0x70140 + (pipe) * 0x1000 + (level) * 4)
#define CUR_WM_TRANS(pipe)              (0x70168 + (pipe) * 0x1000)
#define CUR_BUF_CFG(pipe)               (0x7017c + (pipe) * 0x1000)

#define MCURSOR_MODE_MASK               0x27u
#define MCURSOR_MODE_DISABLE            0x00u
#define MCURSOR_MODE_64_ARGB_AX         0x27u
#define MCURSOR_MODE_128_ARGB_AX        0x22u
#define MCURSOR_MODE_256_ARGB_AX        0x23u
#define MCURSOR_PIPE_GAMMA_ENABLE       (1u << 26)
#define MCURSOR_PIPE_CSC_ENABLE         (1u << 24)

#define CURSOR_POS_X_MASK               0x7fffu
#define CURSOR_POS_X_SIGN               (1u << 15)
#define CURSOR_POS_Y_MASK               (0x7fffu << 16)
#define CURSOR_POS_Y_SIGN               (1u << 31)

#define CUR_FBC_EN                      (1u << 31)
#define CUR_FBC_HEIGHT(h)               (((h) - 1u) & 0xffu)

#define CUR_WM_EN                       (1u << 31)
#define CUR_WM_BLOCKS_MASK              0xfffu
#define CUR_BUF_END(v)                  ((((v) - 1u) & 0xfffu) << 16)
#define CUR_BUF_START(v)                ((v) & 0xfffu)

#endif /* INTEL_REGS_H */
