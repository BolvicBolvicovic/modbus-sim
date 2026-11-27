# Step 2: Serve it from your own loop

> **Summary:** Pick a transport, create the server, and step it from the program's own loop instead of handing the thread over.

A server (`mogbus_server_t*`, created below) exposes the devices of the context at the moment it is created, over one transport. Devices added later are not served, so build the devices first, as in [step 1](library-device.md). A context holds up to `MOGBUS_MAX_SERVERS` servers.

## Several servers

Give each server an `id` in its options and assign devices with @ref mogbus_device_add_server "mogbus_device_add_server()" (call it again to put a device on several servers) before creating the servers. A server with an id serves the devices that list it; a server without one serves the devices that list none, which is every device unless some were assigned. Unit ids must be unique per server, so two devices of the context may share one when they are on different servers. All servers of the context live in the context's arena (see [lifetimes](library-lifetimes.md)) and are driven together by @ref mogbus_step "mogbus_step()" (one poll for all of them) or @ref mogbus_run "mogbus_run()"; @ref mogbus_stop "mogbus_stop()" stops them all. After @ref mogbus_load_config "mogbus_load_config()" of a file with several `(server ...)` forms, @ref mogbus_config_server_count "mogbus_config_server_count()" says how many there are and @ref mogbus_server_create_from_config_at "mogbus_server_create_from_config_at()" creates each one. `mogbus_server_create_from_config()` creates the first.

## Options

A server is described by a @ref mogbus_server_options_t "mogbus_server_options_t". Start from @ref mogbus_server_options_defaults "mogbus_server_options_defaults()", which matches the config defaults (TCP on `127.0.0.1:1502`, 8N1, no TLS), and change what you need. When the tutorial program runs with `--self-test` it needs a private loopback TCP port, so it sets the mode, the bind address and a free port (`free_port()` is the program's own helper: it binds port 0 and reads back the port the OS chose):

```c
mogbus_server_options_t options = mogbus_server_options_defaults();
...
options.mode = MOGBUS_MODE_TCP;
options.bind_address = "127.0.0.1";
options.port = free_port();
```

Every transport of the `mogbus` binary is a `mogbus_mode_t`: Modbus TCP (optionally TLS), RTU, ASCII, RTU-over-TCP, ASCII-over-TCP, and the UDP modes, plus PTY, serial-device and file-descriptor variants of RTU and ASCII. Which fields a mode reads is listed on @ref mogbus_server_options_t "the options struct"; fields a mode does not use are ignored. Strings you pass are copied. TCP-family and UDP listeners bind to loopback unless you set `bind_address` to `0.0.0.0` on purpose.

The `*_FD` modes take a byte-stream file descriptor you own (a socketpair or a PTY master), which makes a test harness possible without any network: one end is the simulator, the other the client under test. The library never closes it.

If you loaded a config in step 1, @ref mogbus_config_server_options "mogbus_config_server_options()" gives you its `(server ...)` section as options, and @ref mogbus_server_create_from_config "mogbus_server_create_from_config()" creates the server from it directly. The tutorial program starts from the file's options when there is one, and lets `--port` override the port.

## Check, then create

@ref mogbus_server_check "mogbus_server_check()" validates options exactly like create (device path, fd, serial line, string lengths, TLS files through a throwaway TLS context) without opening anything, so a port that is in use still passes. @ref mogbus_server_create "mogbus_server_create()" then starts the transport and hands the server back through its last parameter (NULL on failure). In `main()`, `ctx` is the context of step 1 and `g_server` a global that step 5 explains (it lets Ctrl-C stop the server):

```c
if (mogbus_server_check(ctx, &options) != MOGBUS_OK) { ... }
if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
{
	fprintf(stderr, "server: %s\n", mogbus_last_error(ctx));
	goto done;
}
g_server = server;
```

On failure @ref mogbus_server_create "mogbus_server_create()" returns a @ref mogbus_result_t "mogbus_result_t" that tells why and @ref mogbus_last_error "mogbus_last_error(ctx)" has the text; `mogbus_server_check()` returns the same codes: `MOGBUS_ERR_INVALID` (bad options, no devices, a limit), `MOGBUS_ERR_SERIAL` (serial device or PTY) or `MOGBUS_ERR_LISTEN` (port in use or TLS setup). The `mogbus` binary maps serial to exit status 1 and listen to 2. A TLS server needs a library built with TLS; without it creation fails with an error.

## The loop

You own the thread. @ref mogbus_server_step "mogbus_server_step()" runs one poll iteration (accept, read, answer, write whatever is ready) and returns after at most `max_wait_ms` (0 = do not block, -1 = until the server's next deadline). Interleave it with your own work. In the tutorial program, `now_s()` is the program's clock in seconds, `st` the `struct station` of step 1, `speed` the `--speed` factor and `simulate()` the process model of [step 3](library-simulate.md); each pass of the loop advances the simulation by the real time elapsed since the last pass:

```c
while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
{
	double t = now_s();

	simulate(&st, (t - last) * speed);   /* step 3 */
	last = t;
	...
}
```

It returns `MOGBUS_STEP_CONTINUE`, `MOGBUS_STEP_DONE` (stopped, or the byte-stream peer went away) or `MOGBUS_STEP_ERROR` (poll failed). The wait doubles as your loop's timer: 100 ms here means the simulation advances at least ten times a second even with no clients. If you have nothing else to do, @ref mogbus_server_run "mogbus_server_run()" steps until stopped for you.

Requests are answered inside `mogbus_server_step`, so the simulation, the access hook and a host write never run concurrently with a request: a context has one lock, held while a request is answered and released while the step waits (see [threading](library-lifetimes.md#threading)). A host that wants a background thread can run `mogbus_server_run()` on it and keep calling `mogbus_device_*` from the others. A long computation between steps of your own loop still delays every client.

## Check that it answers

@ref mogbus_probe "mogbus_probe()" is what `mogbus --health` uses: one FC 08 Return Query Data request to a unit of a server, `MOGBUS_OK` for the echo or a Modbus exception (on failure the message is `mogbus_last_error(NULL)`). It blocks with a deadline, so the thread that steps the server cannot call it. The program's helper `probe_self()` therefore forks a child that probes while the parent keeps stepping the server until the child exits (`options` and `unit` are the server options and unit id from above):

```c
pid_t pid = fork();
if (pid == 0)
	_exit(mogbus_probe(options, unit, 2000) == MOGBUS_OK ? 0 : 1);
for (...)
{
	if (waitpid(pid, &status, WNOHANG) == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	mogbus_server_step(server, 5);
}
```

In a real program a probe thread or an external `mogbus --health` does the same. It works for the TCP, UDP, RTU-over-TCP/UDP and ASCII-over-TCP/UDP modes (a wildcard bind is reached through loopback, a TLS server is only connected to) and rejects serial and fd modes. A fault that drops or delays replies on the probed unit fails it. With `--self-test` the program prints:

```text
  ok   mogbus_probe answers from a child process
```

## The program as its own client

Everything the tutorial shows about requests is checked by `--self-test`, a mode of the tutorial program (not of mogbus) in which the program sends Modbus TCP requests to its own server and prints an `ok` or `FAIL` line for each expectation, as quoted in the steps. It needs no second thread or process because it steps the server itself. The helper `exchange()` writes one request on a connected socket `fd`, then steps the server between reads until the reply is complete:

```c
for (int i = 0; i < 2000; i++)
{
	if (mogbus_server_step(server, 1) != MOGBUS_STEP_CONTINUE) return -1;
	if (poll(&pfd, 1, 0) > 0) { n = read(fd, reply + got, 260 - got); ... }
	if (got >= 7 && got >= 6 + (size_t)(reply[4] << 8 | reply[5])) return (int)got - 7;
}
```

The checks that follow use it, and the reads are real Modbus traffic:

```text
  ok   FC 03 reads the setpoint, 2.5 m
  ok   FC 03 reads the site name string
```

The same trick tests any harness that needs a client and a server in one process. The self-test is how the tutorial program is verified, so the `ok` lines in these pages can be reproduced by running it.

## An HTTP endpoint and the web UI

The options also carry the observability endpoint. With `--http PORT` the tutorial program sets three fields before creating the server (`http_port` is the number from the command line):

```c
options.http_bind_address = "127.0.0.1";
options.http_port = http_port;
options.http_ui = true;
```

`http_bind_address` (NULL = off) serves `/healthz`, `/readyz` and `/metrics`; `http_control` adds the unauthenticated `/api/` [runtime control API](../observability/api/control-api.md); `http_ui` serves the [web UI](../observability/web-ui.md) on `/` and implies the control API. `http_allowed_hosts` lists extra Host names those two accept. The endpoint is part of the same step loop; keep it on loopback, since the control API has no authentication.

## Next

- [Step 3: Simulate the process from the host](library-simulate.md)
