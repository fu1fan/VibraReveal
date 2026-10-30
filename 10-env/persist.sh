#!/bin/sh
# Run before starting IDE processes; --report does not mutate state.
set -eu
exec python3 "$(dirname "$0")/persist.py" "$@"
