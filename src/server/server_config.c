/**
 * @file
 * @brief Adapter from a loaded config to server options.
 */
#include "server/server_config.h"

server_options_t
server_options_from_config(config_t* config)
{
	return (server_options_t){
		.mode                     = config->mode,
		.bind_address             = config->bind_address,
		.port                     = config->port,
		.path                     = config->path,
		.fd                       = -1,
		.seed                     = config->seed,
		.idle_timeout_ms          = config->idle_timeout_ms,
		.frame_gap_ms             = config->frame_gap_ms,
		.baud                     = config->baud,
		.strict_t15               = config->strict_t15,
		.broadcast_execute        = config->broadcast_execute,
		.data_bits                = config->data_bits,
		.parity                   = config->parity,
		.stop_bits                = config->stop_bits,
		.tls_enabled              = config->tls_enabled,
		.tls_handshake_timeout_ms = config->tls_handshake_timeout_ms,
		.http_bind_address = config->http_enabled ? config->http_bind_address : NULL,
		.http_port         = config->http_port,
		.tls = {
			.certificate                = config->tls_certificate,
			.private_key                = config->tls_private_key,
			.ca_certificate             = config->tls_ca_certificate[0] ?
						      config->tls_ca_certificate : NULL,
			.require_client_certificate = config->tls_require_client_certificate,
			.min_version                = config->tls_min_version == CONFIG_TLS_VERSION_1_3 ?
						      NET_TLS_VERSION_1_3 : NET_TLS_VERSION_1_2,
			.tls1_2_ciphers             = config->tls1_2_ciphers[0] ? config->tls1_2_ciphers : NULL,
			.tls1_3_ciphersuites        = config->tls1_3_ciphersuites[0] ?
						      config->tls1_3_ciphersuites : NULL,
		},
		.devices                  = config->devices,
		.device_count             = config->device_count,
	};
}
