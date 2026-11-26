/* Prints "<struct>.<field> <offset> <size>" for the structs the Python package mirrors with ctypes.
 * `make python-tests` builds and runs it; test_layout compares the output with ctypes. */
#include "mogbus.h"
#include <stddef.h>
#include <stdio.h>

#define SIZE(T)		printf(#T " %zu\n", sizeof(T))
#define F(T, name)	printf(#T "." #name " %zu %zu\n", offsetof(T, name), sizeof(((T*)0)->name))

int
main(void)
{
	typedef mogbus_server_options_t O;
	typedef mogbus_point_t P;
	typedef mogbus_device_info_t I;

	SIZE(O);
	F(O, mode); F(O, bind_address); F(O, port); F(O, path); F(O, fd); F(O, seed);
	F(O, idle_timeout_ms); F(O, frame_gap_ms); F(O, baud); F(O, strict_t15);
	F(O, broadcast_execute); F(O, data_bits); F(O, parity); F(O, stop_bits); F(O, tls_enabled);
	F(O, tls_handshake_timeout_ms); F(O, tls_certificate); F(O, tls_private_key);
	F(O, tls_ca_certificate); F(O, tls_require_client_certificate); F(O, tls_min_version);
	F(O, tls_ciphers_1_2); F(O, tls_ciphersuites_1_3); F(O, http_bind_address); F(O, http_port); F(O, http_control);

	SIZE(P);
	F(P, space); F(P, address); F(P, type); F(P, word_order); F(P, kind); F(P, value); F(P, ramp);
	F(P, ramp.from); F(P, ramp.to); F(P, ramp.step); F(P, ramp.every_ms); F(P, ramp.mode);
	F(P, byte_order); F(P, length); F(P, text);

	SIZE(I);
	F(I, name); F(I, unit_id); F(I, point_count); F(I, faults); F(I, custom_fc_count);
	F(I, custom_fcs);
	F(I, faults[0].present); F(I, faults[0].scheduled); F(I, faults[0].percent);
	F(I, faults[0].delay_ms); F(I, faults[0].exception_code);
	F(I, custom_fcs[0].code); F(I, custom_fcs[0].name);
	printf("faults_count %d\n", MOGBUS_FAULT_KIND_COUNT);
	printf("space_count %d\n", MOGBUS_SPACE_COUNT);
	return 0;
}
