/* console_ring.h -- a 4KB circular buffer capturing every byte the Pi
 * writes to its console UART (ARCH-002 §03), for post-mortem reads after
 * a crash: the boot processor keeps the Pi's last output even after a
 * power-cycle wipes the Pi's own RAM.
 *
 * Single-producer (the UART RX interrupt handler), single-consumer (the
 * main superloop, when asked to dump the buffer). No locking: on a
 * single-core-at-a-time access pattern like this, disabling the UART
 * interrupt around a read is enough, which console_ring_read() does
 * itself.
 */
#ifndef CONSOLE_RING_H
#define CONSOLE_RING_H

#include <stdint.h>

#define CONSOLE_RING_SIZE 4096

typedef struct {
    uint8_t buf[CONSOLE_RING_SIZE];
    uint16_t head;   /* next write index */
    uint16_t count;  /* bytes held, saturates at CONSOLE_RING_SIZE (oldest overwritten past that) */
} console_ring_t;

void console_ring_init(console_ring_t *r);

/* Call only from the UART RX interrupt handler. */
void console_ring_push(console_ring_t *r, uint8_t byte);

/* Copies up to max_len of the most recently captured bytes into out, in
 * chronological order (oldest of the retained bytes first). Returns the
 * number of bytes copied. */
uint16_t console_ring_read(console_ring_t *r, uint8_t *out, uint16_t max_len);

#endif
