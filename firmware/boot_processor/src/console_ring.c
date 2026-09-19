#include "console_ring.h"

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

void console_ring_init(console_ring_t *r) {
    r->head = 0;
    r->count = 0;
}

void console_ring_push(console_ring_t *r, uint8_t byte) {
    r->buf[r->head] = byte;
    r->head = (uint16_t)((r->head + 1) % CONSOLE_RING_SIZE);
    if (r->count < CONSOLE_RING_SIZE) r->count++;
}

uint16_t console_ring_read(console_ring_t *r, uint8_t *out, uint16_t max_len) {
    /* Interrupts off for the duration of the read: console_ring_push()
     * runs in the UART RX ISR and must not interleave with this. The
     * capture stays lossless either way -- new bytes queue in the UART's
     * own hardware FIFO while interrupts are briefly masked -- this just
     * needs to be short, and a 4KB memcpy at UART byte rates is. */
    uint32_t saved = save_and_disable_interrupts();

    uint16_t n = r->count < max_len ? r->count : max_len;
    /* Oldest retained byte is at (head - count) mod SIZE; copy forward
     * from there so the caller gets chronological order. */
    uint16_t start = (uint16_t)((r->head + CONSOLE_RING_SIZE - r->count) % CONSOLE_RING_SIZE);
    /* If count > max_len, skip the oldest (count - n) bytes we're not
     * returning, so the caller still gets the most recent n bytes. */
    start = (uint16_t)((start + (r->count - n)) % CONSOLE_RING_SIZE);

    for (uint16_t i = 0; i < n; i++) {
        out[i] = r->buf[(start + i) % CONSOLE_RING_SIZE];
    }

    restore_interrupts(saved);
    return n;
}
