# Intel-GPU-for-Cact-x86_32

An out-of-tree **`.cctk`** driver that brings Intel integrated graphics to
[CactKernel-x86_32](../CactKernel-x86_32): the display engine of a **Gen9.5 /
Coffee Lake** part — "UHD Graphics 630" (i3-8100 / i5-8400 / i7-8700 class) —
is discovered and registered with the kernel's generic DRM/KMS core.

The driver is the C side of the split the kernel already uses for GPUs: the DRM
core (GEM, KMS objects, ioctl decoding, planes, cursors, atomic, syncobj) lives
**in-tree** in Rust; a hardware driver is a signed `.cctk` module that
implements `drm_driver_ops_t` and drives its own registers. `virtio_gpu.cctk`
does this for virtio-gpu; this repo does it for Intel.

## What works (stage 1)

`modload /lib/i915.cctk` on a Coffee Lake machine:

1. binds the VGA function by vendor/device id (`8086:3E91` on an i3-8100),
   maps the GTTMMADR BAR uncached and reads back the display version;
2. brings up the display and DDI power wells;
3. probes DDI A–D, reads each sink's EDID over **GMBUS/DDC** (HDMI/DVI) or
   **DP AUX** (DisplayPort);
4. publishes `/dev/dri/cardN` with one connector per connected output, each
   carrying the modes and EDID the sink reports.

**The display is driven by the driver (stage 3 + 4).** On load it finds the
transcoder already serving the connected DDI, programs CDCLK and the HDMI WRPLL
from scratch, routes the DPLL, and takes over the pipe/encoder/plane — keeping
the firmware's plane buffer (the boot framebuffer is an *aperture* address, not
physical memory, so it cannot be re-mapped as pages).  `set_config()` performs a
full modeset for a client framebuffer (linear, 16/32 bpp), an MMIO page flip
re-points the primary plane, and a real vblank MSI plus the SKL hardware cursor
are wired up.

## Stages

| Stage | Scope | State |
|-------|-------|-------|
| 1 | Bind, identify, power wells, EDID, DRM connectors + modes | done |
| 2 | DP/eDP AUX link training, HDMI DDC pin resolution | planned |
| 3 | CDCLK/DPLL, DDI, pipe/transcoder/plane, GGTT scanout, console handoff | done — pipe/plane/DDI/GGTT scanout + console handoff + CDCLK/WRPLL programmed from scratch |
| 4 | Vblank IRQ, page flip, cursor plane | implemented — real vblank MSI + `GEN8_DE_PIPE_IIR`, MMIO page flip, SKL hardware cursor (not yet exercised: CactOS had no DRM client; `drmtest` in the image can drive them) |
| 5 | Render engine (GuC/HuC, execbuf) — likely out of scope | not planned |

## Kernel prerequisites

Stage 1 needs nothing new from the kernel beyond the DRM core API it already
exports through `ksym`. Stage 3 needs **`drm_gem_page_phys()` /
`drm_gem_page_count()`** (physical backing of a GEM object, so the driver can
build GGTT page-table entries); those were added to the kernel alongside this
repo and are exported in `kernel/elf/ksym.c`.

## Building

```sh
meson setup build-meson --cross-file cross/i686-cact-clang.ini \
    -Dkern_root=../CactKernel-x86_32 -Dlocal_repo=../LocalRepoCactOS-x86_32
ninja -C build-meson          # build-meson/i915.cctk
ninja -C build-meson stage    # copy into <local_repo>/lib/
```

The `.cctk` must be **ECDSA P-256 signed** before the kernel will load it; the
LocalRepo's signing step (`tools/cact_sign.py` per file, then
`tools/pack_cctkfs.py`) does that, after which a full ISO is built with
`ninja -C ../CactKernel-x86_32/build-meson iso-full`.

## Testing on the target machine

QEMU does not emulate an Intel GPU, so the driver can only be exercised on real
hardware (the i3-8100 machine — **not** the machine this repo is developed on).
Boot the full ISO there and:

```
modload /usr/lib/modules/i915.cctk
# or, since libfs searches the module directory:  modload i915.cctk
```

then read the kernel log. A healthy load prints one line per connected output
(DDI letter, DP/HDMI, EDID source, the sink's PNP id and mode count) followed by
`i915: 8086:3e91 display ver 10, N port(s) — /dev/dri ready`. `/dev/dri/card0`
can then be queried with a DRM client.

## References

Register offsets and bring-up sequences are ported from the Linux i915 driver
(`drivers/gpu/drm/i915/`), and each constant in `intel_regs.h` carries the
Linux name it was taken from so it can be re-checked against the upstream
source.
