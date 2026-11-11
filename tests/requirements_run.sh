#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "$root/tests/test_requirements.cpp" -o "$scratch/requirements"
"$scratch/requirements"
compat=()
if [[ -n "${NR_DEPS_DIR:-}" && -f "$NR_DEPS_DIR/compat-include/bits/syscall.h" ]]; then
  compat=(-I "$NR_DEPS_DIR/compat-include")
fi
# Discard the Context GPU methods; the feature-chain constructor itself needs no loader.
"${CXX:-c++}" -std=c++20 -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -DVK_NO_PROTOTYPES -DVK_ENABLE_BETA_EXTENSIONS "${compat[@]}" \
  -I "$root/tools/Vulkan-Headers/include" -I "$root/tools/volk" -I "$root/src" \
  "$root/tests/test_vulkan_features.cpp" "$root/src/vk_context.cpp" -o "$scratch/features"
"$scratch/features"
