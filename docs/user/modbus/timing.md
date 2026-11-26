# Request timing

When values are computed, when responses go out, and how many clients are served at once.

- Values are computed from the clock when a request arrives. There is no background thread.
- Responses are sent as soon as they are ready, unless a `delay` fault holds them. A `disconnect`
  fault closes the client connection instead of sending the suppressed response.
- One connection handles one request at a time; many connections are served concurrently, up to 32
  in the TCP modes. More connections are refused.
- Optional `(server (idle-timeout-ms N))` closes a connection after `N` ms with no I/O progress
  (`0` disables it, the default). Connections waiting to send a delayed fault response are not
  idle. Idle closes are logged at `INFO`.
- TLS handshakes have their own deadline from `(tls (handshake-timeout-ms N))`, default `10000` ms,
  independent of the idle timeout. Stalled handshakes are closed at `WARN`; this does not change
  request timing after the handshake completes.
- TLS closes send a `close_notify` before the session is released. The wait is best-effort and
  bounded, so an unresponsive peer cannot delay shutdown indefinitely.
- RTU byte-stream transports find frame boundaries with inter-frame timing; see
  [Modbus RTU](../transports/transport-rtu.md).

## See also

- [Faults on the wire](fault-effects.md)
- [Modbus TCP](../transports/transport-tcp.md)
