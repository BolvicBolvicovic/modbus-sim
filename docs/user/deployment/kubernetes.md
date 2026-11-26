# Kubernetes

`deploy/kubernetes/mogbus.yaml` holds a ConfigMap with the config, a one-replica Deployment and a ClusterIP Service.

Build the image and push it to a registry the cluster can pull from, change `image:` to match, then:

```bash
kubectl apply -f deploy/kubernetes/mogbus.yaml
```

## What the pod does

- It runs as UID 10001 with `runAsNonRoot`, a read-only root filesystem,
  `allowPrivilegeEscalation: false`, all capabilities dropped and the `RuntimeDefault` seccomp
  profile, with CPU and memory requests and limits.
- The config enables the HTTP endpoint on port 9102, so the readiness probe is `httpGet /readyz` and
  the liveness probe `httpGet /healthz`. The pod carries the common `prometheus.io/scrape`, `port`
  and `path` annotations for Prometheus. Without `(http ...)`, use exec probes that run
  `mogbus --health` on the mounted config instead.
- The config is mounted read-only at `/etc/mogbus`. Change the ConfigMap and restart the pod: mogbus
  does not reload its config.

## One replica per plant

Run one replica per simulated plant: every replica holds its own device state, so a Service in front
of several replicas would answer from different devices.

## See also

- [Health, readiness and metrics endpoint](../observability/http-endpoint.md)
- [Health check](health-check.md)
- [Container hardening and limits](container-hardening.md)
