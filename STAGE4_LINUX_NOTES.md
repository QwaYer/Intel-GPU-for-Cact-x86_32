# STAGE 4 LINUX NOTES — VBLANK IRQ / PAGE FLIP / HARDWARE CURSOR (Gen9.5, DISPLAY_VER 9)

Reference: Linux `7.3.0-rc4`, tree at
`/home/qwayer/Projects/linux import/linux/linux-master/`, driver under
`drivers/gpu/drm/i915/`. All offsets are **absolute MMIO byte offsets from the
GTTMMADR BAR** (display CPU blocks `0x4xxxx`/`0x6xxxx`/`0x7xxxx`, PCH/south
block `0xc0000`), i.e. directly usable as `intel.mmio + offset`.

CFL is `DISPLAY_VER == 9`, so:
* display-IRQ implementation = `gen8_display_irq_funcs` (`DISPLAY_VER >= 8 && < 11`)
  — `display/intel_display_irq.c:2578-2581`, selection at `:2633-2646`.
* the master display interrupt is **`GEN8_MASTER_IRQ` (0x44200)**, *not* the
  gen11 `GEN11_GFX_MSTR_IRQ`/`GEN11_DISPLAY_INT_CTL` pair.
* CPU-pipe vblank comes from the **per-pipe `GEN8_DE_PIPE_*`** registers, not
  from the legacy `DEIIR` (0x44008) and not from `PIPESTAT`.
* CFL has a PCH (SPT/CNP class) → hotplug/AUX/GMBUS arrive via **`SDEIIR`**
  (0xc4008) and are forwarded through `GEN8_DE_PCH_IRQ`.

---

## PART 1 — VBLANK / DISPLAY INTERRUPTS

### 1.1 Register map (raw MMIO offsets)

| Register | Offset | Meaning | Source |
|---|---|---|---|
| `DEISR` | `0x44000` | legacy DE interrupt status (gen7) | `intel_display_regs.h:1145` |
| `DEIMR` | `0x44004` | legacy DE interrupt mask (1 = masked) | `intel_display_regs.h:1146` |
| `DEIIR` | `0x44008` | legacy DE interrupt identity (ack on write) | `intel_display_regs.h:1147` |
| `DEIER` | `0x4400c` | legacy DE interrupt enable | `intel_display_regs.h:1148` |
| `GEN8_MASTER_IRQ` | `0x44200` | **gen8+ master display interrupt** | `include/drm/intel/intel_gmd_interrupt_regs.h:43` |
| `GEN8_DE_PIPE_ISR(pipe)` | `0x44400 + 0x10*pipe` | per-pipe status | `intel_display_regs.h:1397` |
| `GEN8_DE_PIPE_IMR(pipe)` | `0x44404 + 0x10*pipe` | per-pipe mask (1 = masked) | `intel_display_regs.h:1398` |
| `GEN8_DE_PIPE_IIR(pipe)` | `0x44408 + 0x10*pipe` | per-pipe identity (ack on write) | `intel_display_regs.h:1399` |
| `GEN8_DE_PIPE_IER(pipe)` | `0x4440c + 0x10*pipe` | per-pipe enable | `intel_display_regs.h:1400` |
| `GEN8_DE_PORT_ISR/IMR/IIR/IER` | `0x44440/44/48/4c` | AUX + (BDW/BXT) hotplug | `intel_display_regs.h:1448-1451` |
| `GEN8_DE_MISC_ISR/IMR/IIR/IER` | `0x44460/64/68/6c` | misc (GSE, PSR) | `intel_display_regs.h:1524-1527` |
| `SDEISR` | `0xc4000` | PCH/south status | `intel_display_regs.h:1909` |
| `SDEIMR` | `0xc4004` | PCH mask (1 = masked) | `intel_display_regs.h:1910` |
| `SDEIIR` | `0xc4008` | PCH identity (ack on write) | `intel_display_regs.h:1911` |
| `SDEIER` | `0xc400c` | PCH enable | `intel_display_regs.h:1912` |
| `PIPEDSL(pipe A)` | `0x70000` | pipe scanline counter | `intel_display_regs.h:864` |
| `PIPESTAT(pipe A)` | `0x70024` | legacy pipe status | `intel_display_regs.h:929-930` |
| `PIPEFRAMEPIXEL(pipe A)` | `0x70044` | pixel-in-frame counter | `intel_display_regs.h:1096` |

Pipe-A instances (the only ones this driver needs):
`GEN8_DE_PIPE_IIR(PIPE_A) = 0x44408`, `IMR = 0x44404`, `IER = 0x4440c`.

`PCH_DISPLAY_BASE = 0xc0000` (`intel_display_regs.h:1799`).

### 1.2 Master interrupt + bit fields

```
#define GEN8_MASTER_IRQ             _MMIO(0x44200)                     // intel_gmd_interrupt_regs.h:43
#define  GEN8_MASTER_IRQ_CONTROL    (1 << 31)                          // :44  master enable
#define  GEN8_DE_PCH_IRQ            (1 << 23)                          // :46  PCH/SDE pending
#define  GEN8_DE_MISC_IRQ           (1 << 22)                          // :47
#define  GEN8_DE_PORT_IRQ           (1 << 20)                          // :48
#define  GEN8_DE_PIPE_C_IRQ         (1 << 18)                          // :50
#define  GEN8_DE_PIPE_B_IRQ         (1 << 17)                          // :49
#define  GEN8_DE_PIPE_A_IRQ         (1 << 16)                          // :51
#define  GEN8_DE_PIPE_IRQ(pipe)     (1 << (16 + (pipe)))               // :52
```
`GEN8_MASTER_IRQ` is a **level** register: bit31 is the enable; bits 16..23 are
the second-level "there is a pending DE sub-block" indications. i915 disables
the whole display IRQ by writing 0 (dropping bit31), reads the value first to
snapshot pending sources, then re-writes bit31 last —
`i915_irq.c:422-437` (`gen8_master_intr_disable`/`gen8_master_intr_enable`).

### 1.3 Per-pipe INTR bits (what vblank actually is on gen9)

```
// GEN8_DE_PIPE_IIR/IMR/IER bits — intel_display_regs.h
#define GEN8_PIPE_VBLANK              REG_BIT(0)   // :1439   <-- vblank (pipe A)
#define GEN8_PIPE_VSYNC               REG_BIT(1)   // :1438
#define GEN8_PIPE_SCAN_LINE_EVENT     REG_BIT(2)   // :1437
#define GEN9_PIPE_PLANE1_FLIP_DONE    REG_BIT(3)   // :1434   <-- primary flip done
#define GEN9_PIPE_PLANE2_FAULT        REG_BIT(8)   // :1428
#define GEN9_PIPE_CURSOR_FAULT        REG_BIT(11)  // :1421
#define GEN8_PIPE_FIFO_UNDERRUN       REG_BIT(31)  // :1401   <-- pipe A underrun
#define GEN8_PIPE_CDCLK_CRC_DONE      REG_BIT(28)  // :1403
```
There is no "PIPE_A_UNDERRUN" name; the pipe-A underrun is
`GEN8_PIPE_FIFO_UNDERRUN` (bit31) in `GEN8_DE_PIPE_IIR(PIPE_A)`. Pipe-A vblank
is `GEN8_PIPE_VBLANK` (bit0) in that same register.

The **legacy** names exist but are only used by the pre-gen8 path and must not
be used for gen9:
```
#define DE_PIPEA_VBLANK         (1 << 7)            // intel_display_regs.h:1508 (DEIIR, gen<=7)
#define DE_PIPE_VBLANK(pipe)    (1 << (7+8*pipe))   // :1509
#define DE_PIPEA_VBLANK_IVB     (1 << 0)            // :1392 (IVB DEIIR)
#define DE_PIPE_VBLANK_IVB(pipe)(1 << (5*pipe))     // :1393
#define DE_MASTER_IRQ_CONTROL   (1 << 31)           // :1487 (DEIER bit31, gen7)
#define DE_PIPEA_FIFO_UNDERRUN  (1 << 0)            // :1516
```

### 1.4 South/PCH display interrupts (`0xc4xxx`)

```
#define SDEIIR  _MMIO(0xc4008)   // intel_display_regs.h:1911  ack on write
#define SDEIMR  _MMIO(0xc4004)   // :1910
#define SDEIER  _MMIO(0xc400c)   // :1912
```
On SPT-class PCH (CFL: `INTEL_PCH_TYPE >= PCH_SPT && < PCH_ICP`) the hotplug
bits are:
```
#define SDE_PORTE_HOTPLUG_SPT  (1 << 25)   // :1852
#define SDE_PORTA_HOTPLUG_SPT  (1 << 24)   // :1853  <-- HDMI on port A
#define SDE_PORTD_HOTPLUG_CPT  (1 << 23)   // :1854
#define SDE_PORTC_HOTPLUG_CPT  (1 << 22)   // :1855
#define SDE_PORTB_HOTPLUG_CPT  (1 << 21)   // :1856
#define SDE_CRT_HOTPLUG_CPT    (1 << 19)   // :1857
#define SDE_GMBUS_CPT          (1 << 17)   // :1870
#define SDE_HOTPLUG_MASK_SPT   (...)       // :1864
```
**Split rule:** CPU-pipe vblank (and CPU pipe underrun / flip done) is delivered
in `GEN8_DE_PIPE_IIR(pipe)` under `GEN8_MASTER_IRQ`; PCH-originated events
(HDMI hotplug on the PCH DDC/AUX pins, GMBUS, audio, PCH transcoder
underrun) are delivered in `SDEIIR` and surfaced to the CPU through
`GEN8_MASTER_IRQ` bit23 `GEN8_DE_PCH_IRQ`. i915 acks SDE inside
`gen8_read_and_ack_pch_irqs()` (`intel_display_irq.c:1379-1407`) and dispatches
to `spt_irq_handler()` for SPT/CNP (`intel_hotplug_irq.c:593`) via
`gen8_de_irq_handler()` (`intel_display_irq.c:1552-1573`). For a single HDMI
output you normally only need SDE for hotplug/AUX; the vblank path does not
touch SDE.

### 1.5 Enabling per-pipe vblank (i915 sequence)

* CRTC funcs for `DISPLAY_VER >= 8` select `.enable_vblank = bdw_enable_vblank`
  (`intel_crtc.c:253-256`, `:392-393`).
* `bdw_enable_vblank()` — `intel_display_irq.c:1809-1832`:
```c
bdw_enable_pipe_irq(display, pipe, GEN8_PIPE_VBLANK);
```
* `bdw_enable_pipe_irq()` → `bdw_update_pipe_irq()` — `intel_display_irq.c:224-265`:
```c
new_val = display->irq.de_pipe_imr_mask[pipe];
new_val &= ~interrupt_mask;                 /* clear mask bit  -> unmask */
new_val |= (~enabled_irq_mask & interrupt_mask);
if (new_val != display->irq.de_pipe_imr_mask[pipe]) {
        display->irq.de_pipe_imr_mask[pipe] = new_val;
        intel_de_write(display, GEN8_DE_PIPE_IMR(pipe), new_val);
        intel_de_posting_read(display, GEN8_DE_PIPE_IMR(pipe));   /* posting read: order the write */
}
```
So **enabling pipe vblank = clearing the vblank bit in `GEN8_DE_PIPE_IMR`**;
the IER bit was already set at postinstall.
* Postinstall (`gen8_de_irq_postinstall`, `intel_display_irq.c:2410-2503`):
```c
de_pipe_enables = de_pipe_masked |                     /* faults + CDCLK CRC done */
                  GEN8_PIPE_VBLANK | GEN8_PIPE_FIFO_UNDERRUN |
                  gen8_de_pipe_flip_done_mask(display); /* = GEN9_PIPE_PLANE1_FLIP_DONE for ver>=9, :1373-1378 */
...
display->irq.de_pipe_imr_mask[pipe] = ~de_pipe_masked;
irq_init(display, GEN8_DE_PIPE_IRQ_REGS(pipe),
         display->irq.de_pipe_imr_mask[pipe], de_pipe_enables);
```
`irq_init()` (`:67-75`) writes `IER = ier_val`, then `IMR = imr_val`, then a
posting read of IMR. Net effect at postinstall for pipe A:
`IER(0x4440c) = <faults|CDCLK_CRC_DONE> | VBLANK | FIFO_UNDERRUN | PLANE1_FLIP_DONE`,
`IMR(0x44404) = ~<faults|CDCLK_CRC_DONE>` (VBLANK/UNDERRUN/FLIP_DONE still masked;
each is unmasked on demand by the IMR rmw above).
**Both registers matter: a source fires only when its IER bit is 1 *and* its
IMR bit is 0.** For a driver that always wants vblank, simply write
`IER = VBLANK|FIFO_UNDERRUN|PLANE1_FLIP_DONE` and `IMR = 0`.

`PIPESTAT` is **not** used for gen9 vblank. It still exists
(`PIPESTAT(pipe A)=0x70024`): bit17 `PIPE_VBLANK_INTERRUPT_ENABLE`, bit1
`PIPE_VBLANK_INTERRUPT_STATUS`, bit31 `PIPE_FIFO_UNDERRUN_STATUS`
(`intel_display_regs.h:951`, `:974`, `:931`) and is the path used by
`i8xx/i965/ilk_enable_vblank` only (see `intel_display_irq.c:1809` vs `:1672`,
`:1702`, `:1747`).

### 1.6 Hardware master enable (PCI/MSI)

* MSI is enabled once at probe for GRAPHICS_VER >= 5: `pci_enable_msi(pdev)`
  (`i915_driver.c:559`), then `intel_irq_install()` does
  `request_irq(pci_dev->irq, handler, IRQF_SHARED, ...)` (`i915_irq.c:1107-1131`).
  IRQF_SHARED must be kept if legacy INTx is used.
* Hardware master enable is the last postinstall step:
  `gen8_irq_postinstall()` → `gen8_master_intr_enable()` →
  `raw_reg_write(regs, GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL)`
  (`i915_irq.c:701-709`, `:435-438`).
* Before enabling, i915 resets IRQs: `gen8_irq_reset()` writes master=0, resets
  GT, calls `intel_display_irq_reset()` (masks everything) —
  `i915_irq.c:618-627`, `intel_display_irq.c:2196-2214`.

**Minimal init order for the Cact module (gen9):**
1. map BAR, set up MSI/INTx, `request_irq(..., IRQF_SHARED, ...)`.
2. `GEN8_MASTER_IRQ = 0` (disable master).
3. `GEN8_DE_PIPE_IMR(PIPE_A) = 0xffffffff` (mask all), `GEN8_DE_PIPE_IIR(PIPE_A) = 0xffffffff` twice (drain).
4. `GEN8_DE_PIPE_IER(PIPE_A) = GEN8_PIPE_VBLANK | GEN8_PIPE_FIFO_UNDERRUN | GEN9_PIPE_PLANE1_FLIP_DONE`.
5. `GEN8_DE_PIPE_IMR(PIPE_A) = ~(VBLANK|FIFO_UNDERRUN|PLANE1_FLIP_DONE)` (unmask), posting read.
6. enable the pipe (transcoder/DDI already done).
7. `GEN8_MASTER_IRQ = GEN8_MASTER_IRQ_CONTROL` (bit31), posting read.

### 1.7 Handler: how DEIIR is read and dispatched

Top level `gen8_irq_handler()` — `i915_irq.c:440-472`:
```c
master_ctl = gen8_master_intr_disable(regs);   /* write 0, then read snapshot */
if (!master_ctl) { gen8_master_intr_enable(regs); return IRQ_NONE; }
gen8_gt_irq_handler(to_gt(dev_priv), master_ctl);
if (master_ctl & ~GEN8_GT_IRQS) {
        const struct intel_display_irq_state state = { .master_ctl = master_ctl };
        intel_display_irq_handler(display, &state);   /* -> gen8_de_irq_handler */
}
gen8_master_intr_enable(regs);                 /* restore bit31 last */
```
`gen8_de_irq_handler()` — `intel_display_irq.c:1409-1575`, pipe loop:
```c
for_each_pipe(display, pipe) {
        if (!(master_ctl & GEN8_DE_PIPE_IRQ(pipe))) continue;
        iir = intel_de_read(display, GEN8_DE_PIPE_IIR(pipe));
        if (!iir) { /* "master control interrupt lied" */ continue; }
        intel_de_write(display, GEN8_DE_PIPE_IIR(pipe), iir);   /* ack: write value back */
        if (iir & GEN8_PIPE_VBLANK)   intel_handle_vblank(display, pipe);
        if (iir & gen8_de_pipe_flip_done_mask(display)) flip_done_handler(display, pipe);
        if (iir & GEN8_PIPE_CDCLK_CRC_DONE) hsw_pipe_crc_irq_handler(display, pipe);
        if (iir & GEN8_PIPE_FIFO_UNDERRUN)  intel_cpu_fifo_underrun_irq_handler(display, pipe);
        ...
}
```
`intel_handle_vblank()` → `drm_crtc_handle_vblank(&crtc->base)`
(`intel_display_irq.c:146-152`). `flip_done_handler()` sends the pending flip
event (`:463-478`), used for the flip-completion path (Part 2).

### 1.8 Ack semantics (write-1-to-clear)

IIR registers are read-modify-write/ack-on-write. i915's rules
(`intel_display_irq.c:34-75`):
```c
irq_reset:  IMR = 0xffffffff; posting read;
            IER = 0;
            IIR = 0xffffffff; posting read;   /* clear */
            IIR = 0xffffffff; posting read;   /* "IIR can queue up two events" */
irq_init:   assert_iir_is_zero();             /* WARN + clear 0xffffffff twice if non-zero */
            IER = ier_val; IMR = imr_val; posting read;
```
To ack: **write back exactly the value you read** (clears precisely those
pending bits), or write `0xffffffff` to clear all. Always follow with a
posting read before re-enabling `GEN8_MASTER_IRQ`, otherwise the completion of
the MMIO write is not ordered before the master re-enable.

### 1.9 Chicken bits / ordering

* Always re-write `GEN8_MASTER_IRQ = GEN8_MASTER_IRQ_CONTROL` **last** in the
  handler (`i915_irq.c:469`), after acking the sub-blocks.
* The "master control interrupt lied" checks (`:1419`, `:1441`, `:1469`,
  `:1514`) are just diagnostics for master-bit-set-but-IIR-zero.
* Legacy `DEIMR` (0x44004) masks the `DE_MASTER_IRQ_CONTROL` bit when the
  gen7 path disables the display IRQ (`intel_display_irq.c:999`); not used on
  gen9.
* `intel_wait_for_pipe_scanline_moving()` polls `PIPEDSL` (line mask
  `GENMASK(19,0)`, `intel_display_regs.h:867`) until two reads 5 ms apart
  differ — used to unstick a pipe before re-programming timing/watermarks
  (`intel_vblank.c:482-516`, `intel_display.c:8503`). Useful before the first
  flip after the pipe was just enabled.
* Pipe scanline reads: `PIPEDSL(pipe A) = 0x70000` (bit31 ctg+ "curr field",
  bits[19:0] line), `PIPEFRAMEPIXEL(pipe A) = 0x70044` (low 24 bits = pixel
  count). i915 chooses between them for timestamping
  (`intel_vblank.c:375-410`).

---

## PART 2 — PAGE FLIP (MMIO, no DSB / no flip queue)

### 2.1 Plane registers (pipe A, primary = PLANE_1)

| Register | Offset | Notes | Source |
|---|---|---|---|
| `PLANE_CTL(pipe A, plane 1)` | `0x70180` | `_PLANE_CTL_1_A` | `skl_universal_plane_regs.h:35-38` |
| `PLANE_STRIDE` | `0x70188` | `PLANE_STRIDE_(stride)` = bytes/line, mask GENMASK(11,0) | `:88`, `:95-97` |
| `PLANE_POS` | `0x7018c` | dst x/y | `:99` |
| `PLANE_SIZE` | `0x70190` | `(h-1)<<16 | (w-1)` | `:110` |
| `PLANE_KEYVAL/KEYMSK/KEYMAX` | `0x70194/98/a0` | colorkey | `:118-133` |
| `PLANE_SURF` | `0x7019c` | graphics (GGTT) address, **arming write** | `:155-162` |
| `PLANE_SURFLIVE` | `0x701ac` | currently-scanned-out address | `:186-191` |
| `PLANE_OFFSET` | `0x701a4` | plane offset x/y (tiling) | `:135-145` |
| `PLANE_AUX_DIST` | `0x701c0` | 0 for linear single-plane | `:202-207` |
| `PLANE_AUX_OFFSET` | `0x701c4` | 0 for linear single-plane | `:213-218` |

`PLANE_SURF` fields: `PLANE_SURF_ADDR_MASK = GENMASK(31,12)` (4 KB granular),
`PLANE_SURF_DECRYPT = BIT(2)`, `PLANE_SURF_ASYNC_UPDATE = BIT(0)`
(`skl_universal_plane_regs.h:160-162`).

`PLANE_CTL` fields used on gen9 (`skl_universal_plane_regs.h:39-86`,
`skl_plane_ctl()` at `skl_universal_plane.c:1186-1222`):
`PLANE_CTL_ENABLE = BIT(31)`, `FORMAT_MASK_SKL = GENMASK(27,24)` with
`XRGB_8888 = 4<<24` (XRGB8888/ARGB8888), `PLANE_CTL_ORDER_RGBX = BIT(20)` for
BGR, `TILED_MASK = GENMASK(12,10)` (0 = linear, 1 = X-tile, 4 = Y-tile),
`PLANE_CTL_PLANE_GAMMA_DISABLE = BIT(13)` (set for ver<10),
`PLANE_CTL_ASYNC_FLIP = BIT(9)`, `PLANE_CTL_TRICKLE_FEED_DISABLE = BIT(14)`,
`PLANE_CTL_ROTATE_MASK = GENMASK(1,0)`, alpha `GENMASK(5,4)`.

### 2.2 Address encoding

```c
/* skl_surf_address() — skl_universal_plane.c:1293-1304 */
drm_WARN_ON(display->drm, offset & 0xfff);   /* normal path: must be 4 KB aligned */
return offset;                                /* raw GGTT byte address */
/* plane_state->surf = <GGTT offset of fb> + plane->surf_offset();  intel_plane.c:1374 */
/* skl_plane_surf_offset(): skl_universal_plane.c:1317-1328 */
plane_surf = skl_surf_address(plane_state, color_plane);
if (plane_state->decrypt) plane_surf |= PLANE_SURF_DECRYPT;
```
So write the raw 4 KB-aligned GGTT address of the new framebuffer; add
`PLANE_SURF_DECRYPT` only for protected content. **Do not** add
`PLANE_SURF_ASYNC_UPDATE` for a normal vblank-synced flip (that bit is for
async page flips on DISPLAY_VER >= 30; on gen9 async uses
`PLANE_CTL_ASYNC_FLIP` and only when the UAPI asked for async).

### 2.3 `skl_plane_update_noarm` / `skl_plane_update_arm`

`skl_plane_update_noarm()` — `skl_universal_plane.c:1405-1429` (safe to write
any time; NOT arming):
```c
PLANE_STRIDE(pipe,plane_id) = PLANE_STRIDE_(stride);
PLANE_POS(pipe,plane_id)    = PLANE_POS_Y(crtc_y) | PLANE_POS_X(crtc_x);
PLANE_SIZE(pipe,plane_id)   = PLANE_HEIGHT(src_h-1) | PLANE_WIDTH(src_w-1);
skl_write_plane_wm(...);                    /* watermark/DDB regs */
```
`skl_plane_update_arm()` — `skl_universal_plane.c:1436-1500` (the arming pair):
```c
plane_ctl = plane_state->ctl | skl_plane_ctl_crtc(crtc_state);
...
PLANE_KEYVAL/KEYMSK/KEYMAX, PLANE_OFFSET, PLANE_AUX_DIST, PLANE_AUX_OFFSET,
[PLANE_COLOR_CTL if ver>=10],
[scaler if any],
/* "The control register self-arms if the plane was previously disabled.
   Try to make the plane enable atomic by writing the control register
   just before the surface register." */
PLANE_CTL(pipe, plane_id) = plane_ctl;
PLANE_SURF(pipe, plane_id) = plane_state->surf;
```
Note `intel_de_write_dsb()` with `dsb == NULL` is a plain `intel_de_write()`,
so these become direct MMIO writes for a no-DSB driver.

Async variant `skl_plane_async_flip()` — `:1707-1733`: sets
`PLANE_CTL_ASYNC_FLIP` (ver<30) when `async_flip`, then writes `PLANE_CTL`
then `PLANE_SURF`. `intel_plane_update_arm()` (`intel_plane.c:1020-1035`)
routes to it only when `crtc_state->do_async_flip`.

### 2.4 The actual commit / "intel_crtc_page_flip"

There is no bespoke `intel_crtc_page_flip` any more; CRTCs use
`.page_flip = drm_atomic_helper_page_flip` (`intel_crtc.c:245`) and the commit
tail is `intel_update_crtc()` — `intel_display.c:6883-6915`:
```c
} else {                                   /* no DSB, no flipq  -- MMIO flip */
        /* Perform vblank evasion around commit operation */
        intel_pipe_update_start(state, crtc);
        if (new_crtc_state->dsb_commit) intel_dsb_commit(...);
        commit_pipe_pre_planes(state, crtc);
        intel_crtc_planes_update_arm(NULL, state, crtc);   /* PLANE_CTL + PLANE_SURF */
        commit_pipe_post_planes(state, crtc);
        intel_pipe_update_end(state, crtc);
}
```
`intel_crtc_planes_update_arm()` → `skl_crtc_planes_update_arm()`
(`intel_plane.c:1102-1133`) walks planes in DDB order and calls
`intel_plane_update_arm()` (which calls `skl_plane_update_arm`).

`intel_pipe_update_start()` / `_end()` — `intel_crtc.c:587-641`, `:726-826`:
* `intel_vblank_evade_init()` computes the safe scanline window just before
  vblank (`intel_vblank.c:682-720`; `min = vblank_start - VBLANK_EVASION_TIME_US`,
  `max = vblank_start - 1`, `VBLANK_EVASION_TIME_US = 100`).
* `drm_crtc_vblank_get()` then `local_irq_disable()` then `intel_vblank_evade()`
  spins/blocks until the current scanline is outside `[min,max]`
  (`intel_vblank.c:722-760`), giving ~100 us of IRQ-disabled time to write the
  plane registers.
* At the end `intel_pipe_update_end()` re-enables IRQs, checks the frame
  counter did not roll (`Atomic update failure on pipe ...`), and arms the vblank
  event with `intel_crtc_arm_vblank_event(new_crtc_state)`
  (`intel_crtc.c:760-761`, `:694-716`).
* For `do_async_flip`, `start` skips evasion and just moves the pending event to
  `crtc->flip_done_event` (`:603-605`); it is completed later from
  `flip_done_handler()` on `GEN9_PIPE_PLANE1_FLIP_DONE`.

**The vblank-wait primitive when there is no DSB is exactly
`intel_crtc_wait_for_next_vblank()` → `drm_crtc_wait_one_vblank()`**
(`intel_crtc.c:67-69`); the fine-grained variant used *inside* the commit is
`intel_vblank_evade()` driven by `intel_get_crtc_scanline()` reading
`PIPEDSL` (`intel_vblank.c:465-480`, `:244-260`).

### 2.5 Recommended minimal MMIO flip sequence (pipe A, primary plane 1)

Use this when a new buffer has the same size/stride/format (pure address flip):

1. Ensure `GEN9_PIPE_PLANE1_FLIP_DONE` (bit3) and `GEN8_PIPE_VBLANK` (bit0) are
   enabled in `GEN8_DE_PIPE_IER/IMR` (Part 1).
2. Optionally `intel_wait_for_pipe_scanline_moving(PIPE_A)` once after pipe
   enable to be sure `PIPEDSL` is running.
3. Wait for the start of vblank / evade:
   * coarse: `wait for vblank` (block on the vblank IRQ or poll `PIPEDSL` until
     it wraps), or
   * precise: read `PIPEDSL(0x70000) & GENMASK(19,0)` in a loop and issue the
     write when the scanline is in the `[vtotal-... , vblank_start-1]` window
     (i915's `intel_vblank_evade`).
4. `PLANE_SURF(0x7019c) = new_ggtt_addr` (4 KB aligned; no DECRYPT/ASYNC bits
   unless needed). This single double-buffered write self-arms and latches at
   the next vblank start.
5. If size/stride/format changed, first write `PLANE_STRIDE/POS/SIZE`
   (`skl_plane_update_noarm`) and then re-write `PLANE_CTL` **immediately before**
   `PLANE_SURF` so plane-enable is atomic (`skl_plane_update_arm`).
6. Poll `PLANE_SURFLIVE(0x701ac) & PLANE_SURF_ADDR_MASK` until it equals the new
   address to confirm the flip latched; and/or wait for
   `GEN9_PIPE_PLANE1_FLIP_DONE` in `GEN8_DE_PIPE_IIR(PIPE_A)` to signal
   completion (ack by writing the value back).
7. Do not rewrite `PLANE_CTL` on a pure address flip — it is not required and
   may cause a needless re-arm. The register that must be written to flip is
   `PLANE_SURF`; `PLANE_CTL` is only needed when format/tiling/enable change.

### 2.6 PLANE_SURF extras

* `PLANE_SURF_ASYNC_UPDATE` (bit0) is only used for async flips on
  DISPLAY_VER >= 30 (`skl_plane_async_flip`, `:1721-1723`). On gen9 async flips
  set `PLANE_CTL_ASYNC_FLIP` instead — and only for a genuine async-flip
  request; a normal flip must not set it.
* `PLANE_SURF_DECRYPT` (bit2) only for protected buffers.
* No `PLANE_CTL` re-write is required for an address-only flip.

---

## PART 3 — HARDWARE CURSOR (SKL universal cursor, pipe A)

### 3.1 Register map (raw MMIO offsets, pipe A)

| Canonical / bspec name | Kernel name | Offset | Source |
|---|---|---|---|
| `CUR_CTL` | `CURCNTR(pipe)` | `0x70080` (`_CURACNTR`) | `intel_cursor_regs.h:11-12, 64` |
| `CUR_BASE` | `CURBASE(pipe)` | `0x70084` (`_CURABASE`) | `:43-44, 91` |
| `CUR_POS` | `CURPOS(pipe)` | `0x70088` (`_CURAPOS`) | `:46-47, 88` |
| `CUR_SURF` (live) | `CURSURFLIVE(pipe)` | `0x700ac` (`_CURASURFLIVE`) | `:74-75` |
| `CUR_SIZE`(845) / `CUR_FBC_CTL` | `CUR_FBC_CTL(pipe)` | `0x700a0` (`_CUR_FBC_CTL_A`) | `:59-66` |
| `CUR_WM(level)` | `CUR_WM(pipe,level)` | `0x70140 + 4*level` | `:78-81` |
| `CUR_WM_TRANS` | `CUR_WM_TRANS(pipe)` | `0x70168` | `:94-96` |
| `CUR_WM_SAGV` / `_TRANS` | `CUR_WM_SAGV*` | `0x70158` / `0x7015c` | `:89-92` |
| `CUR_BUF_CFG` | `CUR_BUF_CFG(pipe)` | `0x7017c` | `:98-105` |

`CUR_SIZE` (the fixed-size width/height register, `CURSIZE(pipe)` = 0x700a0,
`intel_cursor_regs.h:59-63`) is **pre-SKL only** (845/865) and shares the
offset with `CUR_FBC_CTL`; on gen9 use `CUR_FBC_CTL` + `MCURSOR_MODE_*`, not
`CUR_SIZE`.

### 3.2 Field layouts

`CUR_CTL` / `CURCNTR` (`intel_cursor_regs.h:13-42`):
```
MCURSOR_MODE_MASK       0x27
  MCURSOR_MODE_DISABLE     0x00
  MCURSOR_MODE_64_ARGB_AX  0x27   (0x20 | 0x07)   <-- 64x64, ARGB/XRGB 32bpp
  MCURSOR_MODE_128_ARGB_AX 0x22   (0x20 | 0x02)   <-- 128x128
  MCURSOR_MODE_256_ARGB_AX 0x23   (0x20 | 0x03)   <-- 256x256
MCURSOR_ROTATE_180      BIT(15)
MCURSOR_PIPE_GAMMA_ENABLE BIT(26)   /* set only if crtc_state->gamma_enable (ver<11) */
MCURSOR_PIPE_CSC_ENABLE   BIT(24)
MCURSOR_TRICKLE_FEED_DISABLE BIT(14)
```
The HW title `CUR_SURF_PIXEL_FORMAT` is encoded inside `MCURSOR_MODE`: only
32bpp ARGB/XRGB (the `_AX` modes) are supported. i915 advertises exactly one
format, `DRM_FORMAT_ARGB8888` (`intel_cursor_formats[]`,
`intel_cursor.c:32-34`); XRGB is not a separate mode.

`CUR_POS` / `CURPOS` (`intel_cursor_regs.h:48-53`):
```
CURSOR_POS_X_MASK  GENMASK(14,0)     CURSOR_POS_X(x) = x
CURSOR_POS_X_SIGN  BIT(15)           set for negative x
CURSOR_POS_Y_MASK  GENMASK(30,16)    CURSOR_POS_Y(y) = y
CURSOR_POS_Y_SIGN  BIT(31)           set for negative y
```
i915 builds it in `intel_cursor_position()` (`intel_cursor.c:41-70`) — the
cursor may have negative X/Y; sign bit + magnitude.

`CUR_FBC_CTL` (`intel_cursor_regs.h:65-68`):
```
CUR_FBC_EN          BIT(31)
CUR_FBC_HEIGHT_MASK GENMASK(7,0)     CUR_FBC_HEIGHT(h) = h
```
Used when cursor is non-square: `fbc_ctl = CUR_FBC_EN | CUR_FBC_HEIGHT(height-1)`
(`intel_cursor.c:676-677`).

`CUR_BUF_CFG` (`intel_cursor_regs.h:101-105`):
```
CUR_BUF_END_MASK   GENMASK(27,16)    CUR_BUF_END(end)
CUR_BUF_START_MASK GENMASK(11,0)     CUR_BUF_START(start)
```
(`skl_cursor_ddb_reg_val()` = `CUR_BUF_END(end-1) | CUR_BUF_START(start)`,
`intel_cursor.c:596-601`.)

`CUR_WM` (`intel_cursor_regs.h:81-84`):
```
CUR_WM_EN           BIT(31)          <-- watermarks must be enabled
CUR_WM_IGNORE_LINES BIT(30)
CUR_WM_LINES_MASK   GENMASK(26,14)   CUR_WM_LINES
CUR_WM_BLOCKS_MASK  GENMASK(11,0)    CUR_WM_BLOCKS
```
(`skl_cursor_wm_reg_val()`, `intel_cursor.c:605-617`.)

### 3.3 Size, format, alignment (gen9)

* **Max cursor is 256x256, not 64x64.** `intel_cursor_mode_config_init()`
  (`intel_cursor.c:1103-1121`) sets `cursor_width = 256; cursor_height = 256`
  for every non-i845/i865/i830/i85x/i915 platform, i.e. SKL/CFL.
  `intel_cursor_add_size_hints_property()` enumerates square POT sizes
  64/128/256 (`:983-1004`). Legal widths are exactly {64,128,256}
  (`i9xx_cursor_size_ok()`, `intel_cursor.c:433-469`).
* **Non-square:** gen9 has `HAS_CUR_FBC` (`DISPLAY_VER 7..13`,
  `intel_display_device.h:160`), so with rotation 0 the height may be 8..width
  and is programmed via `CUR_FBC_CTL`; without it the cursor must be square.
* **Formats:** ARGB8888 / XRGB8888 32bpp only.
* **Alignment:** cursor buffer must be 4 KB aligned — `i9xx_cursor_min_alignment()`
  returns `4*1024` (`intel_cursor.c:367-374`); max stride = `cursor_width * 4`
  = 1024 B for a 256-wide cursor (`intel_cursor.c:344-350`). The cursor buffer
  is a **GGTT graphics address**, same encoding as `PLANE_SURF`
  (`CURBASE` = 4 KB-aligned GGTT byte address; i915 warns if unaligned via the
  shared `intel_plane_compute_gtt` path).

### 3.4 Programming sequence

`i9xx_cursor_update_arm()` is the function used on gen9 (assigned at
`intel_cursor.c:1042`; the plane-create switch is `:1026-1044`). Full ordering
(`intel_cursor.c:652-718`):
```c
if (plane_state && plane_state->uapi.visible) {
        cntl = plane_state->ctl | i9xx_cursor_ctl_crtc(crtc_state);
        if (ver < 14 && width != height) fbc_ctl = CUR_FBC_EN | CUR_FBC_HEIGHT(height-1);
        base = plane_state->surf;                      /* GGTT addr, ARGB8888 */
        pos  = intel_cursor_position(crtc_state, plane_state, false);
}

if (DISPLAY_VER >= 9)
        skl_write_cursor_wm(dsb, plane, crtc_state);   /* CUR_WM, CUR_WM_TRANS,
                                                          CUR_WM_SAGV*, CUR_BUF_CFG */
i9xx_cursor_update_sel_fetch_arm / disable (tgl+; NOP on gen9)

if (base != old.base || fbc_ctl != old.size || cntl != old.cntl) {
        if (HAS_CUR_FBC) CUR_FBC_CTL = fbc_ctl;         /* 0x700a0 */
        CURCNTR = cntl;                                 /* 0x70080 */
        CURPOS  = pos;                                  /* 0x70088 */
        CURBASE = base;                                 /* 0x70084  <-- arms */
} else {
        CURPOS  = pos;                                  /* 0x70088  move */
        CURBASE = base;                                 /* 0x70084  <-- arms */
}
```
`skl_write_cursor_wm()` (`intel_cursor.c:619-649`) writes, in order:
`CUR_WM(pipe, level)` for each WM level (with `CUR_WM_EN` when
`level->enable`), `CUR_WM_TRANS`, optional `CUR_WM_SAGV`/`CUR_WM_SAGV_TRANS`,
then `CUR_BUF_CFG`. **`CUR_WM_EN` (bit31) must be set** for the enabled WM
level and a valid `CUR_BUF_CFG` (DDB start/end within the pipe's cursor DDB
allocation) must be programmed, or the cursor fetch/watermark is invalid.

`i9xx_cursor_ctl()` (`intel_cursor.c:400-427`) sets the mode:
```c
switch (drm_rect_width(&plane_state->uapi.dst)) {
case 64:  cntl |= MCURSOR_MODE_64_ARGB_AX;  break;   /* 0x27 */
case 128: cntl |= MCURSOR_MODE_128_ARGB_AX; break;   /* 0x22 */
case 256: cntl |= MCURSOR_MODE_256_ARGB_AX; break;   /* 0x23 */
}
if (rotation & DRM_MODE_ROTATE_180) cntl |= MCURSOR_ROTATE_180;
```

**Ordering notes** (from the comment block in `i9xx_cursor_update_arm`,
`intel_cursor.c:679-700`):
* Always write `CURCNTR` **before** `CURPOS` (a `CURCNTR` write can arm a
  pending `CURPOS` update on some platforms).
* Always write `CURPOS` **then** `CURBASE`; `CURBASE` is what arms the move.
  A write to *any* cursor register cancels an already-armed update, so never
  leave out the `CURBASE` write after `CURPOS`.
* On plane enable, the `CURCNTR` write itself arms the update.
* Update the watermark/DDB (`skl_write_cursor_wm`) first, then `CUR_FBC_CTL`
  (if changing size), then `CURCNTR`, `CURPOS`, `CURBASE`.

**Sequence for a minimal driver:**
1. (pipe already enabled) program cursor WM/DDB: `CUR_WM(A,0..n)` with
   `CUR_WM_EN`, `CUR_WM_TRANS(A)`, `CUR_BUF_CFG(A)` with a valid DDB slice.
2. `CURBASE(A) = ggtt_addr` (4 KB aligned, ARGB8888) — before enabling.
3. `CUR_CTL` (`0x70080`) = `MCURSOR_MODE_256_ARGB_AX` (or 64/128) |
   `MCURSOR_PIPE_GAMMA_ENABLE`; **not** the old `CURSOR_ENABLE` bit31.
4. `CUR_POS` (`0x70088`) = `CURSOR_POS_X(x)|CURSOR_POS_Y(y)` (+ sign bits);
   `CURBASE = ggtt_addr` to arm.
5. Move: rewrite `CUR_POS` then `CURBASE` (same base if only position changes).
6. Hide: `CURCNTR = MCURSOR_MODE_DISABLE` (0x00); `CURPOS`/`CURBASE` with 0.
   `i9xx_cursor_disable_arm()` just calls `i9xx_cursor_update_arm(..., NULL)`
   (`intel_cursor.c:721-726`), which computes `cntl=base=pos=0` and writes
   `CURCNTR=0, CURPOS=0, CURBASE=0`.
7. Confirm with `CURSURFLIVE` (`0x700ac`) if desired.

Cursor updates in i915 are done as a legacy cursor update with vblank evasion
(`intel_legacy_cursor_update()`, `intel_cursor.c:803-971`; evasion block at
`:914-950`): it calls
`intel_vblank_evade_init()`/`intel_vblank_evade()`, then
`intel_plane_update_noarm()` (WM) + `intel_plane_update_arm()` (CURCNTR/POS/BASE)
inside an IRQ-disabled window, then re-enables IRQs. A minimal driver can do
the same: write the cursor registers inside the vblank window to avoid a
mid-frame tear/one-frame flicker.

---

## APPENDIX — quick offset cheat sheet (pipe A)

```
/* interrupts */
0x44000 DEISR   0x44004 DEIMR   0x44008 DEIIR     0x4400c DEIER     (gen7 legacy)
0x44200 GEN8_MASTER_IRQ                                  bit31 = CONTROL
0x44400 GEN8_DE_PIPE_ISR(A)  0x44404 IMR  0x44408 IIR  0x4440c IER (A)
     bit0=PIPE_VBLANK  bit3=PLANE1_FLIP_DONE  bit31=FIFO_UNDERRUN  bit11=CURSOR_FAULT
0x44440 GEN8_DE_PORT_ISR/IMR/IIR/IER
0x44460 GEN8_DE_MISC_ISR/IMR/IIR/IER
0xc4000 SDEISR  0xc4004 SDEIMR  0xc4008 SDEIIR  0xc400c SDEIER
0x70000 PIPEDSL        0x70024 PIPESTAT       0x70044 PIPEFRAMEPIXEL

/* plane 1 (primary) */
0x70180 PLANE_CTL      0x70188 PLANE_STRIDE    0x7018c PLANE_POS
0x70190 PLANE_SIZE     0x7019c PLANE_SURF      0x701ac PLANE_SURFLIVE
0x701a4 PLANE_OFFSET   0x701c0 PLANE_AUX_DIST  0x701c4 PLANE_AUX_OFFSET

/* cursor */
0x70080 CUR_CTL/CURCNTR  0x70084 CUR_BASE/CURBASE  0x70088 CUR_POS/CURPOS
0x700a0 CUR_FBC_CTL      0x700ac CUR_SURFLIVE
0x70140 CUR_WM(level)    0x70168 CUR_WM_TRANS     0x7017c CUR_BUF_CFG
```
