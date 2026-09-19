/* shard_config.h -- config file format for shard-execd.
 *
 * node_id 7
 * weight_file /var/lib/continuum/shard-07.weights
 * page_size 4096
 * swapd_unix_socket /run/continuum/swapd.sock
 * listen_unix_socket /run/continuum/shard-execd.sock
 * default_max_new_tokens 64
 * default_temperature 0.8
 */
#ifndef SHARD_CONFIG_H
#define SHARD_CONFIG_H

#include <stdint.h>

typedef struct {
    uint32_t node_id;
    char weight_file[256];
    uint64_t page_size;
    char swapd_unix_socket[128];
    char listen_unix_socket[128];
    uint32_t default_max_new_tokens;
    float default_temperature;
} shard_config_t;

int shard_config_load(const char *path, shard_config_t *out);

#endif
