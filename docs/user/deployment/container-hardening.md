# Container hardening and limits

The image needs nothing special from the container runtime, so it can run with a read-only root filesystem, no capabilities and tight resource limits.

## Non-root, read-only, no capabilities

The image sets `USER 10001:10001` and needs nothing else:

- mogbus writes no files; logs go to stderr (`docker logs`), so `--read-only` works without tmpfs mounts.
- It binds a port above 1024 and needs no capability, so `--cap-drop ALL` and
  `--security-opt no-new-privileges` are safe.
- It handles SIGTERM and SIGINT itself and forks no children, so it works as PID 1: `docker stop`
  ends it within a poll tick with exit status 0, and there are no zombies to reap. `--init` is not
  needed.

```bash
docker run -d --name mogbus --read-only --cap-drop ALL --security-opt no-new-privileges \
  --memory 64m --cpus 0.5 --pids-limit 64 -p 127.0.0.1:1502:1502 mogbus:0.0.1
```

## Resource limits

mogbus is single-threaded (one `poll` loop), allocates only from mmap arenas created at start-up,
and allocates nothing per request. Measured on Linux with the `minimal` profile: about 10 MB
resident (14 MB peak at start-up while the config is parsed), one thread. Memory does not grow with
traffic; at most 32 TCP clients are served at once. The shipped examples therefore use generous
limits:

| Limit | Value | Why |
|---|---|---|
| memory | 64 MiB (request 16 MiB in Kubernetes) | several times the working set, including many devices |
| CPU | 0.5 core | one thread; a simulator needs far less, a busy client cannot use more than one core |
| pids | 64 | one process, one thread; this only stops fork bombs from a compromised process |

Raise the memory limit for very large configs (many devices, machines or file records).

## See also

- [Run mogbus in Docker](docker.md)
- [Docker Compose](docker-compose.md)
- [Kubernetes](kubernetes.md)
