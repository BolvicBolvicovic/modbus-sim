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
 * Strings and devices are borrowed from *config, which must outlive the server. The only
 * server/ file that needs config/.
 */
server_options_t	server_options_from_config(config_t* config);

#endif
