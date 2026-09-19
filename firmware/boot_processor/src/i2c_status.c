/* i2c_status.c -- implements the "memory device" I2C slave pattern from
 * the RP2040 SDK's own i2c/slave_mem_i2c example (an external master
 * writes one register-index byte, then reads that register back): here
 * adapted to a small READ-ONLY status register file instead of a general
 * read/write memory, since nothing external should be able to mutate a
 * node's own reset-reason/boot-count bookkeeping.
 *
 * Build note: this firmware has never been compiled against a real Pico
 * SDK checkout in this environment (no arm-none-eabi toolchain or
 * pico-sdk available here -- see firmware/boot_processor/CMakeLists.txt).
 * The interrupt-status register names below (I2C_IC_INTR_MASK_M_*,
 * hw->clr_*) match the SDK's hardware/regs/i2c.h as of the SDK versions
 * this was written against; double-check them against whichever pico-sdk
 * version is actually installed before flashing.
 */
#include "i2c_status.h"

#include <string.h>

#include "hardware/irq.h"
#include "hardware/regs/i2c.h"

static i2c_inst_t *s_i2c;
static i2c_status_regs_t *s_regs;
static uint8_t s_selected_reg;
static int s_have_reg_select;

static uint8_t read_register_byte(uint8_t reg_index) {
    uint8_t snapshot[9];
    snapshot[0] = (uint8_t)s_regs->last_reset_reason;
    memcpy(&snapshot[1], &s_regs->boot_count, 4);
    memcpy(&snapshot[5], &s_regs->uptime_seconds, 4);

    if (reg_index >= sizeof(snapshot)) return 0xFF; /* out of range -- report a sentinel, not garbage */
    return snapshot[reg_index];
}

static void i2c_status_irq_handler(void) {
    i2c_hw_t *hw = s_i2c->hw;
    uint32_t status = hw->intr_stat;

    if (status & I2C_IC_INTR_STAT_R_RX_FULL_BITS) {
        /* Master wrote a byte: the very first byte of a transaction
         * selects which register subsequent reads return. */
        uint8_t data = (uint8_t)hw->data_cmd;
        if (!s_have_reg_select) {
            s_selected_reg = data;
            s_have_reg_select = 1;
        }
        /* Bytes after the first (a master writing, not reading, more
         * than one byte) are simply discarded -- this device has no
         * writable registers. */
    }

    if (status & I2C_IC_INTR_STAT_R_RD_REQ_BITS) {
        uint8_t value = read_register_byte(s_selected_reg);
        hw->data_cmd = value;
        s_selected_reg++; /* sequential reads walk forward through the register map */
        (void)hw->clr_rd_req;
    }

    if (status & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
        s_have_reg_select = 0; /* next transaction must start with a fresh register-select byte */
        (void)hw->clr_stop_det;
    }
}

void i2c_status_init(i2c_inst_t *i2c, uint8_t addr, i2c_status_regs_t *regs) {
    s_i2c = i2c;
    s_regs = regs;
    s_selected_reg = 0;
    s_have_reg_select = 0;

    i2c_init(i2c, 100 * 1000); /* standard-mode 100kHz -- plenty for a status register nobody polls quickly */
    i2c_set_slave_mode(i2c, true, addr);

    i2c->hw->intr_mask = I2C_IC_INTR_MASK_M_RD_REQ_BITS | I2C_IC_INTR_MASK_M_RX_FULL_BITS |
                          I2C_IC_INTR_MASK_M_STOP_DET_BITS;

    int irq_num = (i2c == i2c0) ? I2C0_IRQ : I2C1_IRQ;
    irq_set_exclusive_handler(irq_num, i2c_status_irq_handler);
    irq_set_enabled(irq_num, true);
}
