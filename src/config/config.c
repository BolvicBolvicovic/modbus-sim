/**
 * @file
 * @brief Loads a Scheme config file into devices and server settings.
 */
#include "config/config.h"
#include "config/config_device.h"
#include "config/config_machine.h"
#include "device/machine.h"
#include "os/os_serial.h"
#include "os/os_socket.h"
#include "os/os_time.h"
#include "vlib/macros.h"
#include "vlib/readfile.h"
#include <string.h>

/**
 * @brief Scheme helper run once: reads every form in a text without evaluating any of them.
 *
 * Returns (#t form ...) on success or (#f "message") on a syntax error.
 */
static const char READ_ALL_FORMS[] =
	"(lambda (text)"
	"  (catch #t"
	"    (lambda ()"
	"      (let ((port (open-input-string text)))"
	"        (let loop ((form (read port)) (forms ()))"
	"          (if (eof-object? form)"
	"              (cons #t (reverse forms))"
	"              (loop (read port) (cons form forms))))))"
	"    (lambda (type info)"
	"      (list #f (if (and (pair? info) (string? (car info)))"
	"                   (apply format #f info)"
	"                   (object->string info))))))";

static const char* const PARITY_NAMES[] = { "none", "even", "odd", "mark", "space" };
/** @brief Indexed by every server_mode_t so messages never read past the table. */
static const char* const MODE_NAMES[] = {
	[SERVER_MODE_TCP]          = "tcp",
	[SERVER_MODE_RTU_PTY]      = "rtu-pty",
	[SERVER_MODE_RTU_DEVICE]   = "rtu-device",
	[SERVER_MODE_RTU_TCP]      = "rtu-tcp",
	[SERVER_MODE_ASCII_PTY]    = "ascii-pty",
	[SERVER_MODE_ASCII_DEVICE] = "ascii-device",
	[SERVER_MODE_ASCII_TCP]    = "ascii-tcp",
	[SERVER_MODE_RTU_FD]       = "rtu-fd",
	[SERVER_MODE_ASCII_FD]     = "ascii-fd",
	[SERVER_MODE_UDP]          = "udp",
	[SERVER_MODE_ASCII_UDP]    = "ascii-udp",
	[SERVER_MODE_RTU_UDP]      = "rtu-udp",
};
/** @brief What (mode ...) accepts, in selection order; the _FD modes are host-only. */
static const char* const CONFIG_MODE_NAMES[] = {
	"tcp", "rtu-pty", "rtu-device", "rtu-tcp", "ascii-pty", "ascii-device", "ascii-tcp", "udp",
	"ascii-udp", "rtu-udp",
};
static const server_mode_t CONFIG_MODE_VALUES[] = {
	SERVER_MODE_TCP, SERVER_MODE_RTU_PTY, SERVER_MODE_RTU_DEVICE, SERVER_MODE_RTU_TCP,
	SERVER_MODE_ASCII_PTY, SERVER_MODE_ASCII_DEVICE, SERVER_MODE_ASCII_TCP, SERVER_MODE_UDP,
	SERVER_MODE_ASCII_UDP,
	SERVER_MODE_RTU_UDP,
};
#if WITH_TLS
static const char* const TLS_VERSION_NAMES[] = { "tls1.2", "tls1.3" };
#endif

bool
config_parity_from_name(const char* name, parity_t* out)
{
	for (u32 i = 0; i < ARRAY_COUNT(PARITY_NAMES); i++)
	{
		if (strcmp(name, PARITY_NAMES[i]) != 0)
			continue;
		*out = (parity_t)i;
		return true;
	}
	return false;
}

static void
config_server_defaults(config_server_t* srv)
{
	memset(srv, 0, sizeof(*srv));
	strcpy(srv->bind_address, "127.0.0.1");
	srv->mode              = SERVER_MODE_TCP;
	srv->port              = SERVER_DEFAULT_PORT;
	srv->idle_timeout_ms   = SERVER_DEFAULT_IDLE_TIMEOUT_MS;
	srv->baud              = SERVER_DEFAULT_BAUD;	/* frame gap 0: auto, derived from baud or the default gap */
	srv->data_bits         = 8;
	srv->parity            = PARITY_NONE;
	srv->stop_bits         = 1;
	srv->tls_require_client_certificate = true;
	srv->tls_min_version   = CONFIG_TLS_VERSION_1_2;
	srv->tls_handshake_timeout_ms = SERVER_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS;
}

#if WITH_TLS
static bool
check_tls_path(form_ctx_t* ctx, const char* key, const char* value)
{
	if (!value[0])
		return form_fail(ctx, "tls: %s path must not be empty", key);
	if (strlen(value) >= SERVER_TLS_PATH_MAX)
		return form_fail(ctx, "tls: %s path is too long (max %d characters)",
				 key, SERVER_TLS_PATH_MAX - 1);
	return true;
}

static bool
parse_tls(form_ctx_t* ctx, s7_pointer options, config_server_t* srv)
{
	static const char* const keys[] = {
		"certificate", "private-key", "ca-certificate", "handshake-timeout-ms",
		"require-client-certificate", "min-version", "tls1-2-ciphers",
		"tls1-3-ciphersuites"
	};
	const char*	certificate = NULL;
	const char*	private_key = NULL;
	const char*	ca_certificate = NULL;
	const char*	ciphers = NULL;
	const char*	ciphersuites = NULL;
	bool		require_client = true;
	u32		version = CONFIG_TLS_VERSION_1_2;
	s64		handshake_timeout = SERVER_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "tls"))
		return false;
	if (!form_need(ctx, options, "certificate", "tls") ||
	    !form_need(ctx, options, "private-key", "tls"))
		return false;

	if (!form_opt_string(ctx, options, "certificate", &certificate) ||
	    !form_opt_string(ctx, options, "private-key", &private_key) ||
	    !form_opt_string(ctx, options, "ca-certificate", &ca_certificate) ||
	    !form_opt_string(ctx, options, "tls1-2-ciphers", &ciphers) ||
	    !form_opt_string(ctx, options, "tls1-3-ciphersuites", &ciphersuites))
		return false;

	if (!check_tls_path(ctx, "certificate", certificate) ||
	    !check_tls_path(ctx, "private-key", private_key))
		return false;
	if (ca_certificate && !check_tls_path(ctx, "ca-certificate", ca_certificate))
		return false;

	if (ciphers)
	{
		if (!ciphers[0])
			return form_fail(ctx, "tls: tls1-2-ciphers must not be empty");
		if (strlen(ciphers) >= SERVER_TLS_CIPHER_MAX)
			return form_fail(ctx, "tls: tls1-2-ciphers is too long (max %d characters)",
					 SERVER_TLS_CIPHER_MAX - 1);
	}
	if (ciphersuites)
	{
		if (!ciphersuites[0])
			return form_fail(ctx, "tls: tls1-3-ciphersuites must not be empty");
		if (strlen(ciphersuites) >= SERVER_TLS_CIPHER_MAX)
			return form_fail(ctx, "tls: tls1-3-ciphersuites is too long (max %d characters)",
					 SERVER_TLS_CIPHER_MAX - 1);
	}

	if (!form_opt_int(ctx, options, "handshake-timeout-ms", 1, 86400000, &handshake_timeout))
		return false;
	if (!form_opt_bool(ctx, options, "require-client-certificate", &require_client))
		return false;
	if (!form_opt_choice(ctx, options, "min-version", TLS_VERSION_NAMES,
			     ARRAY_COUNT(TLS_VERSION_NAMES), &version))
		return false;
	if (require_client && !ca_certificate)
		return form_fail(ctx, "tls: ca-certificate is required when client certificates are required");

	strcpy(srv->tls_certificate, certificate);
	strcpy(srv->tls_private_key, private_key);
	if (ca_certificate)
		strcpy(srv->tls_ca_certificate, ca_certificate);
	if (ciphers)
		strcpy(srv->tls1_2_ciphers, ciphers);
	if (ciphersuites)
		strcpy(srv->tls1_3_ciphersuites, ciphersuites);
	srv->tls_enabled               = true;
	srv->tls_require_client_certificate = require_client;
	srv->tls_min_version           = (config_tls_version_t)version;
	srv->tls_handshake_timeout_ms  = (u32)handshake_timeout;
	return true;
}
#endif

/* (http (port N) (bind "ADDR") (control on) (ui on) (allowed-host "NAME")...): the health, readiness,
 * Prometheus, optional control and web UI endpoint, any mode. */
static bool
parse_http(form_ctx_t* ctx, s7_pointer options, config_server_t* srv)
{
	static const char* const keys[] = { "port", "bind", "control", "ui", "allowed-host" };
	s64		port = SERVER_DEFAULT_HTTP_PORT;
	const char*	bind = NULL;
	bool		control = false;
	bool		ui = false;
	char		hosts[sizeof(srv->http_allowed_hosts)] = "";
	u32		host_count = 0;

	if (!form_check_keys_repeat(ctx, options, keys, ARRAY_COUNT(keys), "http", "allowed-host") ||
	    !form_opt_int(ctx, options, "port", 1, 65535, &port) ||
	    !form_opt_string(ctx, options, "bind", &bind) ||
	    !form_opt_bool(ctx, options, "control", &control) ||
	    !form_opt_bool(ctx, options, "ui", &ui))
		return false;
	for (s7_pointer p = options; s7_is_pair(p); p = s7_cdr(p))
	{
		const char*	head = form_head_name(s7_car(p));
		const char*	name = NULL;

		if (!head || strcmp(head, "allowed-host") != 0)
			continue;
		if (!form_get_string(ctx, s7_car(p), &name))
			return false;
		if (++host_count > 8)
			return form_fail(ctx, "http: at most 8 allowed-host names");
		if (!name[0] ||
		    strlen(name) + strlen(hosts) + 2 > sizeof(hosts) ||
		    strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != strlen(name))
			return form_fail(ctx, "http: allowed-host '%s' must be a host name of letters, digits, '.', '-' and '_' "
					 "(no port, at most %u characters in all)", name, (u32)sizeof(hosts) - 1);
		sprintf(hosts + strlen(hosts), "%s%s", hosts[0] ? "," : "", name);
	}
	if (bind && strlen(bind) >= SERVER_BIND_MAX)
		return form_fail(ctx, "http: bind address is too long (max %d characters)", SERVER_BIND_MAX - 1);
	if (bind && !os_ip_is_literal(bind))
		return form_fail(ctx, "http: bind address '%s' is not a valid IPv4 or IPv6 address", bind);
	strcpy(srv->http_bind_address, bind ? bind : "127.0.0.1");
	srv->http_port    = (u16)port;
	srv->http_control = control || ui;
	srv->http_ui      = ui;
	strcpy(srv->http_allowed_hosts, hosts);
	srv->http_enabled = true;
	return true;
}

static bool
parse_server(form_ctx_t* ctx, s7_pointer form, config_server_t* srv)
{
	static const char* const keys[] = { "id", "mode", "path", "port", "bind", "seed", "idle-timeout-ms", "frame-gap-ms", "baud", "strict-t15", "broadcast-execute", "data-bits", "parity", "stop-bits", "tls", "http" };
	static const char* const tcp_keys[] = { "mode", "port", "bind", "seed", "idle-timeout-ms", "tls" };
	static const char* const udp_keys[] = { "mode", "port", "bind", "seed" };
	static const char* const ascii_udp_keys[] = { "mode", "port", "bind", "seed" };
	static const char* const rtu_udp_keys[] = { "mode", "port", "bind", "seed", "broadcast-execute" };
	static const char* const ascii_tcp_keys[] = { "mode", "port", "bind", "seed", "idle-timeout-ms" };
	static const char* const rtu_tcp_keys[] = { "mode", "port", "bind", "seed", "idle-timeout-ms", "baud", "frame-gap-ms", "broadcast-execute" };
	static const char* const rtu_serial_keys[] = { "mode", "seed", "baud", "frame-gap-ms", "strict-t15", "broadcast-execute", "parity", "stop-bits" };
	static const char* const ascii_keys[] = { "mode", "seed", "baud", "data-bits", "parity", "stop-bits" };
	const char* const* allowed = NULL;
	u32		allowed_count     = 0;
	s7_pointer	options           = s7_cdr(form);
	s64		port              = srv->port;
	s64		seed              = srv->seed;
	s64		idle              = srv->idle_timeout_ms;
	s64		frame             = srv->frame_gap_ms;
	s64		baud              = srv->baud;
	s64		data_bits         = srv->data_bits;
	s64		stop_bits         = srv->stop_bits;
	u32		parity            = srv->parity;
	u32		mode              = srv->mode;
	u32		mode_index        = 0;
	bool		strict_t15        = srv->strict_t15;
	bool		broadcast_execute = srv->broadcast_execute;
	const char*	bind              = NULL;
	const char*	path              = NULL;
	const char*	id                = NULL;
#if WITH_TLS
	s7_pointer	tls_form;
#endif
	bool		port_given        = false;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "server"))
		return false;

#if !WITH_TLS
	if (!form_is_absent(ctx, form_find(ctx, options, "tls")))
		return form_fail(ctx, "tls: built without TLS support");
#endif

	if (!form_opt_string(ctx, options, "id", &id))
		return false;
	if (id &&
	    (!id[0] ||
	     strlen(id) >= SERVER_ID_MAX ||
	     strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != strlen(id)))
		return form_fail(ctx, "server: id '%s' must be 1 to %d letters, digits, '.', '-' or '_'", id,
				 SERVER_ID_MAX - 1);
	if (id)
		strcpy(srv->id, id);

	if (!form_opt_choice(ctx, options, "mode", CONFIG_MODE_NAMES, ARRAY_COUNT(CONFIG_MODE_NAMES),
			     &mode_index))
		return false;
	if (!form_is_absent(ctx, form_find(ctx, options, "mode")))
		mode = CONFIG_MODE_VALUES[mode_index];

	if (!form_opt_string(ctx, options, "path", &path))
		return false;

	switch (mode)
	{
		case SERVER_MODE_TCP:
			allowed = tcp_keys;
			allowed_count = ARRAY_COUNT(tcp_keys);
			break;
		case SERVER_MODE_UDP:
			allowed = udp_keys;
			allowed_count = ARRAY_COUNT(udp_keys);
			break;
		case SERVER_MODE_ASCII_UDP:
			allowed = ascii_udp_keys;
			allowed_count = ARRAY_COUNT(ascii_udp_keys);
			break;
		case SERVER_MODE_RTU_UDP:
			allowed = rtu_udp_keys;
			allowed_count = ARRAY_COUNT(rtu_udp_keys);
			break;
		case SERVER_MODE_ASCII_TCP:
			allowed = ascii_tcp_keys;
			allowed_count = ARRAY_COUNT(ascii_tcp_keys);
			break;
		case SERVER_MODE_RTU_TCP:
			allowed = rtu_tcp_keys;
			allowed_count = ARRAY_COUNT(rtu_tcp_keys);
			break;
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_RTU_DEVICE:
			allowed = rtu_serial_keys;
			allowed_count = ARRAY_COUNT(rtu_serial_keys);
			break;
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_ASCII_DEVICE:
			allowed = ascii_keys;
			allowed_count = ARRAY_COUNT(ascii_keys);
			break;
		case SERVER_MODE_RTU_FD:
		case SERVER_MODE_ASCII_FD:
			break;	/* host-only modes; MODE_NAMES never yields them */
	}
	/* ASCII never executes broadcasts; call that out instead of relying on the
	 * generic mode check below. */
	if ((mode == SERVER_MODE_ASCII_PTY ||
	     mode == SERVER_MODE_ASCII_DEVICE ||
	     mode == SERVER_MODE_ASCII_TCP ||
	     mode == SERVER_MODE_ASCII_UDP) &&
	    !form_is_absent(ctx, form_find(ctx, options, "broadcast-execute")))
		return form_fail(ctx, "server: broadcast-execute is not valid in %s mode (ASCII never executes unit-0 broadcasts)",
				 MODE_NAMES[mode]);

	/* Check presence, not value: even a default-valued option is rejected when
	 * the mode does not support it. (path) has its own required/forbidden checks below. */
	for (u32 i = 0; i < ARRAY_COUNT(keys); i++)
	{
		bool supported = false;

		if (strcmp(keys[i], "path") == 0 ||
		    strcmp(keys[i], "id") == 0 ||
		    strcmp(keys[i], "http") == 0 ||
		    form_is_absent(ctx, form_find(ctx, options, keys[i])))
			continue;
		for (u32 j = 0; j < allowed_count; j++)
		{
			if (strcmp(keys[i], allowed[j]) != 0)
				continue;
			supported = true;
			break;
		}
		if (!supported)
			return form_fail(ctx, "server: %s is not valid in %s mode", keys[i], MODE_NAMES[mode]);
	}

#if WITH_TLS
	tls_form = form_find(ctx, options, "tls");
#endif
	port_given = !form_is_absent(ctx, form_find(ctx, options, "port"));
#if WITH_TLS
	if (!form_is_absent(ctx, tls_form) && !parse_tls(ctx, s7_cdr(tls_form), srv))
		return false;
#endif

	if (!form_opt_int(ctx, options, "port", 1, 65535, &port))
		return false;

	if (!form_is_absent(ctx, form_find(ctx, options, "http")) &&
	    !parse_http(ctx, s7_cdr(form_find(ctx, options, "http")), srv))
		return false;

	if (!form_opt_int(ctx, options, "seed", 0, 4294967295LL, &seed))
		return false;

	if (!form_opt_int(ctx, options, "idle-timeout-ms", 0, 86400000, &idle))
		return false;

	if (!form_opt_int(ctx, options, "frame-gap-ms", 1, 60000, &frame))
		return false;

	if (!form_opt_int(ctx, options, "baud", 1, 4000000, &baud))
		return false;
	if (baud > 0 && !os_baud_is_standard((u32)baud))
		return form_fail(ctx, "server: baud must be a standard termios rate (e.g. 9600, 19200, 38400, 115200)");

	if (!form_opt_bool(ctx, options, "strict-t15", &strict_t15))
		return false;

	if (!form_opt_bool(ctx, options, "broadcast-execute", &broadcast_execute))
		return false;

	if (!form_opt_int(ctx, options, "data-bits", 7, 8, &data_bits))
		return false;

	if (!form_opt_choice(ctx, options, "parity", PARITY_NAMES, ARRAY_COUNT(PARITY_NAMES), &parity))
		return false;

	if (!form_opt_int(ctx, options, "stop-bits", 1, 2, &stop_bits))
		return false;

	if (!form_opt_string(ctx, options, "bind", &bind))
		return false;

	if (bind && strlen(bind) >= SERVER_BIND_MAX)
		return form_fail(ctx, "server: bind address is too long (max %d characters)", SERVER_BIND_MAX - 1);

	if (bind && !os_ip_is_literal(bind))
		return form_fail(ctx, "server: bind address '%s' is not a valid IPv4 or IPv6 address", bind);

	if ((mode == SERVER_MODE_RTU_DEVICE || mode == SERVER_MODE_ASCII_DEVICE) &&
	    !path)
		return form_fail(ctx, "server: (path ...) is required in rtu-device and ascii-device modes");

	if (path &&
	    mode != SERVER_MODE_RTU_DEVICE &&
	    mode != SERVER_MODE_ASCII_DEVICE)
		return form_fail(ctx, "server: (path ...) is only valid in rtu-device and ascii-device modes");

	if (path && strlen(path) >= SERVER_PATH_MAX)
		return form_fail(ctx, "server: path is too long (max %d characters)", SERVER_PATH_MAX - 1);

	if (bind)
		strcpy(srv->bind_address, bind);
	if (path)
		strcpy(srv->path, path);

	srv->mode              = (server_mode_t)mode;
	srv->port              = (u16)(srv->tls_enabled && !port_given ? CONFIG_DEFAULT_TLS_PORT : port);
	if (srv->http_enabled &&
	    srv->http_port == srv->port &&
	    (mode == SERVER_MODE_TCP || mode == SERVER_MODE_RTU_TCP || mode == SERVER_MODE_ASCII_TCP))
		return form_fail(ctx, "http: port %u is already the Modbus server port", srv->port);
	srv->seed              = (u32)seed;
	srv->idle_timeout_ms   = (u32)idle;
	srv->frame_gap_ms      = (u32)frame;
	srv->baud              = (u32)baud;
	srv->strict_t15        = strict_t15;
	srv->broadcast_execute = broadcast_execute;
	srv->data_bits         = (u32)data_bits;
	srv->parity            = (parity_t)parity;
	srv->stop_bits         = (u32)stop_bits;
	return true;
}

/** @brief True if a device with a server list names a server that does not exist. */
static bool
check_device_servers(form_ctx_t* ctx, const config_t* config)
{
	for (u32 i = 0; i < config->device_count; i++)
	{
		const device_t* dev = config->devices[i];

		for (u32 k = 0; k < dev->server_count; k++)
		{
			bool found = false;

			for (u32 s = 0; s < config->server_count && !found; s++)
				found = strcmp(config->servers[s].id, dev->servers[k]) == 0;
			if (!found)
				return form_fail(ctx, "device '%s': (servers ...) names '%s', but no (server (id \"%s\")) exists",
						 dev->name, dev->servers[k], dev->servers[k]);
		}
	}
	return true;
}

/** @brief Devices served by server `s`, in file order; returns how many. */
static u32
server_devices(const config_t* config, u32 s, const device_t** out)
{
	u32 n = 0;

	for (u32 i = 0; i < config->device_count; i++)
	{
		if (device_serves(config->devices[i], config->servers[s].id[0] ? config->servers[s].id : NULL))
			out[n++] = config->devices[i];
	}
	return n;
}

/** @brief Unit ids, any-unit, RTU/ASCII unit range and fault/mode rules of the devices of each server. */
static bool
check_server_devices(form_ctx_t* ctx, const config_t* config)
{
	for (u32 s = 0; s < config->server_count; s++)
	{
		const config_server_t*	srv = &config->servers[s];
		const device_t*		devs[SERVER_MAX_DEVICES];
		u32			n = server_devices(config, s, devs);
		const device_t*		any = NULL;
		bool			tcp_like = srv->mode == SERVER_MODE_TCP ||
						   srv->mode == SERVER_MODE_RTU_TCP ||
						   srv->mode == SERVER_MODE_ASCII_TCP;

		if (n == 0)
			return form_fail(ctx, "server '%s' serves no device; add (servers \"%s\") to a device",
					 srv->id, srv->id);
		for (u32 i = 0; i < n; i++)
		{
			for (u32 j = i + 1; j < n; j++)
			{
				if (devs[i]->unit_id == devs[j]->unit_id)
					return form_fail(ctx, "devices '%s' and '%s' both use unit-id %u",
							 devs[i]->name, devs[j]->name, devs[i]->unit_id);
			}
		}
		for (u32 i = 0; i < n; i++)
		{
			if (!devs[i]->any_unit)
				continue;
			if (any)
				return form_fail(ctx, "devices '%s' and '%s' both set any-unit; only one device may answer every unit id",
						 any->name, devs[i]->name);
			any = devs[i];
		}
		for (u32 i = 0; i < n; i++)
		{
			const device_t*	dev = devs[i];
			u8		id  = dev->unit_id;
			bool		base_checksum =
				(dev->faults.present & FAULT_BIT(FAULT_CORRUPT)) != 0 &&
				dev->faults.by_kind[FAULT_CORRUPT].corrupt_mode == FAULT_CORRUPT_CHECKSUM;
			bool		base_disconnect =
				(dev->faults.present & FAULT_BIT(FAULT_DISCONNECT)) != 0;

			if (srv->mode != SERVER_MODE_TCP &&
			    srv->mode != SERVER_MODE_UDP &&
			    (id == 0 || id > 247))
				return form_fail(ctx, "device '%s': unit-id %u is not valid for RTU/ASCII (allowed: 1-247; 0 is broadcast, 248-255 reserved)",
						 dev->name, id);
			if ((srv->mode == SERVER_MODE_TCP ||
			     srv->mode == SERVER_MODE_UDP) &&
			    (base_checksum ||
			     device_machine_corrupt_mode_present(dev, FAULT_CORRUPT_CHECKSUM)))
				return form_fail(ctx, "device '%s': corrupt mode checksum is not valid in %s mode (MBAP frames have no checksum)",
						 dev->name, MODE_NAMES[srv->mode]);
			if (!tcp_like &&
			    (base_disconnect ||
			     device_machine_fault_present(dev, FAULT_DISCONNECT)))
				return form_fail(ctx, "device '%s': (fault (kind disconnect)) is not valid in %s mode (TCP-like transports only)",
						 dev->name, MODE_NAMES[srv->mode]);
		}
	}
	return true;
}

/** @brief True for the addresses that listen on every interface. */
static bool
bind_is_wildcard(const char* bind)
{
	return strcmp(bind, "0.0.0.0") == 0 || strcmp(bind, "::") == 0;
}

/** @brief One socket or serial device a server opens, for the clash check. */
struct listener_t
{
	/** @brief What the listener binds. */
	enum { LISTEN_TCP, LISTEN_UDP, LISTEN_SERIAL } kind;
	const char*	bind;	/**< address, or the serial path */
	u16		port;
};

/** @brief Fills the listeners of a server (its transport, then its HTTP endpoint); returns how many. */
static u32
server_listeners(const config_server_t* srv, struct listener_t* out)
{
	u32 n = 0;

	switch (srv->mode)
	{
		case SERVER_MODE_TCP:
		case SERVER_MODE_RTU_TCP:
		case SERVER_MODE_ASCII_TCP:
			out[n++] = (struct listener_t){ LISTEN_TCP, srv->bind_address, srv->port };
			break;
		case SERVER_MODE_UDP:
		case SERVER_MODE_RTU_UDP:
		case SERVER_MODE_ASCII_UDP:
			out[n++] = (struct listener_t){ LISTEN_UDP, srv->bind_address, srv->port };
			break;
		case SERVER_MODE_RTU_DEVICE:
		case SERVER_MODE_ASCII_DEVICE:
			out[n++] = (struct listener_t){ LISTEN_SERIAL, srv->path, 0 };
			break;
		case SERVER_MODE_RTU_PTY:
		case SERVER_MODE_ASCII_PTY:
		case SERVER_MODE_RTU_FD:
		case SERVER_MODE_ASCII_FD:
			break;
	}
	if (srv->http_enabled)
		out[n++] = (struct listener_t){ LISTEN_TCP, srv->http_bind_address, srv->http_port };
	return n;
}

/**
 * @brief Rejects two servers that would open the same socket or serial device.
 *
 * Sockets clash when they are of the same family (TCP-like, UDP-like), use the same port and bind
 * the same address or one binds every interface (`::` also covers every IPv4 address; `0.0.0.0`
 * only IPv4 ones). The HTTP endpoints count as TCP listeners.
 */
static bool
check_listener_clashes(form_ctx_t* ctx, const config_t* config)
{
	for (u32 a = 0; a < config->server_count; a++)
	{
		for (u32 b = a + 1; b < config->server_count; b++)
		{
			struct listener_t	la[2];
			struct listener_t	lb[2];
			u32			na = server_listeners(&config->servers[a], la);
			u32			nb = server_listeners(&config->servers[b], lb);

			for (u32 i = 0; i < na; i++)
			{
				for (u32 j = 0; j < nb; j++)
				{
					bool clash;

					if (la[i].kind != lb[j].kind)
						continue;
					if (la[i].kind == LISTEN_SERIAL)
						clash = strcmp(la[i].bind, lb[j].bind) == 0;
					else
						clash = la[i].port == lb[j].port &&
							(strcmp(la[i].bind, lb[j].bind) == 0 ||
							 strcmp(la[i].bind, "::") == 0 ||
							 strcmp(lb[j].bind, "::") == 0 ||
							 (bind_is_wildcard(la[i].bind) && !strchr(lb[j].bind, ':')) ||
							 (bind_is_wildcard(lb[j].bind) && !strchr(la[i].bind, ':')));
					if (!clash)
						continue;
					if (la[i].kind == LISTEN_SERIAL)
						return form_fail(ctx, "servers '%s' and '%s' both use the serial device %s",
								 config->servers[a].id, config->servers[b].id, la[i].bind);
					return form_fail(ctx, "servers '%s' and '%s' both listen on %s port %u (%s and %s)",
							 config->servers[a].id, config->servers[b].id,
							 la[i].kind == LISTEN_TCP ? "TCP" : "UDP", la[i].port,
							 la[i].bind, lb[j].bind);
				}
			}
		}
	}
	return true;
}

static bool
parse_top_level_form(form_ctx_t* ctx, s7_pointer form, arena_t* arena, u64 now_ms,
		     config_t* config)
{
	const char*	head = form_head_name(form);

	if (!head)
		return form_fail(ctx, "expected (server ...) or (device ...), got %s", form_text(ctx, form));

	if (strcmp(head, "server") == 0)
	{
		config_server_t* srv = &config->servers[config->server_count];

		if (config->server_count >= SERVER_MAX_SERVERS)
			return form_fail(ctx, "too many (server ...) forms (max %d)", SERVER_MAX_SERVERS);
		config_server_defaults(srv);
		if (!parse_server(ctx, form, srv))
			return false;
		config->server_count++;
		return true;
	}

	if (strcmp(head, "define-action") == 0)
		return config_action_define(ctx, form);

	if (strcmp(head, "device") != 0)
		return form_fail(ctx, "unknown top-level form '%s'. Use (server ...), (define-action ...) or (device ...)", head);

	if (config->device_count >= SERVER_MAX_DEVICES)
		return form_fail(ctx, "too many devices (max %d)", SERVER_MAX_DEVICES);

	return config_parse_device(ctx, form, arena, now_ms,
				   &config->devices[config->device_count++]);
}

static bool
parse_forms(form_ctx_t* ctx, s7_pointer forms, arena_t* arena, config_t* config)
{
	u64	now_ms = os_now_ms();

	for (s7_pointer p = forms; s7_is_pair(p); p = s7_cdr(p))
	{
		if (!parse_top_level_form(ctx, s7_car(p), arena, now_ms, config))
			return false;
	}

	if (config->server_count == 0)
	{
		config_server_defaults(&config->servers[0]);
		config->server_count = 1;
	}
	if (config->server_count > 1)
	{
		for (u32 i = 0; i < config->server_count; i++)
		{
			if (!config->servers[i].id[0])
				return form_fail(ctx, "server %u has no (id \"NAME\"); every server needs one when the file has several",
						 i + 1);
			for (u32 j = 0; j < i; j++)
			{
				if (strcmp(config->servers[i].id, config->servers[j].id) == 0)
					return form_fail(ctx, "servers %u and %u both use id '%s'", j + 1, i + 1,
							 config->servers[i].id);
			}
		}
	}

	/* A single server serves every device; a device without (servers ...) is assigned to it. */
	for (u32 i = 0; i < config->device_count; i++)
	{
		device_t* dev = config->devices[i];

		if (dev->server_count > 0)
			continue;
		if (config->server_count > 1)
			return form_fail(ctx, "device '%s' is not assigned to a server; add (servers \"ID\" ...)", dev->name);
		if (config->servers[0].id[0])
			device_add_server(dev, config->servers[0].id);
	}
	if (!check_device_servers(ctx, config))
		return false;

	/* Machine point references were checked while each device was read; seeding and
	 * finalization wait for (server (seed N)), which may appear after the devices. A device
	 * is seeded by the first server (in file order) that serves it. */
	for (u32 i = 0; i < config->device_count; i++)
	{
		device_t*	dev  = config->devices[i];
		u32		seed = 0;

		for (u32 s = 0; s < config->server_count; s++)
		{
			if (!device_serves(dev, config->servers[s].id[0] ? config->servers[s].id : NULL))
				continue;
			seed = config->servers[s].seed;
			break;
		}
		device_seed_machines(dev, seed);
		if (!config_machine_finalize(ctx, dev) ||
		    !config_action_finalize(ctx, dev))
			return false;
	}

	if (config->device_count == 0)
		return form_fail(ctx, "the file has no (device ...) form");

	return check_server_devices(ctx, config) &&
	       check_listener_clashes(ctx, config);
}

/**
 * @brief Reads all forms of `text` with s7.
 *
 * Returns the list of forms, or false with ctx->err set.
 */
static bool
read_forms(form_ctx_t* ctx, const char* text, s7_pointer* forms)
{
	s7_scheme*	sc     = ctx->sc;
	s7_pointer	reader = s7_eval_c_string(sc, READ_ALL_FORMS);
	s7_pointer	args   = s7_cons(sc, s7_make_string(sc, text), s7_nil(sc));
	s7_pointer	result = s7_call(sc, reader, args);

	s7_gc_protect(sc, result);
	if (!s7_boolean(sc, s7_car(result)))
		return form_fail(ctx, "syntax error: %s", s7_string(s7_cadr(result)));

	*forms = s7_cdr(result);
	return true;
}

bool
config_load(arena_t* persistent, arena_t* scratch, const char* path, config_t* out)
{
	form_ctx_t	ctx    = { .sc = NULL, .scratch = scratch, .machine_state = NULL };
	s7_pointer	forms  = NULL;
	char*		text   = readfile(scratch, path);
	bool		ok     = false;

	memset(out, 0, sizeof(*out));
	if (!text)
		form_fail(&ctx, "cannot read file '%s'", path);
	else if (!(ctx.sc = s7_init()))
		form_fail(&ctx, "cannot start the Scheme reader");
	else
	{
		ok = read_forms(&ctx, text, &forms) && parse_forms(&ctx, forms, persistent, out);
		s7_free(ctx.sc);
	}
	if (!ok)
		memcpy(out->error, ctx.err, sizeof(out->error));
	return ok;
}
