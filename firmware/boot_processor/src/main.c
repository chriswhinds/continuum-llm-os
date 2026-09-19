/* Continuum boot processor firmware (ARCH-002 §03) -- runs on the RP2040
 * (Raspberry Pi Pico) wired to one Raspberry Pi 5 node. Bare metal, no
 * RTOS: a single superloop plus a UART RX interrupt, matching ARCH-002's
 * ~700 LOC / "no RTOS needed at this scope" scope note.
 *
 * Responsibilities:
 *   1. Power sequencing: hold the Pi's power rail off briefly at boot
 *      (self-check window), then enable it.
 *   2. Console capture: every byte from the Pi's UART console lands in a
 *      4KB ring buffer (console_ring.c) for post-mortem reads.
 *   3. Heartbeat watchdog: continuumd writes one byte to this same UART
 *      every heartbeat_interval_ms (ARCH-002 §04's continuumd.conf,
 *      default 2000ms). Three missed intervals (see HEARTBEAT_TIMEOUT_MS
 *      below) cuts and re-enables the Pi's power rail -- a hard reset.
 *   4. Status register: boot count, last reset reason, uptime, exposed
 *      over I2C (i2c_status.c) so it's readable even with the Pi's own
 *      network stack dead.
 *   5. Self-supervision: the RP2040's own hardware watchdog resets THIS
 *      chip if the superloop ever hangs -- independent of and unrelated
 *      to the Pi-power-cycle watchdog above.
 *
 * Wiring (documented here since there's no schematic elsewhere in this
 * repo -- see ARCH-002 §01/§03 for the reference-hardware framing):
 *   GPIO 0  (UART0 TX)  -> Pi UART RX
 *   GPIO 1  (UART0 RX)  <- Pi UART TX
 *   GPIO 2  (out)       -> power-control MOSFET/relay gate (Pi's USB-C
 *                          power input; HIGH = rail enabled)
 *   GPIO 4  (I2C0 SDA), GPIO 5 (I2C0 SCL) -> optional external I2C bus
 *   GPIO 25 (onboard LED) -> blinks steadily while healthy, solid while
 *                             a power-cycle is in progress
 *
 * Build/flash: see CMakeLists.txt in this directory. Untested against
 * real hardware or a real pico-sdk checkout -- this environment has
 * neither (see the module comment in i2c_status.c).
 */
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "console_ring.h"
#include "i2c_status.h"

#define PI_UART        uart0
#define PI_UART_TX_PIN 0
#define PI_UART_RX_PIN 1
#define PI_UART_BAUD   115200

#define POWER_ENABLE_PIN 2
#define STATUS_LED_PIN   25

#define I2C_PORT      i2c0
#define I2C_SDA_PIN   4
#define I2C_SCL_PIN   5
#define I2C_SLAVE_ADDR 0x42

/* continuumd's default heartbeat_interval_ms (kernel/include/config.h) is
 * 2000ms. "Three missed intervals" per ARCH-002 §03 -- with a little
 * headroom for jitter, since a single late heartbeat under load
 * shouldn't trigger a hard reset. */
#define HEARTBEAT_INTERVAL_MS   2000u
#define HEARTBEAT_TIMEOUT_MS    (3u * HEARTBEAT_INTERVAL_MS + 500u)

#define POWERON_SELFCHECK_MS 200u
#define POWER_CYCLE_OFF_MS   2000u

#define PICO_SELF_WATCHDOG_MS 8000u

static console_ring_t g_console;
static i2c_status_regs_t g_status = {0};
static volatile uint64_t g_last_uart_activity_us;

static void pi_uart_rx_irq_handler(void) {
    while (uart_is_readable(PI_UART)) {
        uint8_t byte = (uint8_t)uart_getc(PI_UART);
        console_ring_push(&g_console, byte);
        /* See the module comment: any UART activity (kernel console
         * spam or a continuumd heartbeat byte) counts as "the Pi is
         * alive," not just a specific heartbeat framing -- simpler than
         * parsing a heartbeat protocol out of the stream, and it also
         * catches a Pi stuck boot-looping before continuumd ever runs,
         * which a heartbeat-only check would miss entirely. */
        g_last_uart_activity_us = time_us_64();
    }
}

static void set_pi_power(bool on) {
    gpio_put(POWER_ENABLE_PIN, on);
}

static void power_cycle_pi(reset_reason_t reason) {
    gpio_put(STATUS_LED_PIN, 1); /* solid LED = power-cycle in progress */

    set_pi_power(false);
    sleep_ms(POWER_CYCLE_OFF_MS);
    set_pi_power(true);

    g_status.boot_count++;
    g_status.last_reset_reason = reason;
    g_last_uart_activity_us = time_us_64(); /* restart the heartbeat clock */
}

int main(void) {
    stdio_init_all();

    gpio_init(POWER_ENABLE_PIN);
    gpio_set_dir(POWER_ENABLE_PIN, GPIO_OUT);
    set_pi_power(false); /* hold the rail off during our own self-check window */

    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);

    console_ring_init(&g_console);

    uart_init(PI_UART, PI_UART_BAUD);
    gpio_set_function(PI_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PI_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(PI_UART, false, false);
    uart_set_format(PI_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(PI_UART, false); /* one byte at a time keeps console_ring_push() simple */

    int uart_irq_num = (PI_UART == uart0) ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(uart_irq_num, pi_uart_rx_irq_handler);
    irq_set_enabled(uart_irq_num, true);
    uart_set_irq_enables(PI_UART, true, false); /* RX interrupt only -- we don't use UART TX */

    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    i2c_status_init(I2C_PORT, I2C_SLAVE_ADDR, &g_status);

    /* Self-check window per ARCH-002 §03: a fixed settle delay stands in
     * for "and self-check" -- this firmware doesn't yet measure the
     * Pi's own boot image before releasing power (e.g. a signature/hash
     * check), which would be the natural next step toward the doc's
     * "measured boot" framing; flagged here rather than silently
     * skipped. */
    sleep_ms(POWERON_SELFCHECK_MS);

    watchdog_enable(PICO_SELF_WATCHDOG_MS, true);

    g_status.boot_count = 1;
    g_status.last_reset_reason = RESET_REASON_POWERON;
    set_pi_power(true);
    g_last_uart_activity_us = time_us_64();

    uint64_t start_us = time_us_64();
    uint64_t last_led_toggle_us = start_us;
    bool led_on = false;

    for (;;) {
        watchdog_update(); /* pet the Pico's own hardware watchdog every loop */

        g_status.uptime_seconds = (uint32_t)((time_us_64() - start_us) / 1000000ull);

        uint64_t now_us = time_us_64();
        uint64_t since_activity_ms = (now_us - g_last_uart_activity_us) / 1000ull;
        if (since_activity_ms > HEARTBEAT_TIMEOUT_MS) {
            power_cycle_pi(RESET_REASON_WATCHDOG_MISSED_HEARTBEAT);
        }

        /* Slow heartbeat blink while healthy -- a operator glancing at
         * the board sees "alive," not a specific status (that's what
         * the I2C register and the cluster console are for). */
        if (now_us - last_led_toggle_us > 500000ull) {
            led_on = !led_on;
            gpio_put(STATUS_LED_PIN, led_on);
            last_led_toggle_us = now_us;
        }

        sleep_ms(50);
    }
}
