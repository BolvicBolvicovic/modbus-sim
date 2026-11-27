# Metrics

Prometheus metrics served at `/metrics` by the [HTTP endpoint](../http-endpoint.md). One page per series; type in the box to filter.

Counters start at zero when the server starts. Series with a `unit` label are per configured
device. Notes:

- `mogbus --health` is a client too: each probe counts as an FC 08 request (`fc="8"`) to the first device.
- The UDP and serial modes have no connections, so the connection metrics stay at 0 there.
- A program that embeds the library can get the same text with @ref mogbus_server_metrics "mogbus_server_metrics()" and export
  it itself; see [Observe, shut down, lifetimes and threading](../../library/library-lifetimes.md).

```bash
curl -s http://127.0.0.1:9102/metrics | grep '^mogbus_device_'
```
