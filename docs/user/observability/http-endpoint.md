# Health, readiness and metrics endpoint

An optional HTTP endpoint answers health and readiness probes and serves Prometheus metrics. It is off by default.

Add `(http ...)` to the `(server ...)` section, in any mode:

```scheme
(server (mode tcp) (port 1502)
  (http (port 9102) (bind "127.0.0.1")))

(device (name "Meter") (unit-id 1) (holding 0 (static 42)))
```

`port` defaults to 9102 and `bind` to `"127.0.0.1"`; use `"0.0.0.0"` (or `"::"`) to reach it from
another machine or from outside a container. In the TCP modes the port must differ from the Modbus
port. The endpoint is served by the same loop that answers Modbus requests.

## Paths

| Path | Answer |
|---|---|
| `/healthz` | `200 ok` whenever the server loop runs. If the loop stalls, the request times out, so it suits a liveness probe. |
| `/readyz` | `200 ready` while the server accepts requests; `503 not ready` before it starts and while it stops. Use it as a readiness probe. |
| `/metrics` | The [metrics](metrics/metrics.md), in the Prometheus text format. |

Other paths get 404 and methods other than GET and HEAD get 405. The optional
[runtime control API](api/control-api.md) under `/api/` is the exception.

## Limits

A connection is closed after one response or after 2 s, and at most 4 clients are served at once.
The endpoint is meant for probes and scrapers, not as a general web server.

```bash
curl -s http://127.0.0.1:9102/metrics | grep '^mogbus_requests_total'
```

## Scraping with Prometheus

```yaml
scrape_configs:
  - job_name: mogbus
    static_configs:
      - targets: ["127.0.0.1:9102"]
```

In Kubernetes, use `/readyz` and `/healthz` as probes; see [Kubernetes](../deployment/kubernetes.md).

## See also

- [Metrics](metrics/metrics.md)
- [Runtime control API](api/control-api.md)
- [Logs](logs.md)
- [Config reference: http](../config/server/index.md)
