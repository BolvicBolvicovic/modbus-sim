/**
 * @file
 * @brief Adapter from a loaded config to server options.
 */
#include "server/server_config.h"

server_options_t
server_options_from_config(config_t* config, u32 index)
{
	const config_server_t* srv = &config->servers[index];

	return (server_options_t){
		.id                       = srv->id[0] ? srv->id : NULL,
		.mode                     = srv->mode,
		.bind_address             = srv->bind_address,
		.port                     = srv->port,
		.path                     = srv->path,
		.fd                       = -1,
		.seed                     = srv->seed,
		.idle_timeout_ms          = srv->idle_timeout_ms,
		.frame_gap_ms             = srv->frame_gap_ms,
		.baud                     = srv->baud,
		.strict_t15               = srv->strict_t15,
		.broadcast_execute        = srv->broadcast_execute,
		.data_bits                = srv->data_bits,
		.parity                   = srv->parity,
		.stop_bits                = srv->stop_bits,
		.tls_enabled              = srv->tls_enabled,
		.tls_handshake_timeout_ms = srv->tls_handshake_timeout_ms,
		.http_bind_address = srv->http_enabled ? srv->http_bind_address : NULL,
		.http_port         = srv->http_port,
		.http_control      = srv->http_enabled && (srv->http_control || srv->http_ui),
		.http_ui           = srv->http_enabled && srv->http_ui,
		.http_allowed_hosts = srv->http_enabled ? srv->http_allowed_hosts : NULL,
		.tls = {
			.certificate                = srv->tls_certificate,
			.private_key                = srv->tls_private_key,
			.ca_certificate             = srv->tls_ca_certificate[0] ?
						      srv->tls_ca_certificate : NULL,
			.require_client_certificate = srv->tls_require_client_certificate,
			.min_version                = srv->tls_min_version == CONFIG_TLS_VERSION_1_3 ?
						      NET_TLS_VERSION_1_3 : NET_TLS_VERSION_1_2,
			.tls1_2_ciphers             = srv->tls1_2_ciphers[0] ? srv->tls1_2_ciphers : NULL,
			.tls1_3_ciphersuites        = srv->tls1_3_ciphersuites[0] ?
						      srv->tls1_3_ciphersuites : NULL,
		},
		.devices                  = config->devices,
		.device_count             = config->device_count,
	};
}
