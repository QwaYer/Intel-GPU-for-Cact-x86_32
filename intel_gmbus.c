/*
 * intel_gmbus.c — reading an EDID block off a display sink.
 *
 * Two transports reach the same EDID:
 *
 *   - GMBUS/DDC, the I2C controller in the PCH south display, used by
 *     HDMI/DVI sinks.  A transfer is "program GMBUS0 pin + rate, issue an
 *     INDEX+READ command in GMBUS1, drain four bytes at a time from GMBUS3
 *     while HW_RDY comes and goes, then STOP".
 *   - DP AUX, used by native DisplayPort sinks.  The EDID is read over
 *     I2C-over-AUX: a header is written into the five data registers and the
 *     request goes out through DP_AUX_CH_CTL.
 *
 * Sequences and register semantics follow the i915 driver
 * (display/intel_gmbus.c, display/intel_dp_aux.c); the constants are in
 * intel_regs.h with the upstream names.
 */

#include "intel_i915.h"
#include "intel_regs.h"

/* ── shared ─────────────────────────────────────────────────────────────── */

static int edid_header_ok(const uint8_t *b) {
    static const uint8_t magic[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    for (int i = 0; i < 8; i++)
        if (b[i] != magic[i])
            return 0;
    return 1;
}

/* ── GMBUS / DDC ────────────────────────────────────────────────────────── */

/* Workaround #0868: GMBUS needs its clock ungated while it runs.  The bit is
 * named "clock gate disable", so setting it turns gating *off*. */
static void gmbus_clock_gating(int disabled) {
    intel_write_mask(SOUTH_DSPCLK_GATE_D, PCH_GMBUSUNIT_CLOCK_GATE_DISABLE,
                     disabled ? PCH_GMBUSUNIT_CLOCK_GATE_DISABLE : 0);
}

/* Wait for one 4-byte FIFO step.  The budget is per *read*, not per transfer:
 * at 100 kHz a 4-byte step takes well under a millisecond, so 2 ms is already
 * generous, and a full 128-byte read can then fail in ~64 ms rather than
 * blocking the probe for seconds when the bus simply is not there. */
static int gmbus_wait(uint32_t status) {
    for (int i = 0; i < 40; i++) {           /* 2 ms at 50 us/step */
        uint32_t s = intel_read(GMBUS2);
        if (s & GMBUS_SATOER)
            return -1;                        /* target NAKed */
        if (s & status)
            return 0;
        ktime_busy_wait_us(50);
    }
    return -1;
}

static int gmbus_wait_idle(void) {
    for (int i = 0; i < 200; i++) {           /* 10 ms */
        if (!(intel_read(GMBUS2) & GMBUS_ACTIVE))
            return 0;
        ktime_busy_wait_us(50);
    }
    return -1;
}

static void gmbus_reset(void) {
    intel_write(GMBUS1, GMBUS_SW_CLR_INT);    /* toggle the software clear */
    intel_write(GMBUS1, 0);
}

/*
 * Read `len` bytes (<= 511) starting at I2C register index `off` from slave
 * `addr`, using the GMBUS index cycle.  A 1-byte index goes through GMBUS1;
 * a larger one needs the 2-byte index in GMBUS5.
 */
static int gmbus_read(int pin, uint8_t addr, uint32_t off,
                      uint8_t *buf, int len) {
    uint32_t index = 0;
    int two_byte = off > 0xFF;

    if (pin < GMBUS_PIN_1 || pin > GMBUS_PIN_4)
        return -1;

    gmbus_reset();
    intel_write(GMBUS0, (uint32_t)pin | GMBUS_RATE_100KHZ);

    if (two_byte) {
        intel_write(GMBUS5, GMBUS_2BYTE_INDEX_EN | (off & 0xFFu) | ((off >> 8) << 8));
    } else {
        index = GMBUS_CYCLE_INDEX | (off << GMBUS_SLAVE_INDEX_SHIFT);
    }

    intel_write(GMBUS1,
                index | GMBUS_CYCLE_WAIT | ((uint32_t)len << GMBUS_BYTE_COUNT_SHIFT) |
                ((uint32_t)addr << GMBUS_SLAVE_ADDR_SHIFT) | GMBUS_SLAVE_READ |
                GMBUS_SW_RDY);

    int got = 0;
    while (got < len) {
        if (gmbus_wait(GMBUS_HW_RDY) != 0)
            goto err;
        uint32_t v = intel_read(GMBUS3);
        for (int j = 0; j < 4 && got < len; j++) {
            buf[got++] = (uint8_t)(v & 0xFF);
            v >>= 8;
        }
    }

    intel_write(GMBUS1, GMBUS_CYCLE_STOP | GMBUS_SW_RDY);
    if (gmbus_wait_idle() != 0)
        goto err;

    if (two_byte)
        intel_write(GMBUS5, 0);
    intel_write(GMBUS0, 0);
    return len;

err:
    (void)gmbus_wait_idle();
    if (two_byte)
        intel_write(GMBUS5, 0);
    gmbus_reset();
    intel_write(GMBUS0, 0);
    return -1;
}

int intel_gmbus_read_edid(int pin, uint8_t *buf, int buf_len) {
    if (!buf || buf_len < 128)
        return -1;

    gmbus_clock_gating(1);

    int rc = gmbus_read(pin, 0x50, 0, buf, 128);
    if (rc < 0 || !edid_header_ok(buf)) {
        gmbus_clock_gating(0);
        return -1;
    }

    /* Extensions (CEA-861 and friends) follow the base block; read as many as
     * fit.  Their block count is byte 126 of the base block. */
    int total = 128;
    int ext = buf[126];
    if (ext > 3)
        ext = 3;
    for (int i = 0; i < ext && total + 128 <= buf_len; i++) {
        if (gmbus_read(pin, 0x50, (uint32_t)(128 * (i + 1)), buf + total, 128) < 0)
            break;
        total += 128;
    }

    gmbus_clock_gating(0);
    return total;
}

/* ── DP AUX ─────────────────────────────────────────────────────────────── */

static int aux_wait_done(uint32_t ctl) {
    for (int i = 0; i < 1000; i++) {          /* 10 ms at 10 us/step */
        if (!(intel_read(ctl) & DP_AUX_CH_CTL_SEND_BUSY))
            return 0;
        ktime_busy_wait_us(10);
    }
    return -1;
}

/* One AUX transaction.  Returns the number of reply bytes (including the
 * reply header byte) or a negative error. */
static int aux_xfer(int aux_ch, const uint8_t *send, int send_len,
                    uint8_t *recv, int recv_len) {
    uint32_t ctl = DP_AUX_CH_CTL(aux_ch);

    /* Wait out any previous transaction. */
    for (int i = 0; i < 3; i++) {
        if (!(intel_read(ctl) & DP_AUX_CH_CTL_SEND_BUSY))
            break;
        ktime_busy_wait_us(1000);
    }
    if (intel_read(ctl) & DP_AUX_CH_CTL_SEND_BUSY)
        return -1;

    /* Bytes are packed big-endian into the 32-bit data registers. */
    for (int i = 0; i < send_len; i += 4) {
        uint32_t v = 0;
        for (int j = 0; j < 4 && i + j < send_len; j++)
            v |= (uint32_t)send[i + j] << ((3 - j) * 8);
        intel_write(DP_AUX_CH_DATA(aux_ch, i >> 2), v);
    }

    uint32_t send_ctl = DP_AUX_CH_CTL_SEND_BUSY | DP_AUX_CH_CTL_DONE |
                        DP_AUX_CH_CTL_INTERRUPT | DP_AUX_CH_CTL_TIME_OUT_ERROR |
                        DP_AUX_CH_CTL_TIME_OUT_MAX | DP_AUX_CH_CTL_RECEIVE_ERROR |
                        DP_AUX_CH_CTL_MESSAGE_SIZE(send_len) |
                        DP_AUX_CH_CTL_FW_SYNC_PULSE(INTEL_AUX_FW_SYNC_LEN) |
                        DP_AUX_CH_CTL_SYNC_PULSE(INTEL_AUX_SYNC_LEN);
    intel_write(ctl, send_ctl);

    if (aux_wait_done(ctl) != 0)
        return -1;

    uint32_t status = intel_read(ctl);
    intel_write(ctl, status | DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR |
                          DP_AUX_CH_CTL_RECEIVE_ERROR);

    if (status & DP_AUX_CH_CTL_TIME_OUT_ERROR)
        return -1;
    if (status & DP_AUX_CH_CTL_RECEIVE_ERROR)
        return -1;
    if (!(status & DP_AUX_CH_CTL_DONE))
        return -1;

    int n = (int)((status & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >> 20);
    if (n > recv_len)
        n = recv_len;
    for (int i = 0; i < n; i += 4) {
        uint32_t v = intel_read(DP_AUX_CH_DATA(aux_ch, i >> 2));
        for (int j = 0; j < 4 && i + j < n; j++)
            recv[i + j] = (uint8_t)(v >> ((3 - j) * 8));
    }
    return n;
}

/* Build the 4-byte AUX header shared by every I2C-over-AUX transfer. */
static void aux_i2c_header(uint8_t hdr[4], uint8_t request, uint8_t addr, int size) {
    hdr[0] = (uint8_t)((request << 4) | ((addr >> 16) & 0x0F));
    hdr[1] = (uint8_t)((addr >> 8) & 0xFF);
    hdr[2] = (uint8_t)(addr & 0xFF);
    hdr[3] = (uint8_t)(size - 1);
}

static int aux_i2c_write(int aux_ch, uint8_t addr, const uint8_t *data, int len,
                         int mot) {
    uint8_t tx[20];
    uint8_t rx[2];
    aux_i2c_header(tx, (uint8_t)(DP_AUX_I2C_WRITE | (mot ? DP_AUX_I2C_MOT : 0)),
                   addr, len);
    for (int i = 0; i < len; i++)
        tx[4 + i] = data[i];

    for (int try = 0; try < 3; try++) {
        int r = aux_xfer(aux_ch, tx, 4 + len, rx, (int)sizeof(rx));
        if (r >= 1 && (rx[0] >> 4) == 0)
            return 0;
        ktime_busy_wait_us(400);
    }
    return -1;
}

static int aux_i2c_read(int aux_ch, uint8_t addr, int len, uint8_t *out, int mot) {
    uint8_t tx[4];
    uint8_t rx[20];
    aux_i2c_header(tx, (uint8_t)(DP_AUX_I2C_READ | (mot ? DP_AUX_I2C_MOT : 0)),
                   addr, len);

    for (int try = 0; try < 3; try++) {
        int r = aux_xfer(aux_ch, tx, 4, rx, (int)sizeof(rx));
        if (r >= 1 && (rx[0] >> 4) == 0) {
            int data = r - 1;
            if (data > len)
                data = len;
            for (int i = 0; i < data; i++)
                out[i] = rx[i + 1];
            return data;
        }
        ktime_busy_wait_us(400);
    }
    return -1;
}

int intel_aux_read_edid(int aux_ch, uint8_t *buf, int buf_len) {
    if (!buf || buf_len < 128 || aux_ch < 0 || aux_ch > 3)
        return -1;

    /* Start an I2C transaction: write the EDID register offset 0, with the
     * middle-of-transaction bit set so the following reads continue it. */
    uint8_t offset = 0x00;
    if (aux_i2c_write(aux_ch, 0x50, &offset, 1, 1) != 0)
        return -1;

    int got = 0;
    while (got < 128) {
        int chunk = 128 - got;
        if (chunk > 16)
            chunk = 16;
        int last = (got + chunk >= 128);
        int r = aux_i2c_read(aux_ch, 0x50, chunk, buf + got, last ? 0 : 1);
        if (r <= 0)
            return -1;
        got += r;
    }

    if (!edid_header_ok(buf))
        return -1;
    return 128;
}
