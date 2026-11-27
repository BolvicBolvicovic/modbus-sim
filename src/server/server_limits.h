/**
 * @file
 * @brief Limits and defaults shared by the config loader and the public API.
 */
#ifndef SERVER_LIMITS_H
#define SERVER_LIMITS_H

/**
 * @brief Limits and defaults shared by the config loader and the public API.
 *
 * Lives in its own header so both can use them without depending on each other (config is
 * optional, see WITH_CONFIG) and without pulling in net/.
 */
enum
{
	SERVER_MAX_DEVICES			= 16,
	SERVER_MAX_SERVERS			= 8,	/**< servers per config and per API context */
	SERVER_ID_MAX				= 32,	/**< longest server id plus the NUL */
	SERVER_DEFAULT_PORT			= 1502,
	SERVER_DEFAULT_HTTP_PORT		= 9102,
	SERVER_DEFAULT_IDLE_TIMEOUT_MS		= 0,
	SERVER_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS	= 10000,
	SERVER_DEFAULT_BAUD			= 0,	/**< 0 = unknown; derive RTU timing from tty when possible */
	SERVER_BIND_MAX				= 128,
	SERVER_PATH_MAX				= 256,
	SERVER_TLS_PATH_MAX			= 256,
	SERVER_TLS_CIPHER_MAX			= 512,
};

#endif
