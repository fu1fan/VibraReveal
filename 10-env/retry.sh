#!/bin/sh
# Retry transient network failures, but fail the image build after exhaustion.
set -eu
attempt=1
while :; do
    if "$@"; then exit 0; else status=$?; fi
    if [ "$attempt" -ge 5 ]; then exit "$status"; fi
    echo "Network install failed; retry $attempt/4: $1" >&2
    sleep "$((attempt * 5))"
    attempt=$((attempt + 1))
done
