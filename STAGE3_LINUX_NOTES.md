# Stage 3 — Programming one gen9.5 (SKL/CFL, DISPLAY_VER 9) display output from scratch

Reference derived from the on-disk Linux tree:

```
/home/qwayer/Projects/linux import/linux/linux-master/
  VERSION=7 PATCHLEVEL=3 SUBLEVEL=0 EXTRAVERSION=-rc4
  drivers/gpu/drm/i915/          (paths below are relative to this)
```

Target: Intel Gen9.5 "UHD Graphics 630" (Coffee Lake), `DISPLAY_VER == 9`, CPU
display only, one output, **no firmware help assumed**. Everything here is
CPU-side MMIO unless stated otherwise.

## Conventions / address map (READ FIRST)

* All offsets below are **raw 32-bit MMIO offsets from the GTTMMADR BAR (PCI BAR0)**.
  On gen9 the CPU display register blocks live at `0x4xxxx`, `0x6xxxx`, `0x7xxxx`
  from that BAR and **need no extra `0xc0000` "south"/PCH base**.
  (`0xc0000`-based PCH registers, e.g. `PCH_DPLL_A 0xc7000`, are on the separate
  PCH MMIO BAR and are *not* used by the CPU-side HDMI/eDP path here.)
* Gen9 has **no combo-PHY and no per-port PLL**. The port clock comes from the
  shared WRPLL / LCPLL blocks; the "port" side is `DDI_BUF_TRANS` + `DDI_BUF_CTL`.
* Pipe index: `PIPE_A=0, PIPE_B=1, PIPE_C=2` (`display/intel_display_limits.h:14`).
* Plane id: `PLANE_1=0`(primary), `PLANE_2=1`(sprite0), `PLANE_3=2`(sprite1),
  `PLANE_CURSOR` (`display/intel_display_limits.h:73`).
* Port: `PORT_A=0, PORT_B=1, PORT_C=2, PORT_D=3, PORT_E=4`
  (`display/intel_display_limits.h:94`).
* `_PICK_EVEN(i,a,b) = a + i*(b-a)` (`include/drm/intel/pick.h:13`); `_PORT`,
  `_PIPE`, `_PLANE` all use it, so 2-entry macros extrapolate linearly.
* Units: Linux uses kHz for clocks; registers use raw encodings noted per item.

### Prerequisites (must be up before any of the below)

**Display power wells.** Gen9 power wells are driven through
`HSW_PWR_WELL_CTL2` (driver request register), `+0x4` per control bank:

```
display/intel_display_regs.h:2374  HSW_PWR_WELL_CTL1  0x45400  (BIOS)
display/intel_display_regs.h:2375  HSW_PWR_WELL_CTL2  0x45404  (driver)
display/intel_display_regs.h:2376  HSW_PWR_WELL_CTL3  0x45408  (KVMR)
display/intel_display_regs.h:2377  HSW_PWR_WELL_CTL4  0x4540C  (debug)
display/intel_display_regs.h:2378  HSW_PWR_WELL_CTL_REQ(idx)   = 0x2 << (idx*2)
display/intel_display_regs.h:2379  HSW_PWR_WELL_CTL_STATE(idx) = 0x1 << (idx*2)
```

Gen9 power-well indices (`display/intel_display_regs.h:2385`):

```
SKL_PW_CTL_IDX_PW_2      15      (main display power well; contains CDCLK etc.)
SKL_PW_CTL_IDX_PW_1      14
SKL_PW_CTL_IDX_DDI_D      4
SKL_PW_CTL_IDX_DDI_C      3
SKL_PW_CTL_IDX_DDI_B      2
SKL_PW_CTL_IDX_DDI_A_E    1      (DDI A and E share)
SKL_PW_CTL_IDX_MISC_IO    0
```

Enable sequence per well: set `HSW_PWR_WELL_CTL_REQ(idx)` in CTL2, poll the
matching `HSW_PWR_WELL_CTL_STATE(idx)` (or the `_STATE` bit in the same register
per the driver's `hsw_power_well_enable`, `intel_display_power_well.c:454`).
Before HDMI on port X you must power `DDI_X` and `PW_2`.

**DBUF slices.** The display data buffer must be powered or the plane will
underrun. `DBUF_CTL_S(slice)`:

```
display/skl_watermark_regs.h:58   DBUF_POWER_REQUEST  bit 31
display/skl_watermark_regs.h:59   DBUF_POWER_STATE    bit 30
display/skl_watermark_regs.h:      DBUF_CTL_S(slice)  0x45008 + slice*4 (S0..S3)
```

Gen9 uses slice `DBUF_S1` which maps to S0 = **0x45008**
(`intel_display_power.h:330`, `skl_watermark_regs.h`). Procedure
(`intel_display_power.c`, `gen9_dbuf_slice_set`):

```c
intel_de_rmw(display, DBUF_CTL_S(slice), DBUF_POWER_REQUEST, DBUF_POWER_REQUEST);
intel_de_posting_read(display, DBUF_CTL_S(slice));
udelay(10);
/* then poll DBUF_POWER_STATE (bit 30) */
```

**PUNIT mailbox** (needed by CDCLK and by watermark latency read):

```
include/drm/intel/intel_pcode_regs.h:6   GEN6_PCODE_MAILBOX 0x138124
  GEN6_PCODE_READY        bit 31
  GEN6_PCODE_MB_PARAM2    bits 23:16
  GEN6_PCODE_MB_PARAM1    bits 15:8
  GEN6_PCODE_MB_COMMAND   bits 7:0
  GEN6_PCODE_SUCCESS      0x0
i915_reg.h:700                           GEN6_PCODE_DATA    0x138128
i915_reg.h:703                           GEN6_PCODE_DATA1   0x13812C
SKL_PCODE_CDCLK_CONTROL            0x7   (intel_pcode_regs.h:35)
  SKL_CDCLK_PREPARE_FOR_CHANGE     0x3
  SKL_CDCLK_READY_FOR_CHANGE       0x1
GEN9_PCODE_READ_MEM_LATENCY        0x6   (intel_pcode_regs.h:30)
```

---

## 1. CDCLK

### 1.1 Registers

```
display/intel_display_regs.h:2774  CDCLK_CTL                  0x46000
  CDCLK_FREQ_SEL_MASK      bits 27:26   (skl)
      CDCLK_FREQ_450_432   0 << 26
      CDCLK_FREQ_540       1 << 26
      CDCLK_FREQ_337_308   2 << 26
      CDCLK_FREQ_675_617   3 << 26
  CDCLK_DIVMUX_CD_OVERRIDE bit 19       (pre-icl; Wa #1183)
  CDCLK_FREQ_DECIMAL_MASK  bits 10:0    (pre-lnl)
```

`CDCLK_SQUASH_CTL` (0x46008) and the BXT `CD2X_DIV_SEL`/`CD2X_PIPE` fields
(bits 23:22 / 21:20) are **not used on SKL/CFL** — those are BXT/GLK plus
squash-only features. Gen9 SKL/CFL use the plain `CDCLK_CTL` + DPLL0 scheme.

DPLL0 is the CDCLK source and is enabled through:

```
display/intel_display_regs.h:2753  LCPLL1_CTL          0x46010
  LCPLL_PLL_ENABLE      bit 31
  LCPLL_PLL_LOCK        bit 30
display/intel_display_regs.h:2812  DPLL_CTRL1          0x6C058
  per-DPLL field layout at bit (id*6):
    DPLL_CTRL1_HDMI_MODE(id)  bit id*6+5
    DPLL_CTRL1_SSC(id)        bit id*6+4
    DPLL_CTRL1_LINK_RATE_MASK bits id*6+3:id*6+1  (3 bits)
    DPLL_CTRL1_OVERRIDE(id)   bit id*6
    LINK_RATE encodings: 2700=0, 1350=1, 810=2, 1620=3, 1080=4, 2160=5
SKL_DPLL0 = 0   (display/intel_dpll_mgr.h, enum DPLL_ID_SKL_DPLL0 = 0)
```

### 1.2 Frequency selection for 1920x1080@60

CDCLK only has to be `>= min_cdclk(pipe)`. For gen9:

```c
/* display/intel_cdclk.c:2913 */
int intel_cdclk_ppc(display, double_wide) { return HAS_2PPC || double_wide ? 2 : 1; }
/* display/intel_cdclk.c:2919 */
static int intel_cdclk_guardband(display) {
    if (DISPLAY_VER >= 9 || broadwell || haswell) return 100;  /* gen9: 100 */
    ...
}
/* display/intel_cdclk.c:2930 */
static int _intel_pixel_rate_to_cdclk(crtc_state, pixel_rate) {
    int ppc = intel_cdclk_ppc(display, crtc_state->double_wide);
    int guardband = intel_cdclk_guardband(display);
    return DIV_ROUND_UP(pixel_rate * 100, guardband * ppc);
}
```

So for a non-double-wide, non-scaled pipe: **`min_cdclk = pixel_rate`**.
1920x1080@60 = 148.5 MHz pixel clock → min_cdclk = 148500 kHz. The lowest SKL
CDCLK values already clear this. The actual pick:

```c
/* display/intel_cdclk.c:930 */
static int skl_calc_cdclk(int min_cdclk, int vco)
{
    if (vco == 8640000) {
        if (min_cdclk > 540000) return 617143;
        else if (min_cdclk > 432000) return 540000;
        else if (min_cdclk > 308571) return 432000;
        else return 308571;
    } else {                     /* vco == 8100000  -- power-on default */
        if (min_cdclk > 540000) return 675000;
        else if (min_cdclk > 450000) return 540000;
        else if (min_cdclk > 337500) return 450000;
        else return 337500;
    }
}
/* display/intel_cdclk.c:953 */
static u8 skl_calc_voltage_level(int cdclk) {
    if (cdclk > 540000) return 3;
    else if (cdclk > 450000) return 2;
    else if (cdclk > 337500) return 1;
    else return 0;
}
```

Preferred VCO defaults to 8100000 (set in `skl_cdclk_init_hw`,
`intel_cdclk.c:1291`). With 1080p60 → `skl_calc_cdclk(148500, 8100000) =
337500`, `voltage_level = 0`.

Freq-sel / decimal encoding:

```c
/* display/intel_cdclk.c:1135 */
static u32 skl_cdclk_freq_sel(display, cdclk, vco) {
	switch (cdclk) {
	case 308571: case 337500: return CDCLK_FREQ_337_308;   /* 2<<26 */
	case 450000: case 432000: return CDCLK_FREQ_450_432;   /* 0<<26 */
	case 540000:              return CDCLK_FREQ_540;       /* 1<<26 */
	case 617143: case 675000: return CDCLK_FREQ_675_617;   /* 3<<26 */
	}
	/* default: bypass */
}
/* display/intel_cdclk.c:1068  "convert from kHz to .1 fixpoint MHz with -1MHz offset" */
static int skl_cdclk_decimal(int cdclk) { return DIV_ROUND_CLOSEST(cdclk - 1000, 500); }
```

So the DECIMAL field is `round((cdclk_kHz - 1000) / 500)` (units of 0.5 MHz
with 1 MHz offset). For 337.5 MHz → `(337500-1000)/500 = 673 = 0x2A1`.
`CDCLK_CTL` value for 1080p60 @VCO 8100 = `337500`: `(2<<26) | 673` = `0x080002A1`.

### 1.3 PLL enable / change sequence (Wa #1183: skl,kbl,cfl)

`skl_dpll0_enable` (`display/intel_cdclk.c:1093`):

```c
intel_de_rmw(display, DPLL_CTRL1,
	     DPLL_CTRL1_HDMI_MODE(SKL_DPLL0) |
	     DPLL_CTRL1_SSC(SKL_DPLL0) |
	     DPLL_CTRL1_LINK_RATE_MASK(SKL_DPLL0),
	     DPLL_CTRL1_OVERRIDE(SKL_DPLL0) |
	     skl_dpll0_link_rate(display, vco));
intel_de_posting_read(display, DPLL_CTRL1);

intel_de_rmw(display, LCPLL1_CTL, 0, LCPLL_PLL_ENABLE);

if (intel_de_wait_for_set_ms(display, LCPLL1_CTL, LCPLL_PLL_LOCK, 5))
	drm_err(display->drm, "DPLL0 not locked\n");
```

`skl_dpll0_link_rate()` (`intel_cdclk.c:1078`): VCO 8640000 → link rate
`DPLL_CTRL1_LINK_RATE_1080` (=4); otherwise `DPLL_CTRL1_LINK_RATE_810` (=2).
DPLL0 is **always** enabled while the display is up (it is also the CDCLK
source).

Full CDCLK change (`skl_set_cdclk`, `display/intel_cdclk.c:1158`) — this is the
exact ordered sequence:

```c
/* inform PCU */
intel_parent_pcode_request(display, SKL_PCODE_CDCLK_CONTROL,
			   SKL_CDCLK_PREPARE_FOR_CHANGE,
			   SKL_CDCLK_READY_FOR_CHANGE,
			   SKL_CDCLK_READY_FOR_CHANGE, 3);   /* 3 ms timeout */

freq_select = skl_cdclk_freq_sel(display, cdclk, vco);

if (display->cdclk.hw.vco != 0 && display->cdclk.hw.vco != vco)
	skl_dpll0_disable(display);

cdclk_ctl = intel_de_read(display, CDCLK_CTL);

if (display->cdclk.hw.vco != vco) {
	/* Wa Display #1183 */
	cdclk_ctl &= ~(CDCLK_FREQ_SEL_MASK | CDCLK_FREQ_DECIMAL_MASK);
	cdclk_ctl |= freq_select | skl_cdclk_decimal(cdclk);
	intel_de_write(display, CDCLK_CTL, cdclk_ctl);
}

/* Wa Display #1183 */
cdclk_ctl |= CDCLK_DIVMUX_CD_OVERRIDE;          /* bit 19 */
intel_de_write(display, CDCLK_CTL, cdclk_ctl);
intel_de_posting_read(display, CDCLK_CTL);

if (display->cdclk.hw.vco != vco)
	skl_dpll0_enable(display, vco);

/* Wa Display #1183 */
cdclk_ctl &= ~(CDCLK_FREQ_SEL_MASK | CDCLK_FREQ_DECIMAL_MASK);
intel_de_write(display, CDCLK_CTL, cdclk_ctl);

cdclk_ctl |= freq_select | skl_cdclk_decimal(cdclk);
intel_de_write(display, CDCLK_CTL, cdclk_ctl);

/* Wa Display #1183 */
cdclk_ctl &= ~CDCLK_DIVMUX_CD_OVERRIDE;
intel_de_write(display, CDCLK_CTL, cdclk_ctl);
intel_de_posting_read(display, CDCLK_CTL);

/* inform PCU of the change (voltage level) */
intel_parent_pcode_write(display, SKL_PCODE_CDCLK_CONTROL,
			 cdclk_config->voltage_level);

intel_update_cdclk(display);
```

`skl_dpll0_disable()` (`intel_cdclk.c:1113`): `rmw(LCPLL1_CTL,
LCPLL_PLL_ENABLE, 0)`, then wait-for-clear `LCPLL_PLL_LOCK` 1 ms.

**Waits/polls summary (CDCLK):**
1. PCODE request completion up to **3 ms**.
2. `LCPLL_PLL_LOCK` set within **5 ms** after enabling DPLL0.
3. `LCPLL_PLL_LOCK` clear within **1 ms** after disabling DPLL0.

**Max CDCLK** is read from `SKL_DFSM` (0x51000, bits 23:22:
`SKL_DFSM_CDCLK_LIMIT_MASK`, 0=675,1=540,2=450,3=337.5 —
`display/intel_display_regs.h:1753`), then run through `skl_calc_cdclk`
(`intel_cdclk.c:3765`). Do the equivalent: pick CDCLK = largest table value
`<= max_cdclk` that is `>= min_cdclk`.

---

## 2. DPLL (WRPLL for HDMI; LCPLL/DPLL0 for eDP)

### 2.1 Registers

```
display/intel_display_regs.h:2812  DPLL_CTRL1            0x6C058
display/intel_display_regs.h:2827  DPLL_CTRL2            0x6C05C
  DPLL_CTRL2_DDI_CLK_OFF(port)     = 1 << (port + 15)
  DPLL_CTRL2_DDI_CLK_SEL_MASK(p)   = 3 << (p*3 + 1)
  DPLL_CTRL2_DDI_CLK_SEL_SHIFT(p)  = p*3 + 1
  DPLL_CTRL2_DDI_CLK_SEL(clk,p)    = (clk) << (p*3 + 1)
  DPLL_CTRL2_DDI_SEL_OVERRIDE(p)   = 1 << (p*3)
display/intel_display_regs.h:2835  DPLL_STATUS           0x6C060
  DPLL_LOCK(id) = 1 << (id*8)
```

WRPLL / LCPLL control registers (`display/intel_dpll_mgr.c:1355`,
`skl_dpll_regs[]`), enable bit is always **bit 31** (`LCPLL_PLL_ENABLE`):

```
DPLL0  ctl = LCPLL1_CTL   0x46010   (no HDMI mode; drives CDCLK + eDP)
DPLL1  ctl = LCPLL2_CTL   0x46014   cfgcr1 0x6C040  cfgcr2 0x6C044
DPLL2  ctl = WRPLL_CTL(0) 0x46040   cfgcr1 0x6C048  cfgcr2 0x6C04C
DPLL3  ctl = WRPLL_CTL(1) 0x46060   cfgcr1 0x6C050  cfgcr2 0x6C054

display/intel_display_regs.h:2842
DPLL_CFGCR1(id):  DPLL_CFGCR1_FREQ_ENABLE      bit 31
                  DPLL_CFGCR1_DCO_FRACTION_MASK (0x7fff << 9)
                  DPLL_CFGCR1_DCO_FRACTION(x)   ((x) << 9)
                  DPLL_CFGCR1_DCO_INTEGER_MASK  0x1ff   (bits 8:0)
DPLL_CFGCR2(id), display/intel_display_regs.h:2851
                  DPLL_CFGCR2_QDIV_RATIO_MASK (0xff << 8)
                  DPLL_CFGCR2_QDIV_RATIO(x)   ((x) << 8)
                  DPLL_CFGCR2_QDIV_MODE(x)    ((x) << 7)
                  DPLL_CFGCR2_KDIV_MASK       (3 << 5)  5=0,2=1,3=2,1=3
                  DPLL_CFGCR2_PDIV_MASK       (7 << 2)  1=0,2=1,3=2,7=4,7inv=5
                  DPLL_CFGCR2_CENTRAL_FREQ_MASK 3        (bits 1:0) 9.0G=1,8.4G=3
```

### 2.2 How a DPLL is assigned to a DDI/port

There is no "PER_PLL" selection register on gen9 — the port's clock mux is
`DPLL_CTRL2[DDI_CLK_SEL]`, written by `skl_ddi_enable_clock`
(`display/intel_ddi.c:1964`):

```c
static void skl_ddi_enable_clock(struct intel_encoder *encoder,
				 const struct intel_crtc_state *crtc_state)
{
	const struct intel_dpll *pll = crtc_state->intel_dpll;
	enum port port = encoder->port;

	intel_de_rmw(display, DPLL_CTRL2,
		     DPLL_CTRL2_DDI_CLK_OFF(port) |
		     DPLL_CTRL2_DDI_CLK_SEL_MASK(port),
		     DPLL_CTRL2_DDI_CLK_SEL(pll->info->id, port) |
		     DPLL_CTRL2_DDI_SEL_OVERRIDE(port));
}
```

`pll->info->id` is `DPLL_ID_SKL_DPLL1 = 1`, `DPLL2 = 2`, `DPLL3 = 3`
(`display/intel_dpll_mgr.h:96`). DPLL selection at modeset
(`skl_get_dpll`, `intel_dpll_mgr.c:1947`): eDP → DPLL0; anything else → first
free of `{DPLL1, DPLL2, DPLL3}`.

Disable: `skl_ddi_disable_clock()` sets `DPLL_CTRL2_DDI_CLK_OFF(port)`
(`intel_ddi.c:1985`).

Additionally the transcoder clock mux must point at the encoder's port
(`intel_ddi_enable_transcoder_clock`, `intel_ddi.c:1042`):

```
display/intel_display_regs.h:2724  TRANS_CLK_SEL_A  0x46140  (+4 per transcoder)
  TRANS_CLK_SEL_DISABLED  (0x0 << 29)
  TRANS_CLK_SEL_PORT(x)   ((x) + 1) << 29      /* x = enum port */
```

For HDMI on port B routed through transcoder A: `TRANS_CLK_SEL(TRANSCODER_A) =
0x46140`, value `(1+1)<<29 = 0x40000000`. eDP (TRANSCODER_EDP) skips this write.

### 2.3 HDMI vs DP programming

`skl_compute_dpll` (`intel_dpll_mgr.c:1937`) dispatches:

* HDMI → `skl_ddi_hdmi_pll_dividers()` — compute WRPLL dividers.
* DP  → `skl_ddi_dp_set_dpll_hw_state()` — only link-rate in `DPLL_CTRL1`.

**HDMI / WRPLL** (`intel_dpll_mgr.c:1827`):

```c
hw_state->ctrl1 = DPLL_CTRL1_OVERRIDE(0) | DPLL_CTRL1_HDMI_MODE(0);
hw_state->cfgcr1 = DPLL_CFGCR1_FREQ_ENABLE |
		   DPLL_CFGCR1_DCO_FRACTION(wrpll_params.dco_fraction) |
		   wrpll_params.dco_integer;
hw_state->cfgcr2 = DPLL_CFGCR2_QDIV_RATIO(wrpll_params.qdiv_ratio) |
		   DPLL_CFGCR2_QDIV_MODE(wrpll_params.qdiv_mode) |
		   DPLL_CFGCR2_KDIV(wrpll_params.kdiv) |
		   DPLL_CFGCR2_PDIV(wrpll_params.pdiv) |
		   wrpll_params.central_freq;
```

The `wrpll_params` come from `skl_ddi_calculate_wrpll(port_clock, ref_clock=24 MHz)`
(`intel_dpll_mgr.c:1684`) and `skl_wrpll_params_populate()`
(`intel_dpll_mgr.c:1615`), whose numeric encoding is:

* `afe_clock = port_clock_kHz * 1000 * 5` (AFE = 5x pixel clock).
* Search all `p0/p1/p2` divider combinations over central freqs
  `{8.4, 9.0, 9.6} GHz`; pick min deviation, even dividers preferred.
* `central_freq`: 9.6G→0, 9.0G→1, 8.4G→3.
* `pdiv` from p0: 1→0, 2→1, 3→2, 7→4.
* `kdiv` from p2: 5→0, 2→1, 3→2, 1→3.
* `qdiv_ratio = p1`, `qdiv_mode = (p1 == 1) ? 0 : 1`.
* `dco_integer  = (p0*p1*p2*afe) / (ref*1e6)`
* `dco_fraction = ((p0*p1*p2*afe) / (ref/1000) - dco_integer*1e6) * 0x8000 / 1e6`

**DP** (`intel_dpll_mgr.c:1867`): `ctrl1 = DPLL_CTRL1_OVERRIDE(0)`;
for `port_clock/2` = 81/108/135/162/216/270 MHz set `DPLL_CTRL1_LINK_RATE_*`
(810→2, 1080→4, 1350→1, 1620→3, 2160→5, 2700→0). No CFGCR writes.

### 2.4 Enable sequence + wait

`skl_ddi_pll_enable` (`display/intel_dpll_mgr.c:1397`):

```c
skl_ddi_pll_write_ctrl1(display, pll, hw_state);   /* DPLL_CTRL1 field for id */
intel_de_posting_read(display, DPLL_CTRL1);

intel_de_write(display, regs[id].cfgcr1, hw_state->cfgcr1);
intel_de_write(display, regs[id].cfgcr2, hw_state->cfgcr2);
intel_de_posting_read(display, regs[id].cfgcr1);
intel_de_posting_read(display, regs[id].cfgcr2);

/* the enable bit is always bit 31 */
intel_de_rmw(display, regs[id].ctl, 0, LCPLL_PLL_ENABLE);

if (intel_de_wait_for_set_ms(display, DPLL_STATUS, DPLL_LOCK(id), 5))
	drm_err(display->drm, "DPLL %d not locked\n", id);
```

`skl_ddi_pll_write_ctrl1` (`intel_dpll_mgr.c:1383`) writes
`DPLL_CTRL1` with `hw_state->ctrl1 << (id*6)` while masking the three fields for
that id. **Poll: `DPLL_STATUS[DPLL_LOCK(id)]` set within 5 ms.**

`skl_ddi_dpll0_enable` only writes CTRL1 (DPLL0 is already on as CDCLK source).

Disable: `skl_ddi_pll_disable` (`intel_dpll_mgr.c:1428`) → clear bit 31 of
`regs[id].ctl` + posting read.

---

## 3. DDI / port

### 3.1 DDI_BUF_CTL

```
display/intel_display_regs.h:2601  _DDI_BUF_CTL_A  0x64000
display/intel_display_regs.h:2604  DDI_BUF_CTL(port) = 0x64000 + port*0x100
    A=0x64000 B=0x64100 C=0x64200 D=0x64300 E=0x64400
  DDI_BUF_CTL_ENABLE            bit 31
  DDI_BUF_EMP_MASK              bits 27:24   (buffer translation index)
  DDI_BUF_TRANS_SELECT(n)       = n << 24
  DDI_BUF_PHY_LINK_RATE_MASK    bits 23:20
  DDI_BUF_PORT_DATA_MASK        bits 19:18
  DDI_BUF_PORT_REVERSAL         bit 16
  DDI_BUF_LANE_STAGGER_DELAY    bits 15:8
  DDI_BUF_IS_IDLE               bit 7
  DDI_A_4_LANES                 bit 4
  DDI_PORT_WIDTH_MASK           bits 3:1   (width-1 encode)
  DDI_INIT_DISPLAY_DETECTED     bit 0
```

For HDMI the port width / swing / emphasis fields are ignored; you only need
`DDI_BUF_TRANS_SELECT`, optional `DDI_BUF_PORT_REVERSAL`, and the enable bit.

### 3.2 Port buffer translation (the gen9 "port PHY" setup)

There is **no combo-PHY**. Program the DDI buffer translation table. The
HDMI entry is **index 9** (DP uses the first `n_entries`):

```
display/intel_display_regs.h:2635  _DDI_BUF_TRANS_A  0x64E00
                                   _DDI_BUF_TRANS_B  0x64E60
  DDI_BUF_TRANS_LO(port,i) = (0x64E00 + port*0x60) + i*8
  DDI_BUF_TRANS_HI(port,i) = (0x64E00 + port*0x60) + i*8 + 4
  DDI_BUF_BALANCE_LEG_ENABLE  bit 31 of LO (i_boost)
```

`hsw_prepare_hdmi_ddi_buffers` (`display/intel_ddi.c:165`):

```c
int level = intel_ddi_level(encoder, crtc_state, 0);
...
/* Entry 9 is for HDMI: */
intel_de_write(display, DDI_BUF_TRANS_LO(port, 9),
	       trans->entries[level].hsw.trans1 | iboost_bit);
intel_de_write(display, DDI_BUF_TRANS_HI(port, 9),
	       trans->entries[level].hsw.trans2);
```

The SKL/CFL HDMI table is `_skl_trans_hdmi[]` (`display/intel_ddi_buf_trans.c:321`),
`hdmi_default_entry = 8` (`intel_ddi_buf_trans.c:338`), applied only when
`has_buf_trans_select()` (i.e. `DISPLAY_VER < 10 && !broxton`, `intel_ddi.c:119`):

```
idx  LO(trans1)   HI(trans2)   i_boost
 0   0x00000018   0x000000AC   0
 1   0x00005012   0x0000009D   0
 2   0x00007011   0x00000088   0
 3   0x00000018   0x000000A1   0
 4   0x00000018   0x00000098   0
 5   0x00004013   0x00000088   0
 6   0x80006012   0x000000CD   1
 7   0x00000018   0x000000DF   0
 8   0x80003015   0x000000CD   1   /* Default */
 9   0x80003015   0x000000C0   1   /* HDMI entry */
10   0x80000018   0x000000C0   1
```

Note the indirection: `hsw_prepare_hdmi_ddi_buffers()` always writes to
**slot 9** of `DDI_BUF_TRANS`, but the values come from
`trans->entries[level]`, where `level = intel_ddi_level()` defaults to
`trans->hdmi_default_entry` = **8** when there is no VBT level shift
(`intel_ddi.c:110`). So the power-on/default HDMI buffer programming is
`DDI_BUF_TRANS_LO(port,9) = entries[8].trans1 = 0x80003015`,
`DDI_BUF_TRANS_HI(port,9) = entries[8].trans2 = 0x000000CD`. You may hardcode
these entry-8 values for a bring-up; if you later need HDMI 2.0 rates, pick the
appropriate `_skl_trans_hdmi[]` row by TMDS clock.

Also program the I_boost LSBs in `DISPIO_CR_TX_BMU_CR0` (host bridge):

```
display/intel_display_regs.h:48   DISPIO_CR_TX_BMU_CR0   0x6C00C
display/intel_display_regs.h:50   BALANCE_LEG_SHIFT(port) = 8 + 3*port
display/intel_display_regs.h:51   BALANCE_LEG_MASK(port)  = 7 << (8 + 3*port)
display/intel_display_regs.h:53   BALANCE_LEG_DISABLE_SHIFT 23
display/intel_display_regs.h:54   BALANCE_LEG_DISABLE(port) (1 << (23 + port))
```

`_skl_ddi_set_iboost` (`intel_ddi.c:1080`): clear `BALANCE_LEG_MASK(port) |
BALANCE_LEG_DISABLE(port)`, then either `iboost << BALANCE_LEG_SHIFT(port)` or
`BALANCE_LEG_DISABLE(port)` if 0. For the default HDMI entry iboost = 1.

### 3.3 HDMI-specific transcoder config

`TRANS_DDI_FUNC_CTL` mode select for HDMI (`intel_ddi_transcoder_func_reg_val_get`,
`display/intel_ddi.c:513`):

```
display/intel_display_regs.h:2484  _TRANS_DDI_FUNC_CTL_A 0x60400 (+0x1000 per transcoder)
  TRANS_DDI_FUNC_ENABLE        bit 31
  TRANS_DDI_PORT_MASK          bits 30:28   TRANS_DDI_SELECT_PORT(port) = port << 28
  TRANS_DDI_MODE_SELECT_MASK   bits 26:24
      HDMI = 0 << 24, DVI = 1 << 24, DP_SST = 2 << 24, DP_MST = 3 << 24
  TRANS_DDI_BPC_MASK           bits 22:20   8bpc = 0
  TRANS_DDI_PVSYNC             bit 17
  TRANS_DDI_PHSYNC             bit 16
  TRANS_DDI_PORT_SYNC_ENABLE   bit 15
  TRANS_DDI_HDMI_SCRAMBLING    bit 0
  TRANS_DDI_HIGH_TMDS_CHAR_RATE bit 4
  TRANS_DDI_PORT_WIDTH_MASK    bits 3:1
```

For HDMI: `FUNC_ENABLE | (port<<28) | MODE_SELECT_HDMI | BPC_8 | VSYNC | HSYNC`,
plus `DDI_SCRAMBLING`/`HIGH_TMDS_CHAR_RATE` only for HDMI 2.0 rates (not 1080p60).

`intel_ddi_enable_transcoder_func` (`intel_ddi.c:627`) simply writes this value.

HDMI infoframes (AVI/VS/SPD/GCP) — needed only if your sink requires AVI
infoframes; at a minimum the GCP/general control packet:

```
display/intel_display_regs.h:2172  HSW_TVIDEO_DIP_CTL(trans) = 0x60200 (+0x1000)
display/intel_display_regs.h:2176  HSW_TVIDEO_DIP_AVI_DATA(trans,i) = 0x60220 + i*4
display/intel_display_regs.h:2217  HSW_TVIDEO_DIP_GCP(trans) = 0x60210
display/intel_display_regs.h:2180  HSW_TVIDEO_DIP_VS_DATA(trans,i) = 0x60260 + i*4
display/intel_display_regs.h:2184  HSW_TVIDEO_DIP_SPD_DATA(trans,i) = 0x602A0 + i*4
```

### 3.4 Buffer enable and idle wait

```c
/* display/intel_ddi.c:3097 */
static void intel_ddi_buf_enable(struct intel_encoder *encoder, u32 buf_ctl)
{
	intel_de_write(display, DDI_BUF_CTL(port), buf_ctl | DDI_BUF_CTL_ENABLE);
	intel_de_posting_read(display, DDI_BUF_CTL(port));
	intel_wait_ddi_buf_active(encoder);
}
```

`intel_wait_ddi_buf_active` (`intel_ddi.c:225`): for `DISPLAY_VER < 10`
(gen9) just `usleep_range(518, 1000)` — i.e. **fixed 518 us** (Bspec
HSW-ICL timeout), then return.

`intel_ddi_buf_disable` (`intel_ddi.c:3108`): clear `DDI_BUF_CTL_ENABLE`, then
(after DP_TP/FEC) `intel_wait_ddi_buf_idle()`.
`intel_wait_ddi_buf_idle` (`intel_ddi.c:199`): waits for `DDI_BUF_IS_IDLE`
(bit 7) to be set, **10 ms timeout** (see also HSW-ADL Bspec timeout 8 us).
The buffer must be idle before reconfiguring it.

### 3.5 Full DDI HDMI enable sequence (`intel_ddi_enable_hdmi`, `intel_ddi.c:3417`)

Ordered (gen9 branch):

1. `intel_hdmi_handle_sink_scrambling()` (SCDC; skip for <=340 MHz).
2. `hsw_prepare_hdmi_ddi_buffers()` — `DDI_BUF_TRANS_LO/HI(port, 9)`.
3. `encoder->set_signal_levels` → `skl_ddi_set_iboost()` → `DISPIO_CR_TX_BMU_CR0`.
4. **Display WA #1143 (skl,kbl,cfl)** chicken bits in `CHICKEN_TRANS` for the
   port's transcoder (`gen9_chicken_trans_reg_by_port`, `intel_ddi.c:3395`):

```
CHICKEN_TRANS_A 0x420c0  B 0x420c4  C 0x420c8  EDP 0x420cc  D 0x420d8
  port A->EDP, B->A, C->B, D->C, E->A
  DDI_TRAINING_OVERRIDE_ENABLE  bit 19
  DDI_TRAINING_OVERRIDE_VALUE   bit 18
  DDIE_TRAINING_OVERRIDE_ENABLE bit 17   /* CHICKEN_TRANS_A only (port E) */
  DDIE_TRAINING_OVERRIDE_VALUE  bit 16   /* CHICKEN_TRANS_A only (port E) */
  set the two (or DDE_ for port E) bits, posting read, udelay(1),
  then clear both bits and rewrite.
```

5. `intel_ddi_power_up_lanes()`.
6. build `buf_ctl` (`DDI_BUF_PORT_REVERSAL`, `DDI_A_4_LANES`, ...).
7. `intel_ddi_buf_enable()` → `DDI_BUF_CTL |= DDI_BUF_CTL_ENABLE`, posting read,
   wait ~518 us.
8. `intel_hdmi_poll_for_scrambling_enable()` (only if scrambling).

`CHICKEN_TRANS` also carries `HSW_FRAME_START_DELAY_MASK` bits 28:27
(`hsw_set_frame_start_delay`, `intel_display.c:1614`), programmed in the pipe
enable path.

---

## 4. Pipe / transcoder

### 4.1 Registers

```
Pipe/transcoder offsets (display/intel_display_device.c:79,96):
  PIPE_A_OFFSET 0x70000  PIPE_B 0x71000  PIPE_C 0x72000
  TRANSCODER_A 0x60000   B 0x61000       C 0x62000   EDP 0x6f000

Timing (transcoder-addressed, +0x1000 per transcoder A/B/C):
display/intel_display_regs.h:485  TRANS_HTOTAL   0x60000  HACTIVE bits15:0, HTOTAL bits31:16 (v-1)
display/intel_display_regs.h:493  TRANS_HBLANK   0x60004  START bits15:0, END bits31:16
display/intel_display_regs.h:500  TRANS_HSYNC    0x60008  START bits15:0, END bits31:16
display/intel_display_regs.h:507  TRANS_VTOTAL   0x6000c  VACTIVE bits15:0, VTOTAL bits31:16
display/intel_display_regs.h:514  TRANS_VBLANK   0x60010  START bits15:0, END bits31:16
display/intel_display_regs.h:521  TRANS_VSYNC    0x60014  START bits15:0, END bits31:16
display/intel_display_regs.h:533  PIPESRC        0x6001c  WIDTH bits31:16, HEIGHT bits15:0
display/intel_display_regs.h:549  TRANS_MULT     0x6002c  (pixel_multiplier - 1)
display/intel_display_regs.h:869  TRANSCONF (PIPECONF) 0x70008  (+0x1000 per trans)
display/intel_display_regs.h:984  PIPE_MISC      0x70030  (+0x1000 per pipe)
display/intel_color_regs.h:71     GAMMA_MODE     0x4a480  (+0x800 per pipe)
display/intel_color_regs.h:313    SKL_BOTTOM_COLOR 0x70034 (+0x1000 per pipe)
display/intel_display_regs.h:1680 CHICKEN_TRANS  0x420c0 (+4 per transcoder)
```

`TRANSCONF` bits (`display/intel_display_regs.h:871`):

```
TRANSCONF_ENABLE              bit 31
TRANSCONF_INTERLACE_MASK_HSW  bits 22:21  (progressive = 0)
TRANSCONF_COLOR_RANGE_SELECT  bit 13
TRANSCONF_DITHER_EN           bit 4
TRANSCONF_PIXEL_COUNT_SCALING bits 1:0
```

`hsw_set_transconf` (`intel_display.c:3282`) writes **val = 0** for a normal
progressive 8bpc RGB mode (interlace PF/PD = 0, no dither), and the enable bit
is set separately:

```c
/* display/intel_display.c:422  intel_enable_transcoder() */
val = intel_de_read(display, TRANSCONF(display, cpu_transcoder));
intel_de_write(display, TRANSCONF(display, cpu_transcoder), val | TRANSCONF_ENABLE);
intel_de_posting_read(display, TRANSCONF(display, cpu_transcoder));
```

Note: **BPC, gamma mode, colorspace are NOT in PIPECONF on gen9.**
* BPC/dither → `PIPE_MISC` bits 7:5/4:2 (`bdw_set_pipe_misc`,
  `intel_display.c:3313`). 8bpc RGB → `PIPE_MISC_BPC_8 = 0`, so writes 0.
* Gamma mode → `GAMMA_MODE` 0x4a480 (mode bits 1:0; 8-bit = 0).
* `SKL_BOTTOM_COLOR` 0x70034: `GAMMA_ENABLE` bit31, `CSC_ENABLE` bit30
  (`skl_color_commit_arm`, `intel_color.c:1141`).

`PIPESRC` value = `((width-1) << 16) | (height-1)`
(`intel_set_pipe_src_size`, `intel_display.c:2887`).

Timing values (`intel_set_transcoder_timings`, `intel_display.c:2714`) are all
`start-1`/`end-1`; e.g. `TRANS_HTOTAL = HACTIVE(hdisplay-1) | HTOTAL(htotal-1)`,
`TRANS_VTOTAL = VACTIVE(vdisplay-1) | VTOTAL(vtotal-1)`, and the HBLANK/HSYNC/
VBLANK/VSYNC registers pack `(start-1)` in bits 15:0 and `(end-1)` in bits 31:16
(compute the actual numbers from your mode).

`TRANS_DDI_FUNC_CTL` — see §3.3.

### 4.2 Enable / disable ordering

**Enable** (`hsw_crtc_enable`, `intel_display.c:1652`; called from
`intel_enable_crtc`/`skl_commit_modeset_enables`, `intel_display.c:7070`):

1. `intel_encoders_pre_pll_enable` → `intel_dpll_enable` → `intel_encoders_pre_enable`
2. `intel_set_pipe_src_size()` (PIPESRC), `bdw_set_pipe_misc()` (PIPE_MISC)
3. `hsw_configure_cpu_transcoder()` (`intel_display.c:1623`): M/N (DP only),
   `intel_set_transcoder_timings()`, `TRANS_MULT`, `hsw_set_frame_start_delay()`
   (CHICKEN_TRANS), `hsw_set_transconf()` (TRANSCONF still **disabled**)
4. `skl_pfit_enable`, color modeset (GAMMA_MODE / BOTTOM_COLOR),
   `hsw_set_linetime_wm`, initial watermarks
5. `intel_encoders_enable` → `intel_ddi_enable()` (`intel_ddi.c:3521`):
   `intel_ddi_enable_transcoder_func()` (TRANS_DDI_FUNC_CTL),
   then **`intel_enable_transcoder()`** (TRANSCONF_ENABLE),
   then `intel_ddi_enable_hdmi()` which finally enables the DDI buffer
   (`DDI_BUF_CTL_ENABLE`, §3.4/§3.5)
6. planes enabled afterwards: `intel_crtc_planes_update_noarm()` then
   `intel_crtc_planes_update_arm()` (`intel_display.c:6881`)

So: **timings → TRANS_DDI_FUNC_CTL → TRANSCONF_ENABLE → DDI_BUF_CTL_ENABLE →
plane**. The DDI buffer is the last encoder-side step, after the transcoder is
running.

**Disable** (`intel_commit_modeset_disables`, `intel_display.c:6979`):
planes first (`PLANE_CTL=0`, `PLANE_SURF=0`), then
`intel_ddi_disable_transcoder_func()`, `intel_disable_transcoder()`,
encoder post-disable, `intel_dpll_disable`.

`intel_disable_transcoder` (`intel_display.c:501`) clears `TRANSCONF_ENABLE`
and calls `intel_wait_for_pipe_off()`.

---

## 5. Primary plane (SKL universal plane)

### 5.1 Offsets

For every SKL plane register with "(plane 1, pipe A)" base `B`:

```
raw_offset(pipe, plane) = B + pipe*0x1000 + plane*0x100
```

`_MMIO_SKL_PLANE` (`display/skl_universal_plane_regs.h:12`) +
`_PIPE`/`_PLANE` = `_PICK_EVEN` (`intel_display_reg_defs.h:54`,
`include/drm/intel/pick.h:13`).

Primary plane (plane id 0), pipe A / B / C:

| Register | Symbol | Pipe A | Pipe B | Pipe C |
|---|---|---|---|---|
| PLANE_CTL | `PLANE_CTL` | **0x70180** | 0x71180 | 0x72180 |
| PLANE_STRIDE | `PLANE_STRIDE` | **0x70188** | 0x71188 | 0x72188 |
| PLANE_POS | `PLANE_POS` | **0x7018c** | 0x7118c | 0x7218c |
| PLANE_SIZE | `PLANE_SIZE` | **0x70190** | 0x71190 | 0x72190 |
| PLANE_KEYVAL | `PLANE_KEYVAL` | **0x70194** | 0x71194 | 0x72194 |
| PLANE_KEYMSK | `PLANE_KEYMSK` | **0x70198** | 0x71198 | 0x72198 |
| PLANE_SURF | `PLANE_SURF` | **0x7019c** | 0x7119c | 0x7219c |
| PLANE_KEYMAX | `PLANE_KEYMAX` | **0x701a0** | 0x711a0 | 0x721a0 |
| PLANE_OFFSET | `PLANE_OFFSET` | **0x701a4** | 0x711a4 | 0x721a4 |
| PLANE_AUX_DIST | `PLANE_AUX_DIST` | **0x701c0** | 0x711c0 | 0x721c0 |
| PLANE_AUX_OFFSET | `PLANE_AUX_OFFSET` | **0x701c4** | 0x711c4 | 0x721c4 |
| PLANE_WM(level) | `PLANE_WM` | **0x70240+4*level** | 0x71240+4*level | 0x72240+4*level |
| PLANE_WM_TRANS | `PLANE_WM_TRANS` | **0x70268** | 0x71268 | 0x72268 |
| PLANE_BUF_CFG | `PLANE_BUF_CFG` | **0x7027c** | 0x7127c | 0x7227c |
| PLANE_NV12_BUF_CFG | `PLANE_NV12_BUF_CFG` | **0x70278** | 0x71278 | 0x72278 |

Sprite0/1 add `+0x100`/`+0x200` to the above. Cursor (pipe A):
`CUR_WM 0x70140+4*level`, `CUR_WM_TRANS 0x70168`, `CUR_BUF_CFG 0x7017c`
(`display/intel_cursor_regs.h:78,94,98`).

### 5.2 Field layouts

```
PLANE_CTL (display/skl_universal_plane_regs.h:35):
  PLANE_CTL_ENABLE                  bit 31
  PLANE_CTL_PIPE_GAMMA_ENABLE       bit 30 (pre-glk)
  PLANE_CTL_YUV_RANGE_CORRECTION_DISABLE bit 28
  PLANE_CTL_FORMAT_MASK_SKL         bits 27:24
      XRGB_8888 = 4<<24, RGB_565 = 14<<24, NV12 = 1<<24, ...
  PLANE_CTL_PIPE_CSC_ENABLE         bit 23 (pre-glk)
  PLANE_CTL_KEY_ENABLE_MASK         bits 22:21
  PLANE_CTL_ORDER_RGBX              bit 20
  PLANE_CTL_PLANE_GAMMA_DISABLE     bit 13 (pre-glk)
  PLANE_CTL_TILED_MASK              bits 12:10
      TILED_LINEAR = 0, TILED_X = 1<<10, TILED_Y = 4<<10, TILED_YF = 5<<10
  PLANE_CTL_ASYNC_FLIP              bit 9
  PLANE_CTL_ALPHA_MASK              bits 5:4
  PLANE_CTL_ROTATE_MASK             bits 1:0

PLANE_STRIDE (skl_universal_plane_regs.h:108): STRIDE bits 11:0
PLANE_POS    (:112): Y bits31:16, X bits15:0
PLANE_SIZE   (:124): HEIGHT bits31:16, WIDTH bits15:0   (each = size-1)
PLANE_KEYMSK (:144): ALPHA_ENABLE bit31
PLANE_KEYMAX (:164): ALPHA bits31:24
PLANE_OFFSET (:174): Y bits31:16, X bits15:0
PLANE_SURF   (:153): ADDR bits31:12, DECRYPT bit2, ASYNC_UPDATE bit0
PLANE_AUX_DIST (:202): AUX_DISTANCE bits31:12, AUX_STRIDE bits11:0
PLANE_WM     (:430): EN bit31, IGNORE_LINES bit30, LINES bits26:14, BLOCKS bits12:0
PLANE_BUF_CFG(:485): END bits28:16, START bits12:0
```

### 5.3 Values

`skl_plane_ctl` (`display/skl_universal_plane.c:1186`): for gen9 it always sets
`PLANE_CTL_PLANE_GAMMA_DISABLE` (bit 13). Typical 1920x1080 XRGB8888 X-tiled:

```
PLANE_CTL = ENABLE(31) | XRGB_8888(4<<24) | PLANE_GAMMA_DISABLE(bit13)
          | TILED_X(1<<10) | ROTATE_0(0) [ | PIPE_GAMMA_ENABLE(bit30) via ctl_crtc ]
```

`PLANE_STRIDE` = `scanout_stride / 64` for **linear** (`skl_plane_stride_mult`,
`skl_universal_plane.c:751`), or `/ tile_width` for tiled. So a linear 1920*4 =
7680-byte stride encodes `7680/64 = 120` (= 0x78).
`PLANE_SIZE` = `(src_h-1)<<16 | (src_w-1)`.
`PLANE_POS`/`PLANE_OFFSET` = `y<<16 | x`.

### 5.4 Arm sequence (`skl_plane_update_arm`, `skl_universal_plane.c:1435`)

Ordered writes:

```
PLANE_KEYVAL, PLANE_KEYMSK, PLANE_KEYMAX
PLANE_OFFSET (Y|X)
PLANE_AUX_DIST, PLANE_AUX_OFFSET
[scaler if any]
PLANE_CTL      <-- self-arms; written just before SURF
PLANE_SURF
```

`skl_plane_update_noarm` (`skl_universal_plane.c:1404`) does
`PLANE_STRIDE`, `PLANE_POS`, `PLANE_SIZE`, then `skl_write_plane_wm()`.

Disable (`skl_plane_disable_arm`, `skl_universal_plane.c:866`):
`skl_write_plane_wm()`, `PLANE_CTL = 0`, `PLANE_SURF = 0`.

### 5.5 PLANE_SURF is a GGTT graphics address

`plane_state->surf = i915_ggtt_offset(ggtt_vma) + surf_offset`
(`display/intel_plane.c:1374`); `i915_ggtt_offset()` = lower 32 bits of the VMA
GGTT node start (`i915_vma.h:151`). Low 12 bits are 0 (4K aligned). **This is a
GGTT/GPA address, not a physical address** — see §7.

---

## 6. Watermarks

### 6.1 Registers and fields

```
display/skl_universal_plane_regs.h:430
  PLANE_WM(pipe,plane,level) = 0x70240 + pipe*0x1000 + plane*0x100 + level*4
    PLANE_WM_EN                bit 31
    PLANE_WM_IGNORE_LINES      bit 30
    PLANE_WM_LINES_MASK        bits 26:14   (13 bits, HW; Linux clamps to 31 on gen9)
    PLANE_WM_BLOCKS_MASK       bits 12:0    (13 bits)
  PLANE_WM_TRANS(pipe,plane)   = 0x70268 + pipe*0x1000 + plane*0x100
  PLANE_BUF_CFG(pipe,plane)    = 0x7027c + pipe*0x1000 + plane*0x100
    PLANE_BUF_END   bits 28:16 (end-1), PLANE_BUF_START bits 12:0
  PLANE_NV12_BUF_CFG           = 0x70278 + ...  (write 0, non-NV12)
display/intel_cursor_regs.h:78
  CUR_WM(pipe,level) = 0x70140 + pipe*0x1000 + level*4 (BLOCKS bits 11:0 for cursor)
  CUR_WM_TRANS(pipe) = 0x70168 + pipe*0x1000
  CUR_BUF_CFG(pipe)  = 0x7017c + pipe*0x1000
display/intel_display_regs.h:3104
  WM_LINETIME(pipe)  = 0x45270 + pipe*4
    HSW_LINETIME     bits 8:0   (max 0x1FF)
    HSW_IPS_LINETIME bits 24:16
```

### 6.2 Per-plane writer (`skl_write_plane_wm`, `skl_universal_plane.c:821`)

```c
for (level = 0; level < display->wm.num_levels; level++)     /* gen9: 8 */
	intel_de_write_dsb(display, dsb, PLANE_WM(pipe, plane_id, level),
			   skl_plane_wm_reg_val(skl_plane_wm_level(pipe_wm, plane_id, level)));
intel_de_write_dsb(display, dsb, PLANE_WM_TRANS(pipe, plane_id),
		   skl_plane_wm_reg_val(skl_plane_trans_wm(pipe_wm, plane_id)));
/* gen9: no SAGV WM write */
intel_de_write_dsb(display, dsb, PLANE_BUF_CFG(pipe, plane_id),
		   skl_plane_ddb_reg_val(ddb));
if (DISPLAY_VER < 11)
	intel_de_write_dsb(display, dsb, PLANE_NV12_BUF_CFG(pipe, plane_id),
			   skl_plane_ddb_reg_val(ddb_y));
```

Packing (`skl_plane_wm_reg_val`, `skl_universal_plane.c:804`):

```
val = (enable ? BIT(31) : 0) | (ignore_lines ? BIT(30) : 0)
    | (blocks << 0) | (lines << 14)
```

DDB (`skl_plane_ddb_reg_val`, `skl_universal_plane.c:779`):
`if (!entry->end) return 0; else PLANE_BUF_END(end-1) | PLANE_BUF_START(start)`.

### 6.3 Minimum safe programming (single linear/X-tiled 1920x1080 plane)

Key facts from the tree (kernel 7.3.0-rc4 renamed/removed the old
`skl_write_wm_values()` / `struct skl_wm_values`; the model is now per-plane):

* Gen9 has **8 watermark levels** (`skl_setup_wm_latency`,
  `skl_watermark.c:3313`: `HAS_HW_SAGV_WM ? 6 : 8`).
* **Level 0 lines are ignored by HW on gen9** (`skl_wm_has_lines`,
  `skl_watermark.c:1782`); the LINES field is only meaningful for levels >= 1.
* On gen9 **transition WM is disabled** (`WaDisableTWM: skl,kbl,cfl,bxt`,
  `skl_watermark.c:1989`), so **`PLANE_WM_TRANS` must be 0**.
* The latency table comes from PCODE `GEN9_PCODE_READ_MEM_LATENCY`; with no
  firmware the driver's fallback gives only level 0 latency = 2 us
  (`wm_read_latency()`, `skl_watermark.c:3186` + `adjust_wm_latency`,
  `skl_watermark.c:3224`); higher levels stay 0 → only level 0 is usable.
  That is exactly the minimum needed.
* Gen9 X-tiled adds +15 us (`skl_needs_memory_bw_wa`, `skl_watermark.c:94`).

**Worked value, 1920x1080@60 XRGB8888, X-tiled, pixel_rate=148500, htotal=2200,
cpp=4, dbuf_block_size=512** (from `skl_compute_plane_wm`,
`skl_watermark.c:1806`):

```
plane_blocks_per_line = ceil(1920*4/512) = 15          (x_tiled: no +1)
latency(0) = 2 + 15 = 17 us
linetime_us = ceil(2200*1000/148500) = 15
method1 = 17*148500*4 / 512000           = 19.72
method2 = ceil(17*148500/(2200*1000))*15 = 2*15 = 30
latency >= linetime, ver 9 -> selected = min(method1,method2) = 19.72
blocks = ceil(19.72) + 1 = 21
lines  = 0 (level 0 ignores lines)
=> PLANE_WM(A, PLANE_1, 0) = PLANE_WM_EN | BLOCKS(21) = 0x80000015
   min_ddb_alloc = 21 + 1 = 22
```

**Linear (not X-tiled):**
```
plane_blocks_per_line = ceil(7680/512) + 1 = 16     (!x_tiled -> +1)
latency(0) = 2
method1 = 2*148500*4/512000 = 2.32 ; method2 = 1*16 = 16
latency(2) < linetime(15) -> selected = method1 = 2.32
blocks = ceil(2.32) + 1 = 4
=> PLANE_WM(A, PLANE_1, 0) = 0x80000004
```

**Pipe linetime:** `DIV_ROUND_UP(htotal*1000*8, pixel_rate)` clamped to 0x1FF
(`skl_linetime_wm`, `intel_display.c:4277`):
1080p60 → `(2200*8000)/148500 = 119 = 0x77`.
`WM_LINETIME(A) = 0x00000077`.

**DDB size:** gen9 total = 896-4 = **892 blocks** (`intel_display_device.c:664`).
A conservative single-plane allocation is start=0, end≈860 →
`PLANE_BUF_CFG(A,0) = (860-1)<<16 | 0 = 0x035B0000`. Hard validity rule:
`blocks + 1 <= plane_ddb_size` (`min_ddb_alloc = max(blocks)+1`,
`skl_watermark.c:1806`; `skl_check_wm_level` disables if it exceeds the
allocation, `skl_watermark.c:1355`).

**Registers that MUST be nonzero for scanout** (single plane, pipe A):

```
DBUF_CTL_S0 0x45008          : DBUF_POWER_REQUEST(31) set, DBUF_POWER_STATE(30) reads 1
PLANE_BUF_CFG(A,0) 0x7027c   : (end-1)<<16 | start   (nonzero)
PLANE_WM(A,0,0) 0x70240      : 0x80000015 (X-tile) / 0x80000004 (linear)  EN + BLOCKS>0
PLANE_WM(A,0,1..7)           : EN=0 (safe to copy level-0 values)
PLANE_WM_TRANS(A,0) 0x70268  : 0x00000000
WM_LINETIME(A) 0x45270       : 0x00000077
```

Bspec note quoted in `skl_check_wm_level` (`skl_watermark.c:1355`): *"All the
watermarks at this level for all enabled planes must be enabled before the
level will be used."* Hence enable only level 0 and it is used directly.

---

## 7. GGTT

### 7.1 BAR layout

```
include/drm/intel/pci_config.h:19   GEN4_GTTMMADR_BAR = 0   (GTTMMADR, BAR0)
include/drm/intel/pci_config.h:20   GEN4_GMADR_BAR   = 2   (GMADR aper., BAR2)

gt/intel_ggtt.c:1180  gen6_gttmmadr_size():  GEN8+ = SZ_16M (0x1000000)
gt/intel_ggtt.c:1190  gen6_gttadr_offset():  size/2
      => gen9 GTT PTE array base = BAR0 + 0x800000  (8 MiB)
```

The source comment (`gt/intel_ggtt.c:1184`): *"GEN8: GTTMMADR size is 16MB and
GTTADR starts at 8MB offset"*. There is a single GGTT shared by render and
display on gen9.

### 7.2 PTE format and write offset

```
gt/intel_gtt.h:62    typedef u64 gen8_pte_t;   (8 bytes)
gt/intel_gtt.h:45-49 I915_GTT_PAGE_SIZE_4K = BIT_ULL(12); I915_GTT_PAGE_SIZE = 4K
gt/intel_ggtt.c:281  gen8_ggtt_pte_encode(addr, pat_index, flags):
                         pte = addr | GEN8_PAGE_PRESENT;
                         if (flags & PTE_LM) pte |= GEN12_GGTT_PTE_LM;  /* gen12 only */
                         return pte;
gt/intel_gtt.h:152   GEN8_PAGE_PRESENT = BIT_ULL(0)
gt/intel_ggtt.c:452  gen8_ggtt_insert_page():
                         pte = (gen8_pte_t __iomem *)ggtt->gsm + offset / 4096;
                         gen8_set_pte(pte, encode);
```

Therefore, for a graphics address `gpa`:

```
pte_raw_offset_in_BAR0 = 0x800000 + (gpa >> 12) * 8
pte_value              = phys_page_addr | 0x1     (gen9: no RW/cache bits)
```

Address field is bits 45:12 (`GEN12_GGTT_PTE_ADDR_MASK`, `gt/intel_gtt.h:105`;
GVT models it as `GTT_HAW=46`, `ADDR_4K_MASK=GENMASK_ULL(45,12)`, `gvt/gtt.c:290`).
After writing PTEs, flush: `GFX_FLSH_CNTL_GEN6 = 0x101008`, bit 0
`GFX_FLSH_CNTL_EN` (`gt/intel_gt_regs.h:1475`).

### 7.3 GPA formation / relationship to GMADR

* `gpa` is the **GGTT virtual byte address** = VMA node start (+ guard);
  the value written to `PLANE_SURF` is `i915_ggtt_offset(vma) + color_plane offset`
  (`display/intel_plane.c:1374`), i.e. exactly the `gpa` you program.
* `i915_ggtt_offset()` = `lower_32_bits(vma->node.start + vma->guard)`
  (`i915_vma.h:151`).
* Display reads `GGTT[gpa >> 12]`, gets the physical page, adds `gpa & 0xFFF`.
* `ggtt->gmadr` (BAR2) is only a **CPU-side aperture window**:
  CPU address = `gmadr.start + gpa` (`gem/i915_gem_mman.c:333`). The display
  itself is **not** limited to the GMADR window on SKL/CFL
  (`intel_plane_needs_low_address()` returns `HAS_GMCH`, false on gen9,
  `display/intel_plane.c:193`).
* GTT VA size = `(PTE_array_bytes / 8) * 4096`, from `SNB_GMCH_CTRL`
  (PCI 0x50 bits 7:6, `gt/intel_ggtt.c:1153`). e.g. 8 MiB of PTEs → 4 GiB VA.

### 7.4 Alignment and coherency

* GGTT page size on gen9 is always **4 KiB** (no 64K/2M GGTT pages).
* Surface base alignment (`skl_plane_min_alignment`, `skl_universal_plane.c:630`):
  linear / X-tiled → **256 KiB**; Y/Yf-tiled → **1 MiB**; aux planes → 4 KiB.
  The offset written into `PLANE_SURF` only needs 4K alignment.
* **No cache/snoop bits exist in the gen9 GGTT PTE.** Coherency is software:
  `i915_gem_object_pin_to_display_plane` sets cache level `I915_CACHE_NONE` and
  flushes caches (`gem/i915_gem_domain.c:421`). **If your OS writes the
  framebuffer with the CPU you must `clflush`/`wbinvd` before enabling scanout.**

---

## 8. Bring-up order (single HDMI output) and gotchas

**Minimal ordered sequence** (pipe A, port B, 1920x1080@60, XRGB8888 X-tiled):

1. Power wells `PW_2` + `DDI_B` (`HSW_PWR_WELL_CTL2` 0x45404, indices 15 and 2).
2. `DBUF_CTL_S0` 0x45008: request + poll state.
3. FB in memory; write GGTT PTEs at `0x800000 + (gpa>>12)*8` = `phys|1`;
   `gpa` 256K-aligned; cache-flush the FB.
4. CDCLK: DPLL0 on (`DPLL_CTRL1` 0x6C058, `LCPLL1_CTL` 0x46010), program
   `CDCLK_CTL` 0x46000 = `0x080002A1` (337.5 MHz), poll LCPLL_PLL_LOCK.
5. HDMI DPLL: choose DPLL1/2/3, write `DPLL_CTRL1`, `DPLL_CFGCR1/2`, set
   `ctl` bit31, poll `DPLL_STATUS` lock.
6. `DPLL_CTRL2` 0x6C05C: `DDI_CLK_SEL(pll_id, PORT_B) | DDI_SEL_OVERRIDE`.
7. `TRANS_CLK_SEL(TRANSCODER_A)` 0x46140 = `0x40000000` (port B).
8. `DDI_BUF_TRANS_LO/HI(PORT_B, 9)` at raw offsets **0x64EA8 / 0x64EAC**
   (`0x64E00 + port*0x60 + 9*8`), default HDMI values `0x80003015 / 0x000000CD`.
9. `DISPIO_CR_TX_BMU_CR0` 0x6C00C iboost for port B.
10. Timings: `TRANS_HTOTAL/HBLANK/HSYNC/VTOTAL/VBLANK/VSYNC` 0x60000..,
    `TRANS_MULT` 0x6002c, `CHICKEN_TRANS_A` 0x420c0 frame-start-delay,
    `PIPESRC` 0x6001c, `TRANSCONF` 0x70008 = 0, `PIPE_MISC` 0x70030 = 0,
    `GAMMA_MODE` 0x4a480 = 0, `SKL_BOTTOM_COLOR` 0x70034.
11. `TRANS_DDI_FUNC_CTL(TRANSCODER_A)` 0x60400 = enable|port<<28|HDMI|8bpc.
12. `TRANSCONF` 0x70008 |= 0x80000000.
13. WA #1143 chicken bits in `CHICKEN_TRANS_A`; `DDI_BUF_CTL(PORT_B)` 0x64100
    |= `0x80000000 | (9<<24)`; posting read; wait ~518 us.
14. Watermarks: `PLANE_BUF_CFG(A,0)=0x035B0000`, `PLANE_WM(A,0,0)=0x80000015`,
    levels 1-7 EN=0, `PLANE_WM_TRANS(A,0)=0`, `WM_LINETIME(A)=0x77`.
15. Plane: `PLANE_STRIDE=120`, `PLANE_POS`, `PLANE_SIZE`, `PLANE_OFFSET`,
    `PLANE_CTL`, `PLANE_SURF=gpa` (`PLANE_CTL` immediately before `PLANE_SURF`).

**Gotchas not to miss**

* **Wa #1183 (skl,kbl,cfl)** — CDCLK change must use the
  `CDCLK_DIVMUX_CD_OVERRIDE` dance in §1.3, plus PCODE
  `SKL_PCODE_CDCLK_CONTROL` prepare/ready handshake. DPLL0 must stay enabled:
  it is the CDCLK source. Poll `LCPLL_PLL_LOCK` (bit 30 of 0x46010), **not**
  `DPLL_STATUS`, for DPLL0.
* **Wa #1143 (skl,kbl,cfl)** — the DDI training-override chicken bits live in
  `CHICKEN_TRANS` for the *port's* transcoder (port→transcoder mapping in §3.5),
  set/clear around DDI buffer enable.
* **WaDisableTWM (skl,kbl,cfl,bxt)** — transition watermarks unusable on gen9;
  `PLANE_WM_TRANS` = 0.
* **Level-0 lines are ignored on gen9**; do not rely on the LINES field for WM0.
* **All enabled planes must enable the same WM level** or HW won't use it —
  enabling only level 0 is the safe minimal config.
* **DDI buffer idle wait** before reconfiguration: `DDI_BUF_IS_IDLE` (bit 7),
  and 518 us after enable on gen9.
* `PLANE_SURF` is a **GGTT GPA**, not a physical address; PTEs are 64-bit at
  BAR0+0x800000; gen9 PTE = `phys | 1` with no cache bits → flush FB caches.
* Gen9 has **no combo PHY / no per-port PLL**; HDMI uses WRPLL (DPLL1/2/3)
  selected via `DPLL_CTRL2`, whereas ICL+ code paths (`ICL_DPCLKA_CFGCR0`,
  `PORT_BUF_CTL`) do not apply.
* **BPC / gamma / colorspace are not in PIPECONF** on gen9 — they are in
  `PIPE_MISC`, `GAMMA_MODE`, `SKL_BOTTOM_COLOR`.
* Timing registers use `value-1` encodings for every start/end field.
* `TRANS_DDI_FUNC_CTL` and `TRANSCONF` must be programmed **before** the plane,
  and the plane's `PLANE_CTL` written immediately before `PLANE_SURF` so the
  plane self-arms atomically.
