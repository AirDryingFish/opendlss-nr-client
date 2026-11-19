#!/usr/bin/env bash
set -euo pipefail
client_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ${1:-} == --live && $# -gt 1 ]]; then
  shift
  if [[ ! -x "$client_root/build/demo/dlss5-live" ]]; then
    echo "Build the live viewer first with scripts/linux/build.sh" >&2
    exit 1
  fi
  export SDL_VIDEODRIVER=x11
  exec "$client_root/build/demo/dlss5-live" "$@"
fi
exec python3 "$client_root/scripts/client_launch.py" "$@"
