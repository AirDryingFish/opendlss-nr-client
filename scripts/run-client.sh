#!/usr/bin/env bash
set -euo pipefail
client_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$client_root/scripts/client_launch.py" "$@"
