/* gen_toy_weights -- writes a toymodel_format.h weight file with
 * deterministic pseudo-random int8 weights, so shard-execd has something
 * real (if tiny) to page in and run a forward pass against.
 *
 * Usage: gen_toy_weights <output_path> [seed]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toymodel_format.h"

#define VOCAB_SIZE 256
#define DIM 32
#define FF_DIM 64
#define PAGE_SIZE 4096

static uint32_t g_rng_state;

static uint32_t xorshift32(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

static void fill_random_int8(int8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = (int8_t)(xorshift32() % 256);
}

static void write_padded_tensor(FILE *f, const int8_t *data, size_t n_bytes, uint32_t page_size) {
    fwrite(data, 1, n_bytes, f);
    size_t padded = (size_t)toymodel_pages_for_bytes(n_bytes, page_size) * page_size;
    for (size_t i = n_bytes; i < padded; i++) fputc(0, f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output_path> [seed]\n", argv[0]);
        return 1;
    }
    const char *out_path = argv[1];
    g_rng_state = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 42u;
    if (g_rng_state == 0) g_rng_state = 1; /* xorshift32 can't start at 0 */

    toymodel_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, TOYMODEL_MAGIC, sizeof(hdr.magic));
    hdr.vocab_size = VOCAB_SIZE;
    hdr.dim = DIM;
    hdr.ff_dim = FF_DIM;
    hdr.page_size = PAGE_SIZE;
    hdr.scale = 0.05f;

    uint32_t page_cursor = 1; /* page 0 is the header itself */
    struct {
        toymodel_tensor_loc_t *loc;
        size_t bytes;
    } tensors[] = {
        {&hdr.embedding, (size_t)VOCAB_SIZE * DIM},
        {&hdr.w_q, (size_t)DIM * DIM},
        {&hdr.w_k, (size_t)DIM * DIM},
        {&hdr.w_v, (size_t)DIM * DIM},
        {&hdr.w_o, (size_t)DIM * DIM},
        {&hdr.w_ff1, (size_t)DIM * FF_DIM},
        {&hdr.w_ff2, (size_t)FF_DIM * DIM},
        {&hdr.unembedding, (size_t)DIM * VOCAB_SIZE},
    };
    size_t n_tensors = sizeof(tensors) / sizeof(tensors[0]);
    for (size_t i = 0; i < n_tensors; i++) {
        tensors[i].loc->start_page = page_cursor;
        tensors[i].loc->n_pages = toymodel_pages_for_bytes(tensors[i].bytes, PAGE_SIZE);
        page_cursor += tensors[i].loc->n_pages;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    uint8_t header_page[PAGE_SIZE];
    memset(header_page, 0, PAGE_SIZE);
    memcpy(header_page, &hdr, sizeof(hdr));
    fwrite(header_page, 1, PAGE_SIZE, f);

    for (size_t i = 0; i < n_tensors; i++) {
        int8_t *buf = malloc(tensors[i].bytes);
        fill_random_int8(buf, tensors[i].bytes);
        write_padded_tensor(f, buf, tensors[i].bytes, PAGE_SIZE);
        free(buf);
    }
    fclose(f);

    printf("wrote %s: vocab=%u dim=%u ff_dim=%u page_size=%u total_pages=%u (%.1f KB)\n",
           out_path, hdr.vocab_size, hdr.dim, hdr.ff_dim, hdr.page_size, page_cursor,
           (double)page_cursor * PAGE_SIZE / 1024.0);
    printf("layout:\n");
    printf("  embedding   page %u..%u\n", hdr.embedding.start_page, hdr.embedding.start_page + hdr.embedding.n_pages - 1);
    printf("  w_q         page %u..%u\n", hdr.w_q.start_page, hdr.w_q.start_page + hdr.w_q.n_pages - 1);
    printf("  w_k         page %u..%u\n", hdr.w_k.start_page, hdr.w_k.start_page + hdr.w_k.n_pages - 1);
    printf("  w_v         page %u..%u\n", hdr.w_v.start_page, hdr.w_v.start_page + hdr.w_v.n_pages - 1);
    printf("  w_o         page %u..%u\n", hdr.w_o.start_page, hdr.w_o.start_page + hdr.w_o.n_pages - 1);
    printf("  w_ff1       page %u..%u\n", hdr.w_ff1.start_page, hdr.w_ff1.start_page + hdr.w_ff1.n_pages - 1);
    printf("  w_ff2       page %u..%u\n", hdr.w_ff2.start_page, hdr.w_ff2.start_page + hdr.w_ff2.n_pages - 1);
    printf("  unembedding page %u..%u\n", hdr.unembedding.start_page, hdr.unembedding.start_page + hdr.unembedding.n_pages - 1);
    printf("tensor_region_bytes for swapd.conf: %u\n", page_cursor * PAGE_SIZE);
    return 0;
}
