#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
stb_include=${STB_INCLUDE_DIR:-"$root/third_party/filament/third_party/stb"}
cgltf_include=${CGLTF_INCLUDE_DIR:-"$root/third_party/filament/third_party/cgltf"}
if [[ ! -f "$cgltf_include/cgltf.h" ]]; then
  echo "cgltf.h missing; run scripts/linux/fetch-deps.sh or set CGLTF_INCLUDE_DIR" >&2
  exit 2
fi
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -I "$root/demo" \
  "$root/tests/client_controls_test.cpp" -o "$scratch/controls"
"$scratch/controls"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I "$root/demo" -I "$cgltf_include" -isystem "$stb_include" \
  "$root/tests/client_controls_import_test.cpp" "$root/demo/client_controls.cpp" -o "$scratch/import"
if [[ -f "$root/assets/test-static.glb" ]]; then
  "$scratch/import" "$root/assets/test-static.glb"
else
  "$scratch/import"
fi
