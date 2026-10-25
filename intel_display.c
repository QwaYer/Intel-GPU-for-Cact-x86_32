/*
 * intel_display.c — chip identification, display power and output probing.
 *
 * Coffee Lake is a DISPLAY_VER 9 part (i915's `skl_display`): the display
 * version is a property of the PCI device id, not something read back from a
 * register, so identification only reads the display-fuse strap (SKL_DFSM) to
 * learn which pipes are enabled.
 *
 * Bringing the display up enough to read EDID means, in order:
 *
 *   1. take the display out of the DC5/DC6 power-saving states (a write to
 *      DC_STATE_EN that blocks until the hardware restores its state);
 *   2. enable the always-on wells (MISC_IO, PW_1) and PW_2, which back the
 *      DDIs other than A;
 *   3. enable the per-DDI IO wells.
 *
 * There is no GMBUS power well on SKL/CFL — GMBUS is covered by the always-on
 * well and only needs its clock ungated around a transfer (workaround #0868,
 * done in intel_gmbus.c).
 */

#include "intel_i915.h"
#include "intel_regs.h"

/* ── register polling helpers ───────────────────────────────────────────── */

/* Wait for `mask` to be fully set in `reg`, polling every 10 us.  Linux uses
 * intel_de_wait_for_set_ms(..., 1) for the SKL power wells and fuses. */
static int intel_wait_set(uint32_t reg, uint32_t mask) {
    for (int i = 0; i < 300; i++) {          /* up to ~3 ms of headroom */
        if ((intel_read(reg) & mask) == mask)
            return 0;
        ktime_busy_wait_us(10);
    }
    return -1;
}

/* gen9_wait_for_power_well_fuses() */
static void intel_wait_fuse(int pg) {
    (void)intel_wait_set(SKL_FUSE_STATUS, SKL_FUSE_PG_DIST_STATUS(pg));
}

/* hsw_power_well_enable() for one well index (no fuses). */
static int intel_well_enable(int idx) {
    intel_write_mask(HSW_PWR_WELL_CTL2, HSW_PWR_WELL_REQ(idx), HSW_PWR_WELL_REQ(idx));
    return intel_wait_set(HSW_PWR_WELL_CTL2, HSW_PWR_WELL_STATE(idx));
}

/* ── identification ─────────────────────────────────────────────────────── */

int intel_display_identify(void) {
    uint32_t dfsm = intel_read(SKL_DFSM);

    /* A BAR that does not decode reads back all ones. */
    if (dfsm == 0xFFFFFFFFu)
        return -1;

    intel.display_ver = INTEL_DISPLAY_VER_CFL;

    int pipes = 0;
    if (!(dfsm & SKL_DFSM_PIPE_A_DISABLE)) pipes++;
    if (!(dfsm & SKL_DFSM_PIPE_B_DISABLE)) pipes++;
    if (!(dfsm & SKL_DFSM_PIPE_C_DISABLE)) pipes++;
    intel.num_pipes = pipes;

    return pipes ? 0 : -1;
}

/* ── power ──────────────────────────────────────────────────────────────── */

/* gen9_set_dc_state(DC_STATE_DISABLE): leaving DC5/DC6 is synchronous — the
 * write only completes once the hardware has restored the display state. */
static void intel_disable_dc_states(void) {
    uint32_t val = intel_read(DC_STATE_EN);
    val &= ~(DC_STATE_UPTO_DC5_DC6_MASK | DC_STATE_EN_DC9);
    intel_write(DC_STATE_EN, val);
}

int intel_display_power_on(void) {
    intel_disable_dc_states();

    /* "always-on" wells: MISC_IO (bits shared with the DDI IO block) and PW_1
     * (the core display well).  PW_1 has PG0/PG1 fuses. */
    intel_wait_fuse(SKL_PG0);
    (void)intel_well_enable(SKL_PW_CTL_IDX_MISC_IO);

    intel_write_mask(HSW_PWR_WELL_CTL2, HSW_PWR_WELL_REQ(SKL_PW_CTL_IDX_PW_1),
                     HSW_PWR_WELL_REQ(SKL_PW_CTL_IDX_PW_1));
    (void)intel_wait_set(HSW_PWR_WELL_CTL2, HSW_PWR_WELL_STATE(SKL_PW_CTL_IDX_PW_1));
    intel_wait_fuse(SKL_PG1);

    /* PW_2 backs DDI B/C/D lanes, their AUX channels and the DDI IO block. */
    if (intel_well_enable(SKL_PW_CTL_IDX_PW_2) == 0)
        intel_wait_fuse(SKL_PG2);

    /* Per-DDI IO wells (A/E share index 1). */
    (void)intel_well_enable(SKL_PW_CTL_IDX_DDI_A_E);
    (void)intel_well_enable(SKL_PW_CTL_IDX_DDI_B);
    (void)intel_well_enable(SKL_PW_CTL_IDX_DDI_C);
    (void)intel_well_enable(SKL_PW_CTL_IDX_DDI_D);

    return 0;
}

/*
 * Power down.  Deliberately a no-op: the firmware booted with a display
 * programmed, and dropping the wells on unload could blank a monitor the user
 * is looking at.  Re-enabling is cheap if the module is loaded again.
 */
void intel_display_power_off(void) {
}

/* ── port probing ───────────────────────────────────────────────────────── */

/* PCH hotplug status bit for a DDI (hpd_spt on CNP). */
static int intel_port_hpd_bit(int port) {
    switch (port) {
    case 0: return SDE_PORTA_HOTPLUG_SPT;
    case 1: return SDE_PORTB_HOTPLUG_CPT;
    case 2: return SDE_PORTC_HOTPLUG_CPT;
    case 3: return SDE_PORTD_HOTPLUG_CPT;
    default: return 0;
    }
}

/* Platform default DDC pin for a port when there is no VBT:
 * cnp_encoder_to_ddc_pin(): B -> PIN_1, C -> PIN_2, D -> PIN_4; A has no case
 * in the upstream switch and falls to the default, PIN_1. */
static int intel_port_default_ddc_pin(int port) {
    switch (port) {
    case 1: return GMBUS_PIN_1;
    case 2: return GMBUS_PIN_2;
    case 3: return GMBUS_PIN_4;
    default: return GMBUS_PIN_1;
    }
}

int intel_probe_ports(void) {
    static const int pins_to_try[] = { GMBUS_PIN_1, GMBUS_PIN_2, GMBUS_PIN_3, GMBUS_PIN_4 };

    int found = 0;
    int type_id[2] = { 0, 0 };   /* [0] = HDMIA, [1] = DisplayPort */

    for (int port = 0; port < INTEL_MAX_PORTS; port++) {
        uint32_t hpd = intel_port_hpd_bit(port);
        if (!(intel_read(SDEISR) & hpd))
            continue;

        /* Keep the DDI index: the transcoder/port we later program is named by
         * it, so compacting the array would move a sink onto the wrong DDI. */
        intel_port_t *p = &intel.ports[port];
        p->present = 1;

        /* A native DisplayPort sink answers on AUX; an HDMI/DVI sink only on
         * DDC.  Try AUX first, then the platform-default pin, then the rest of
         * the pin set (a monitor on a non-default pin is not unusual). */
        int len = intel_aux_read_edid(port, p->edid, (int)sizeof(p->edid));
        if (len > 0) {
            p->is_dp    = 1;
            p->via_aux  = 1;
        } else {
            int def = intel_port_default_ddc_pin(port);
            len = intel_gmbus_read_edid(def, p->edid, (int)sizeof(p->edid));
            if (len > 0) {
                p->ddc_pin = def;
            } else {
                for (unsigned i = 0; i < sizeof(pins_to_try) / sizeof(pins_to_try[0]); i++) {
                    if (pins_to_try[i] == def)
                        continue;
                    len = intel_gmbus_read_edid(pins_to_try[i], p->edid,
                                                (int)sizeof(p->edid));
                    if (len > 0) {
                        p->ddc_pin = pins_to_try[i];
                        break;
                    }
                }
            }
            if (len > 0)
                p->is_dp = 0;
        }
        if (len > 0)
            p->edid_len = len;

        /* A port that answers on DDC (without AUX) is treated as HDMI/DVI:
         * both are TMDS and share the HDMIA connector type. */
        p->connector_type = p->is_dp ? 10 /* DRM_MODE_CONNECTOR_DisplayPort */
                                     : 11 /* DRM_MODE_CONNECTOR_HDMIA */;
        p->type_id = (uint32_t)(++type_id[p->is_dp ? 1 : 0]);

        found++;
    }

    intel.num_ports = found;
    return 0;
}
