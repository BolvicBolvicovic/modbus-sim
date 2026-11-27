/**
 * @file
 * @brief Adapter from a loaded config to server options.
 */
#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include "config/config.h"
#include "server/server.h"

/**
 * @brief Maps a loaded config to server options.
 *
 * Strings are borrowed from *config, which must outlive the server. `devices` is every device
 * of the file: a caller that runs several servers picks each server's devices with
 * device_serves(). Server `index` must be below config->server_count. The only server/ file
 * that needs config/.
 */
server_options_t	server_options_from_config(config_t* config, u32 index);

#endif
