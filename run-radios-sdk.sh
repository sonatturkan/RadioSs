#!/bin/sh
set -eu
RADIOS_PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
export PYTHONDONTWRITEBYTECODE=1
exec python3 "$RADIOS_PROJECT_DIR/tools/radios_sdk.py" --project-root "$RADIOS_PROJECT_DIR" "$@"
