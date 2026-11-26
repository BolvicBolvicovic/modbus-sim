#!/bin/sh
# Optional visual check: screenshots of generated pages with Playwright's Chromium in podman.
# Usage: tools/docs/shoot.sh OUTDIR SITE_DIR spec...   (spec as in shoot.js, e.g. index.html:1440x900:full)
# SITE_DIR is mounted read-only and pages are opened over file://. Needs network once for npm.
set -eu

out=$1
site=$2
shift 2
mkdir -p "$out"
out=$(cd "$out" && pwd)
site=$(cd "$site" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
podman run --rm -v "$site":/site:ro -v "$out":/out -v "$here":/tools:ro \
	mcr.microsoft.com/playwright:v1.49.1-noble \
	sh -c 'mkdir -p /tmp/w && cd /tmp/w && cp /tools/shoot.js . && npm i --silent playwright-core@1.49.1 >/dev/null 2>&1 && node shoot.js /site /out "$@"' sh "$@"
