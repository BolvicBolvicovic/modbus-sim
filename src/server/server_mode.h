/**
 * @file
 * @brief Transport selector shared by the config loader and the server facade.
 */
#ifndef SERVER_MODE_H
#define SERVER_MODE_H

typedef enum server_mode_t server_mode_t;

/**
 * @brief Transport selected by (server (mode NAME)) or by a host program.
 *
 * Lives in its own header so config/ can name modes without depending on net/. The *_FD modes
 * serve an already-open byte-stream fd (socketpair, PTY master, ...) borrowed from the host;
 * the config file cannot select them.
 */
enum server_mode_t
{
	SERVER_MODE_TCP,
	SERVER_MODE_RTU_PTY,
	SERVER_MODE_RTU_DEVICE,
	SERVER_MODE_RTU_TCP,
	SERVER_MODE_ASCII_PTY,
	SERVER_MODE_ASCII_DEVICE,
	SERVER_MODE_ASCII_TCP,
	SERVER_MODE_RTU_FD,
	SERVER_MODE_ASCII_FD,
	SERVER_MODE_UDP,
	SERVER_MODE_ASCII_UDP,
	SERVER_MODE_RTU_UDP,
};

#endif
