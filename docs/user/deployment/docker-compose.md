# Docker Compose

`deploy/docker-compose.yml` builds the image and runs it hardened, with the config mounted from the repository.

It builds the image, publishes 1502 on loopback, bind-mounts `deploy/docker/mogbus.scm` over the
default config, and sets `user`, `read_only`, `cap_drop: [ALL]`, `no-new-privileges`, the health
check, `cpus`, `mem_limit`, `pids_limit` and `restart: unless-stopped`.

```bash
docker compose -f deploy/docker-compose.yml up --build
```

To use your own config, change the bind mount in the file to point at it. If it uses another path
inside the container, change the `healthcheck:` too, so it checks the same config. See
[Health check](health-check.md).

## See also

- [Run mogbus in Docker](docker.md)
- [Container hardening and limits](container-hardening.md)
- [Kubernetes](kubernetes.md)
