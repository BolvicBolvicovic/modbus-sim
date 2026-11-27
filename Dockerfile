# syntax=docker/dockerfile:1
# Build:  docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
# Run:    docker run --rm -p 127.0.0.1:1502:1502 mogbus:0.0.1
# See docs/user/deployment/docker.md.

# Debian 13 (trixie), pinned by tag and by the digest of the multi-arch index. The runtime stage
# (distroless, Debian 13 too) ships the same libssl3t64 major, so the binary runs as linked.
FROM debian:trixie-20261123@sha256:3cda3ca472ce45507a5109c6a3907093e24d9ae67d55954c9a07a45665dafba2 AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends clang make pkg-config libssl-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
# Default prod build: -O2, fortified, PIE, relro/now; WITH_TLS=1 fails if OpenSSL is missing.
RUN make -j"$(nproc)" WITH_TLS=1

FROM gcr.io/distroless/base-debian13:latest@sha256:1eb7e763df3f75a0d0eab87368f04e1bc61ce268caf3f2cbae765fa8fd4fd3e8

ARG VERSION=0.0.1
LABEL org.opencontainers.image.title="mogbus" \
      org.opencontainers.image.description="Modbus device simulator (TCP, UDP, RTU, ASCII) configured with a Scheme file" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.source="https://github.com/BolvicBolvicovic/mogbus" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.base.name="gcr.io/distroless/base-debian13:latest"

# distroless/base-debian13 is built from the same Debian 13 packages as the build stage: glibc,
# libssl3t64 and its libz/libzstd, no shell and no package manager. mogbus reads its own TLS
# files from the config and needs no system CA bundle.

COPY --from=build /src/mogbus /usr/local/bin/mogbus
COPY --from=build /src/profiles /usr/share/mogbus/profiles
COPY deploy/docker/mogbus.scm /etc/mogbus/mogbus.scm

# A fixed unprivileged id (no passwd entry needed) so volume permissions and Kubernetes
# runAsUser can match it. mogbus writes no files, so a read-only root filesystem works.
USER 10001:10001
WORKDIR /
EXPOSE 1502/tcp 1502/udp 9102/tcp

# Sends one Modbus request to the first device of the config; see docs/user/deployment/docker.md.
HEALTHCHECK --interval=10s --timeout=5s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/mogbus", "--health", "/etc/mogbus/mogbus.scm"]

# mogbus handles SIGTERM/SIGINT itself and forks nothing, so it is fine as PID 1.
ENTRYPOINT ["/usr/local/bin/mogbus"]
CMD ["/etc/mogbus/mogbus.scm"]
