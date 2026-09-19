/* i2c_status.h -- a small read-only I2C slave register file (ARCH-002
 * §03) exposing this node's boot count, last reset reason, and uptime --
 * readable by anything on the bus even if the Pi's own network stack is
 * dead, since it lives entirely on the boot processor.
 *
 * Register map (one byte selects which register a subsequent read
 * returns, matching the common "memory device" I2C slave pattern -- an
 * external master writes a single register-index byte, then reads back
 * that register's bytes):
 *
 *   0x00        last_reset_reason (1 byte, reset_reason_t)
 *   0x01 - 0x04 boot_count        (4 bytes, little-endian u32)
 *   0x05 - 0x08 uptime_seconds    (4 bytes, little-endian u32)
 */
#ifndef I2C_STATUS_H
#define I2C_STATUS_H

#include <stdint.h>

#include "hardware/i2c.h"

typedef enum {
    RESET_REASON_POWERON = 0,
    RESET_REASON_WATCHDOG_MISSED_HEARTBEAT = 1,
    RESET_REASON_MANUAL = 2,
} reset_reason_t;

typedef struct {
    reset_reason_t last_reset_reason;
    uint32_t boot_count;
    uint32_t uptime_seconds;
} i2c_status_regs_t;

/* Configures `i2c` as an I2C slave at `addr` and installs its interrupt
 * handler. `regs` must outlive the firmware's run (a static/global is
 * expected) -- the handler reads it directly on every register access,
 * so main.c just updates the struct's fields and the next I2C read picks
 * up the new value with no further calls needed. */
void i2c_status_init(i2c_inst_t *i2c, uint8_t addr, i2c_status_regs_t *regs);

#endif
