#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
"$NR_ROOT/scripts/linux/fetch-deps.sh" 2>&1 | tee "$NR_BUILD_DIR/logs/dependencies.log"
# Re-read after acquiring a local compiler.
source "$NR_ROOT/scripts/linux/common.sh"
cmake -S "$NR_DEPS_DIR/filament" -B "$NR_BUILD_DIR/filament" -G Ninja \
  -DCMAKE_CXX_FLAGS="${CXXFLAGS:-}" \
  -DCMAKE_PROJECT_INCLUDE="$NR_ROOT/scripts/linux/filament-project.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$NR_DEPS_DIR/filament-install" \
  -DFILAMENT_SUPPORTS_VULKAN=ON -DFILAMENT_SUPPORTS_OPENGL=OFF \
  -DFILAMENT_SKIP_SAMPLES=ON -DFILAMENT_BUILD_TESTING=OFF -DFILAMENT_ENABLE_MATDBG=OFF -DFILAMENT_ENABLE_FGVIEWER=OFF \
  2>&1 | tee "$NR_BUILD_DIR/logs/filament-configure.log"
cmake --build "$NR_BUILD_DIR/filament" --target install --parallel "$NR_BUILD_JOBS" \
  2>&1 | tee "$NR_BUILD_DIR/logs/filament-build.log"
"$NR_PYTHON" "$NR_ROOT/scripts/linux/generate.py" all 2>&1 | tee "$NR_BUILD_DIR/logs/generation.log"
cmake -S "$NR_ROOT" -B "$NR_BUILD_DIR/native" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  2>&1 | tee "$NR_BUILD_DIR/logs/native-configure.log"
cmake --build "$NR_BUILD_DIR/native" --parallel "$NR_BUILD_JOBS" 2>&1 | tee "$NR_BUILD_DIR/logs/native-build.log"

# Bundle LLVM's small C++ runtime; the GPU/window stack remains supplied by the OS.
if [[ -d "$NR_DEPS_DIR/llvm/usr/lib/x86_64-linux-gnu" ]]; then
  mkdir -p "$NR_ROOT/build/lib" "$NR_ROOT/build/licenses/llvm"
  for library in libc++.so.1 libc++abi.so.1 libunwind.so.1; do
    cp -L "$NR_DEPS_DIR/llvm/usr/lib/x86_64-linux-gnu/$library" "$NR_ROOT/build/lib/$library"
  done
  for package in libc++1-18 libc++abi1-18 libunwind-18; do
    cp "$NR_DEPS_DIR/llvm/usr/share/doc/$package/copyright" "$NR_ROOT/build/licenses/llvm/$package-copyright"
  done
fi
mkdir -p "$NR_ROOT/build/licenses/filament"
cp "$NR_DEPS_DIR/filament/LICENSE" "$NR_ROOT/build/licenses/filament/LICENSE"
(
  cd "$NR_DEPS_DIR/filament"
  find libs third_party -type f \( -iname 'license*' -o -iname 'notice*' -o -iname 'copying*' -o -iname 'copyright*' \) -print0 |
    while IFS= read -r -d '' notice; do
      cp --parents "$notice" "$NR_ROOT/build/licenses/filament/"
    done
)
