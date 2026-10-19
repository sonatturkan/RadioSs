#!/bin/sh
set -eu
RADIOS_PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
export PYTHONDONTWRITEBYTECODE=1
# Inside an application sandbox (for example the VS Code flatpak terminal) the
# host's data service cannot be recognised as ours, so the launch stops with
# "port kullanımda". Hand the whole launch to the host session instead.
if [ -f /.flatpak-info ] && [ -z "${RADIOS_ON_HOST:-}" ] && command -v flatpak-spawn >/dev/null 2>&1; then
    exec flatpak-spawn --host --directory="$RADIOS_PROJECT_DIR" --env=RADIOS_ON_HOST=1 \
        sh "$RADIOS_PROJECT_DIR/run-radios.sh" "$@"
fi
exec python3 "$RADIOS_PROJECT_DIR/tools/radios_launch.py" --project-root "$RADIOS_PROJECT_DIR" "$@"
