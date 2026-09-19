/* pageclient.h -- the "read a tensor page" side of the local shared-memory
 * fast path: attach to swapd's shared table/pool (read-only, like
 * node-agentd) for zero-copy reads of already-resident pages, and fall
 * back to asking swapd over its Unix socket -- the same PAGE_FETCH_REQ/
 * RESP messages a remote node would use -- on a miss, which makes swapd
 * page it in and publish it. Used by shard-execd.
 */
#ifndef CONTINUUM_PAGECLIENT_H
#define CONTINUUM_PAGECLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct pageclient pageclient_t;

/* Attaches to node `node_id`'s shared state, retrying for a few seconds if
 * swapd hasn't created it yet. `swapd_unix_socket` is used lazily, only on
 * a miss. Returns NULL if attaching never succeeds. */
pageclient_t *pageclient_connect(uint32_t node_id, uint64_t page_size, const char *swapd_unix_socket);
void pageclient_disconnect(pageclient_t *pc);

/* Returns a pointer to page_index's page_size bytes (zero-copy, straight
 * into the shared DRAM pool), fetching it through swapd first if it
 * wasn't already resident. The pointer is a view into shared, mutable
 * state -- valid until that page is evicted, which won't happen between
 * two calls the caller makes back-to-back, but shouldn't be held onto
 * across a long compute step. Returns NULL if swapd couldn't supply it. */
const void *pageclient_get_page(pageclient_t *pc, uint64_t page_index);

uint64_t pageclient_page_size(const pageclient_t *pc);

#endif
