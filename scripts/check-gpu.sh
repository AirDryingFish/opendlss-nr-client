#!/usr/bin/env bash
set -euo pipefail
client_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -x "$client_root/build/nr-doctor" ]]; then
  echo 'GPU checker is not built. Run scripts/linux/build.sh first.' >&2
  exit 1
fi
export LD_LIBRARY_PATH="$client_root/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$client_root/build/nr-doctor" "$@"
