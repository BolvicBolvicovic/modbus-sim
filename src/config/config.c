#include "config/config.h"
#include "config/config_device.h"
#include "util/baud.h"
#include "util/clock.h"
#include "util/ip.h"
#include "vlib/macros.h"
#include "vlib/readfile.h"
#include <string.h>

/* Scheme helper run once: reads every form in a text without evaluating any of them.
 * Returns (#t form ...) on success or (#f "message") on a syntax error. */
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

static void
config_set_defaults(config_t* config)
{
	memset(config, 0, sizeof(*config));
	strcpy(config->bind_address, "0.0.0.0");
	config->port            = CONFIG_DEFAULT_PORT;
	config->idle_timeout_ms = CONFIG_DEFAULT_IDLE_TIMEOUT_MS;
	config->frame_gap_ms    = 0; /* auto: derive from baud, else default gap */
	config->baud            = CONFIG_DEFAULT_BAUD;
}

static bool
parse_server(form_ctx_t* ctx, s7_pointer form, config_t* config)
{
	static const char* const keys[] = { "port", "bind", "seed", "idle-timeout-ms", "frame-gap-ms", "baud" };
	s7_pointer	options = s7_cdr(form);
	s64		port    = config->port;
	s64		seed    = config->seed;
	s64		idle    = config->idle_timeout_ms;
	s64		frame   = config->frame_gap_ms;
	s64		baud    = config->baud;
	const char*	bind    = NULL;

	if (!form_check_keys(ctx, options, keys, ARRAY_COUNT(keys), "server"))
		return false;

	if (!form_opt_int(ctx, options, "port", 1, 65535, &port))
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

	if (!form_opt_string(ctx, options, "bind", &bind))
		return false;

	if (bind && strlen(bind) >= CONFIG_BIND_MAX)
		return form_fail(ctx, "server: bind address is too long (max %d characters)", CONFIG_BIND_MAX - 1);

	if (bind && !ip_is_ipv4_literal(bind))
		return form_fail(ctx, "server: bind address '%s' is not an IPv4 address like 127.0.0.1", bind);

	if (bind)
		strcpy(config->bind_address, bind);

	config->port            = (u16)port;
	config->seed            = (u32)seed;
	config->idle_timeout_ms = (u32)idle;
	config->frame_gap_ms    = (u32)frame;
	config->baud            = (u32)baud;
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

	if (config->device_count >= CONFIG_MAX_DEVICES)
		return form_fail(ctx, "too many devices (max %d)", CONFIG_MAX_DEVICES);

	return config_parse_device(ctx, form, arena, now_ms, &config->devices[config->device_count++]);
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

	if (config->device_count == 0)
		return form_fail(ctx, "the file has no (device ...) form");

	return check_unit_ids_unique(ctx, config);
}

/* Reads all forms of `text` with s7. Returns the list of forms, or false with ctx->err set. */
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
	form_ctx_t	ctx    = { .sc = NULL, .err = err, .err_size = err_size };
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
