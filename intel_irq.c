/*
 * intel_irq.c — gen9 display pipe vblank interrupt.
 *
 * Gen9 has no DEIIR/PIPESTAT vblank: a CPU pipe's vblank is bit 0 of
 * GEN8_DE_PIPE_IIR(pipe) (0x44408), gated by the per-pipe IER/IMR and the
 * GEN8_MASTER_IRQ master bit.  The handler acks the pipe IIR and bumps the
 * CRTC's vblank counter via drm_crtc_handle_vblank().  Register offsets and the
 * ack/ordering rules come from Linux i915 (see STAGE4_LINUX_NOTES.md).
 */

#include <stddef.h>
#include "intel_i915.h"
#include "intel_regs.h"

#define INTEL_VBLANK_PIPE   0

static int              intel_irq_vec = -1;
static struct drm_crtc *intel_irq_crtc;

void intel_irq_set_crtc(struct drm_crtc *crtc) {
    intel_irq_crtc = crtc;
}

/* Shared MSI handler for the GPU PCI function.  Only the display pipe interrupt
 * is enabled, so a vblank is the expected source; anything else is ignored. */
static void intel_gpu_irq(void) {
    intel_write(GEN8_MASTER_IRQ, 0);                 /* mask the master while we work */
    uint32_t master = intel_read(GEN8_MASTER_IRQ);

    if (master & GEN8_DE_PIPE_IRQ(INTEL_VBLANK_PIPE)) {
        uint32_t iir = intel_read(GEN8_DE_PIPE_IIR(INTEL_VBLANK_PIPE));
        if (iir) {
            intel_write(GEN8_DE_PIPE_IIR(INTEL_VBLANK_PIPE), iir);   /* ack: write it back */
            (void)intel_read(GEN8_DE_PIPE_IIR(INTEL_VBLANK_PIPE));
            if ((iir & GEN8_PIPE_VBLANK) && intel_irq_crtc)
                drm_crtc_handle_vblank(intel_irq_crtc);
        }
    }

    intel_write(GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL);   /* re-enable last */
    (void)intel_read(GEN8_MASTER_IRQ);
}

int intel_irq_init(pci_device_t *pdev) {
    /* Arm the sources in IER, keep everything masked in IMR (enable_vblank
     * unmasks vblank when a client asks for it). */
    intel_write(GEN8_DE_PIPE_IER(INTEL_VBLANK_PIPE),
                GEN8_PIPE_VBLANK | GEN8_PIPE_FIFO_UNDERRUN |
                GEN9_PIPE_PLANE1_FLIP_DONE | GEN9_PIPE_CURSOR_FAULT);
    intel_write(GEN8_DE_PIPE_IMR(INTEL_VBLANK_PIPE), 0xffffffffu);
    (void)intel_read(GEN8_DE_PIPE_IER(INTEL_VBLANK_PIPE));

    intel_irq_vec = msidev_register(pdev, intel_gpu_irq);
    if (intel_irq_vec < 0) {
        printk("i915: could not register the display MSI handler\n");
        return -1;
    }
    return 0;
}

void intel_irq_fini(void) {
    if (intel_irq_vec >= 0) {
        intel_write(GEN8_DE_PIPE_IMR(INTEL_VBLANK_PIPE), 0xffffffffu);
        msidev_unregister(intel_irq_vec);
        intel_irq_vec = -1;
    }
    intel_irq_crtc = NULL;
}

int intel_irq_enable_vblank(struct drm_device *dev, struct drm_crtc *crtc) {
    (void)dev;
    intel_irq_crtc = crtc;
    intel_write_mask(GEN8_DE_PIPE_IER(INTEL_VBLANK_PIPE), GEN8_PIPE_VBLANK, GEN8_PIPE_VBLANK);
    intel_write_mask(GEN8_DE_PIPE_IMR(INTEL_VBLANK_PIPE), GEN8_PIPE_VBLANK, 0);
    (void)intel_read(GEN8_DE_PIPE_IMR(INTEL_VBLANK_PIPE));
    intel_write_mask(GEN8_MASTER_IRQ, GEN8_MASTER_IRQ_CONTROL, GEN8_MASTER_IRQ_CONTROL);
    (void)intel_read(GEN8_MASTER_IRQ);
    return 0;
}

void intel_irq_disable_vblank(struct drm_device *dev, struct drm_crtc *crtc) {
    (void)dev;
    (void)crtc;
    intel_write_mask(GEN8_DE_PIPE_IMR(INTEL_VBLANK_PIPE), GEN8_PIPE_VBLANK, GEN8_PIPE_VBLANK);
    (void)intel_read(GEN8_DE_PIPE_IMR(INTEL_VBLANK_PIPE));
}
