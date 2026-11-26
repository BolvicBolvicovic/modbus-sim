/**
 * @file
 * @brief Loads a Scheme config file into devices and server settings.
 */
#include "config/config.h"
#include "config/config_device.h"
#include "config/config_machine.h"
#include "device/machine.h"
#include "util/baud.h"
#include "util/clock.h"
#include "util/ip.h"
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
config_set_defaults(config_t* config)
{
	memset(config, 0, sizeof(*config));
	strcpy(config->bind_address, "127.0.0.1");
	config->mode              = SERVER_MODE_TCP;
	config->port              = SERVER_DEFAULT_PORT;
	config->idle_timeout_ms   = SERVER_DEFAULT_IDLE_TIMEOUT_MS;
	config->frame_gap_ms      = 0; /* auto: derive from baud, else default gap */
	config->baud              = SERVER_DEFAULT_BAUD;
	config->strict_t15        = false;
	config->broadcast_execute = false;
	config->data_bits         = 8;
	config->parity            = PARITY_NONE;
	config->stop_bits         = 1;
	config->tls_require_client_certificate = true;
	config->tls_min_version   = CONFIG_TLS_VERSION_1_2;
	config->tls_handshake_timeout_ms = SERVER_DEFAULT_TLS_HANDSHAKE_TIMEOUT_MS;
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
parse_tls(form_ctx_t* ctx, s7_pointer options, config_t* config)
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

	strcpy(config->tls_certificate, certificate);
	strcpy(config->tls_private_key, private_key);
	if (ca_certificate)
		strcpy(config->tls_ca_certificate, ca_certificate);
	if (ciphers)
		strcpy(config->tls1_2_ciphers, ciphers);
	if (ciphersuites)
		strcpy(config->tls1_3_ciphersuites, ciphersuites);
	config->tls_enabled               = true;
	config->tls_require_client_certificate = require_client;
	config->tls_min_version           = (config_tls_version_t)version;
	config->tls_handshake_timeout_ms  = (u32)handshake_timeout;
	return true;
}
#endif

/* (http (port N) (bind "ADDR") (control on)): the health, readiness, Prometheus and optional control endpoint, any mode. */
static bool
parse_http(form_ctx_t* ctx, s7_pointer options, config_t* config)
{
	static const char* const keys[] = { "port", "bind", "control" };
	s64		port = SERVER_DEFAULT_HTTP_PORT;
	const char*	bind = NULL;
	bool		control = false;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "http") ||
	    !form_opt_int(ctx, options, "port", 1, 65535, &port) ||
	    !form_opt_string(ctx, options, "bind", &bind) ||
	    !form_opt_bool(ctx, options, "control", &control))
		return false;
	if (bind && strlen(bind) >= SERVER_BIND_MAX)
		return form_fail(ctx, "http: bind address is too long (max %d characters)", SERVER_BIND_MAX - 1);
	if (bind && !ip_is_literal(bind))
		return form_fail(ctx, "http: bind address '%s' is not a valid IPv4 or IPv6 address", bind);
	strcpy(config->http_bind_address, bind ? bind : "127.0.0.1");
	config->http_port    = (u16)port;
	config->http_control = control;
	config->http_enabled = true;
	return true;
}

static bool
parse_server(form_ctx_t* ctx, s7_pointer form, config_t* config)
{
	static const char* const keys[] = { "mode", "path", "port", "bind", "seed", "idle-timeout-ms", "frame-gap-ms", "baud", "strict-t15", "broadcast-execute", "data-bits", "parity", "stop-bits", "tls", "http" };
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
	s64		port              = config->port;
	s64		seed              = config->seed;
	s64		idle              = config->idle_timeout_ms;
	s64		frame             = config->frame_gap_ms;
	s64		baud              = config->baud;
	s64		data_bits         = config->data_bits;
	s64		stop_bits         = config->stop_bits;
	u32		parity            = config->parity;
	u32		mode              = config->mode;
	u32		mode_index        = 0;
	bool		strict_t15        = config->strict_t15;
	bool		broadcast_execute = config->broadcast_execute;
	const char*	bind              = NULL;
	const char*	path              = NULL;
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
	if (!form_is_absent(ctx, tls_form) && !parse_tls(ctx, s7_cdr(tls_form), config))
		return false;
#endif

	if (!form_opt_int(ctx, options, "port", 1, 65535, &port))
		return false;

	if (!form_is_absent(ctx, form_find(ctx, options, "http")) &&
	    !parse_http(ctx, s7_cdr(form_find(ctx, options, "http")), config))
		return false;

	if (!form_opt_int(ctx, options, "seed", 0, 4294967295LL, &seed))
		return false;

	if (!form_opt_int(ctx, options, "idle-timeout-ms", 0, 86400000, &idle))
		return false;

	if (!form_opt_int(ctx, options, "frame-gap-ms", 1, 60000, &frame))
		return false;

	if (!form_opt_int(ctx, options, "baud", 1, 4000000, &baud))
		return false;
	if (baud > 0 && !baud_is_standard((u32)baud))
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

	if (bind && !ip_is_literal(bind))
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
		strcpy(config->bind_address, bind);
	if (path)
		strcpy(config->path, path);

	config->mode              = (server_mode_t)mode;
	config->port              = (u16)(config->tls_enabled && !port_given ? CONFIG_DEFAULT_TLS_PORT : port);
	if (config->http_enabled &&
	    config->http_port == config->port &&
	    (mode == SERVER_MODE_TCP || mode == SERVER_MODE_RTU_TCP || mode == SERVER_MODE_ASCII_TCP))
		return form_fail(ctx, "http: port %u is already the Modbus server port", config->port);
	config->seed              = (u32)seed;
	config->idle_timeout_ms   = (u32)idle;
	config->frame_gap_ms      = (u32)frame;
	config->baud              = (u32)baud;
	config->strict_t15        = strict_t15;
	config->broadcast_execute = broadcast_execute;
	config->data_bits         = (u32)data_bits;
	config->parity            = (parity_t)parity;
	config->stop_bits         = (u32)stop_bits;
	return true;
}

static bool
check_unit_ids_unique(form_ctx_t* ctx, const config_t* config)
{
	for (u32 i = 0; i < config->device_count; i++)
	{
		for (u32 j = i + 1; j < config->device_count; j++)
		{
			if (config->devices[i]->unit_id != config->devices[j]->unit_id)
				continue;
			return form_fail(ctx, "devices '%s' and '%s' both use unit-id %u",
					 config->devices[i]->name, config->devices[j]->name,
					 config->devices[i]->unit_id);
		}
	}
	return true;
}

static bool
check_mode_rules(form_ctx_t* ctx, const config_t* config)
{
	if (config->mode == SERVER_MODE_TCP ||
	    config->mode == SERVER_MODE_UDP)
		return true;

	for (u32 i = 0; i < config->device_count; i++)
	{
		u8 id = config->devices[i]->unit_id;

		if (id == 0 || id > 247)
			return form_fail(ctx, "device '%s': unit-id %u is not valid for RTU/ASCII (allowed: 1-247; 0 is broadcast, 248-255 reserved)",
					 config->devices[i]->name, id);
	}

	return true;
}

static bool
check_fault_modes(form_ctx_t* ctx, const config_t* config)
{
	bool tcp_like = config->mode == SERVER_MODE_TCP ||
			config->mode == SERVER_MODE_RTU_TCP ||
			config->mode == SERVER_MODE_ASCII_TCP;

	for (u32 i = 0; i < config->device_count; i++)
	{
		const device_t* dev = config->devices[i];
		bool base_checksum =
			(dev->faults.present & FAULT_BIT(FAULT_CORRUPT)) != 0 &&
			dev->faults.by_kind[FAULT_CORRUPT].corrupt_mode == FAULT_CORRUPT_CHECKSUM;
		bool base_disconnect =
			(dev->faults.present & FAULT_BIT(FAULT_DISCONNECT)) != 0;

		if ((config->mode == SERVER_MODE_TCP ||
		     config->mode == SERVER_MODE_UDP) &&
		    (base_checksum ||
		     device_machine_corrupt_mode_present(dev, FAULT_CORRUPT_CHECKSUM)))
			return form_fail(ctx, "device '%s': corrupt mode checksum is not valid in %s mode (MBAP frames have no checksum)",
					 dev->name, MODE_NAMES[config->mode]);
		if (!tcp_like &&
		    (base_disconnect ||
		     device_machine_fault_present(dev, FAULT_DISCONNECT)))
			return form_fail(ctx, "device '%s': (fault (kind disconnect)) is not valid in %s mode (TCP-like transports only)",
					 dev->name, MODE_NAMES[config->mode]);
	}
	return true;
}

static bool
parse_top_level_form(form_ctx_t* ctx, s7_pointer form, arena_t* arena, u64 now_ms,
		     config_t* config, bool* server_seen)
{
	const char*	head = form_head_name(form);

	if (!head)
		return form_fail(ctx, "expected (server ...) or (device ...), got %s", form_text(ctx, form));

	if (strcmp(head, "server") == 0)
	{
		if (*server_seen)
			return form_fail(ctx, "the file has more than one (server ...); only one is allowed");

		*server_seen = true;
		return parse_server(ctx, form, config);
	}

	if (strcmp(head, "device") != 0)
		return form_fail(ctx, "unknown top-level form '%s'. Use (server ...) or (device ...)", head);

	if (config->device_count >= SERVER_MAX_DEVICES)
		return form_fail(ctx, "too many devices (max %d)", SERVER_MAX_DEVICES);

	return config_parse_device(ctx, form, arena, now_ms,
				   &config->devices[config->device_count++]);
}

static bool
parse_forms(form_ctx_t* ctx, s7_pointer forms, arena_t* arena, config_t* config)
{
	u64	now_ms      = clock_now_ms();
	bool	server_seen = false;

	for (s7_pointer p = forms; s7_is_pair(p); p = s7_cdr(p))
	{
		if (!parse_top_level_form(ctx, s7_car(p), arena, now_ms, config, &server_seen))
			return false;
	}

	/* Machine point references were checked while each device was read; seeding and
	 * finalization wait for (server (seed N)), which may appear after the devices. */
	for (u32 i = 0; i < config->device_count; i++)
	{
		device_t* dev = config->devices[i];

		device_seed_machines(dev, config->seed);
		if (!config_machine_finalize(ctx, dev))
			return false;
	}

	if (config->device_count == 0)
		return form_fail(ctx, "the file has no (device ...) form");

	return check_unit_ids_unique(ctx, config) &&
	       check_mode_rules(ctx, config) &&
	       check_fault_modes(ctx, config);
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
config_load(arena_t* persistent, arena_t* scratch, const char* path,
	    config_t* out, char* err, u32 err_size)
{
	form_ctx_t	ctx    = {
		.sc = NULL, .err = err, .err_size = err_size,
		.scratch = scratch, .machine_state = NULL,
	};
	s7_pointer	forms  = NULL;
	char*		text   = readfile(scratch, path);
	bool		ok     = false;

	config_set_defaults(out);
	if (!text)
		return form_fail(&ctx, "cannot read file '%s'", path);

	ctx.sc = s7_init();
	if (!ctx.sc)
		return form_fail(&ctx, "cannot start the Scheme reader");

	ok = read_forms(&ctx, text, &forms) && parse_forms(&ctx, forms, persistent, out);
	s7_free(ctx.sc);
	return ok;
}
