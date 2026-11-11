#!/usr/bin/env bash
set -euo pipefail
NR_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
NR_WORK=${XDG_CACHE_HOME:-$HOME/.cache}/opendlss-nr-client
NR_DEPS_DIR=${NR_DEPS_DIR:-$NR_WORK/native-build-deps}
NR_BUILD_DIR=${NR_BUILD_DIR:-$NR_WORK/native-build}
NR_BUILD_JOBS=${NR_BUILD_JOBS:-4}
mkdir -p "$NR_DEPS_DIR" "$NR_BUILD_DIR/logs"
NR_DEPS_DIR=$(cd -- "$NR_DEPS_DIR" && pwd)
NR_BUILD_DIR=$(cd -- "$NR_BUILD_DIR" && pwd)
export NR_ROOT NR_DEPS_DIR NR_BUILD_DIR NR_BUILD_JOBS
source "$NR_ROOT/scripts/linux/local-toolchain.sh"
# Debian/Ubuntu extraction is entirely user-local, including the runtime loader path.
if [[ -z "${CXX:-}" ]] && command -v clang++ >/dev/null &&
   clang++ --version | head -1 | grep -Eq 'version (1[7-9]|[2-9][0-9])\.' &&
   printf '#include <vector>\n' | clang++ -stdlib=libc++ -xc++ -fsyntax-only - >/dev/null 2>&1; then
  export CC=clang CXX=clang++
fi
if [[ -z "${CXX:-}" ]] && nr_toolchain_ready "$NR_DEPS_DIR/llvm"; then
  export CC="$NR_DEPS_DIR/llvm/usr/bin/clang-18"
  export CXX="$NR_DEPS_DIR/llvm/usr/bin/clang++-18"
fi
if nr_toolchain_ready "$NR_DEPS_DIR/llvm"; then
  export LD_LIBRARY_PATH="$NR_DEPS_DIR/llvm/usr/lib/x86_64-linux-gnu:$NR_DEPS_DIR/llvm/usr/lib/llvm-18/lib:${LD_LIBRARY_PATH:-}"
fi
if [[ -f "$NR_DEPS_DIR/compat-include/bits/syscall.h" ]]; then
  if [[ "${CFLAGS:-}" != *"$NR_DEPS_DIR/compat-include"* ]]; then
    export CFLAGS="-I\"$NR_DEPS_DIR/compat-include\" ${CFLAGS:-}"
  fi
  if [[ "${CXXFLAGS:-}" != *"$NR_DEPS_DIR/compat-include"* ]]; then
    export CXXFLAGS="-I\"$NR_DEPS_DIR/compat-include\" ${CXXFLAGS:-}"
  fi
fi
export NR_PYTHON=${NR_PYTHON:-python3}
