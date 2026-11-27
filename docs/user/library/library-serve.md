# Step 2: Serve it from your own loop

Pick a transport, create the server, and step it from the program's own loop instead of handing the thread over.

A server (@ref mogbus_server_t "mogbus_server_t") exposes every device the context has at the moment it is created, over one transport. Devices added later are not served, so build the devices first. A context holds up to `MOGBUS_MAX_SERVERS` servers.

## Options

Start from @ref mogbus_server_options_defaults "mogbus_server_options_defaults()", which matches the config defaults (TCP on `127.0.0.1:1502`, 8N1, no TLS) and change what you need (@ref mogbus_server_options_t "mogbus_server_options_t"):

```c
mogbus_server_options_t options = mogbus_server_options_defaults();
...
options.mode = MOGBUS_MODE_TCP;
options.bind_address = "127.0.0.1";
options.port = free_port();
```

Every transport of the `mogbus` binary is a `mogbus_mode_t`: Modbus TCP (optionally TLS), RTU, ASCII, RTU-over-TCP, ASCII-over-TCP, and the UDP modes, plus PTY, serial-device and file-descriptor variants of RTU and ASCII. Which fields a mode reads is listed on @ref mogbus_server_options_t "the options struct"; fields a mode does not use are ignored. Strings you pass are copied. TCP-family and UDP listeners bind to loopback unless you set `bind_address` to `0.0.0.0` on purpose.

The `*_FD` modes take a byte-stream file descriptor you own (a socketpair or a PTY master), which makes a test harness possible without any network: one end is the simulator, the other the client under test. The library never closes it.

If you loaded a config, @ref mogbus_config_server_options "mogbus_config_server_options()" gives you its `(server ...)` section as options, and @ref mogbus_server_create_from_config "mogbus_server_create_from_config()" creates the server from it directly. The pump-station program starts from the file's options when there is one, lets `--port` override the port, and forces loopback TCP in `--self-test`.

## Check, then create

@ref mogbus_server_check "mogbus_server_check()" validates options exactly like create (device path, fd, serial line, string lengths, TLS files through a throwaway TLS context) without opening anything, so a port that is in use still passes. @ref mogbus_server_create "mogbus_server_create()" then starts the transport:

```c
if (!mogbus_server_check(ctx, &options, err, sizeof(err))) { ... }
server = mogbus_server_create(ctx, &options, err, sizeof(err), &status);
if (!server)
{
	fprintf(stderr, "server (status %d): %s\n", (int)status, err);
	goto done;
}
g_server = server;
```

On failure `status` (@ref mogbus_error_t "mogbus_error_t") tells why: `MOGBUS_ERR_INVALID` (bad options, no devices, a limit), `MOGBUS_ERR_SERIAL` (serial device or PTY) or `MOGBUS_ERR_LISTEN` (port in use or TLS setup). The binary maps serial to exit status 1 and listen to 2. A TLS server needs a build with TLS; without it creation fails with an error.

## The loop

You own the thread. @ref mogbus_server_step "mogbus_server_step()" runs one poll iteration (accept, read, answer, write whatever is ready) and returns after at most `max_wait_ms` (0 = do not block, -1 = until the server's next deadline). Interleave it with your own work:

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

One thread does everything. Requests are answered inside `mogbus_server_step`, so the simulation, the access hook and the host writes of the next step never run concurrently with a request. That is why the library needs no locks, and also why the step must not be starved: a long computation between steps delays every client.

## Check that it answers

@ref mogbus_probe "mogbus_probe()" is what `mogbus --health` uses: one FC 08 Return Query Data request to a unit of a server, true for the echo or a Modbus exception. It blocks with a deadline, so the thread that steps the server cannot call it. The example forks and probes from the child while the parent keeps stepping:

```c
pid_t pid = fork();
if (pid == 0)
	_exit(mogbus_probe(options, unit, 2000, err, sizeof(err)) ? 0 : 1);
for (...)
{
	if (waitpid(pid, &status, WNOHANG) == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	mogbus_server_step(server, 5);
}
```

In a real program a probe thread or an external `mogbus --health` does the same. It works for the TCP, UDP, RTU-over-TCP/UDP and ASCII-over-TCP/UDP modes (a wildcard bind is reached through loopback, a TLS server is only connected to) and rejects serial and fd modes. A fault that drops or delays replies on the probed unit fails it.

```text
  ok   mogbus_probe answers from a child process
```

## The self-test client

`--self-test` makes the program its own Modbus TCP client in the same thread. `exchange()` sends one request and steps the server between reads until the reply is complete, so no second thread or process is needed:

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

The same trick tests any harness that needs a client and a server in one process.

## An HTTP endpoint and the web UI

The options also carry the observability endpoint. `--http PORT` sets three fields:

```c
options.http_bind_address = "127.0.0.1";
options.http_port = http_port;
options.http_ui = true;
```

`http_bind_address` (NULL = off) serves `/healthz`, `/readyz` and `/metrics`; `http_control` adds the unauthenticated `/api/` [runtime control API](../observability/api/control-api.md); `http_ui` serves the [web UI](../observability/web-ui.md) on `/` and implies the control API. `http_allowed_hosts` lists extra Host names those two accept. The endpoint is part of the same step loop; keep it on loopback, since the control API has no authentication.

## Next

- [Step 3: Simulate the process from the host](library-simulate.md)
