#!/usr/bin/env bash
# Pinned source dependencies. No sudo, driver changes or global installations.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/common.sh"
clone() {
  local name=$1 tag=$2 url=$3 commit=$4
  if [[ ! -f "$NR_DEPS_DIR/$name/.git/HEAD" ]]; then
    if [[ "$name" == filament ]]; then
      # Dawn and documentation dominate the source archive; neither participates
      # in this Vulkan-only desktop build. Git fetches all remaining build blobs.
      git -c core.autocrlf=false clone --filter=blob:none --no-checkout --depth 1 --branch "$tag" "$url" "$NR_DEPS_DIR/$name"
      git -C "$NR_DEPS_DIR/$name" sparse-checkout set --no-cone '/*' '!/docs/' '!/docs_src/' '!/third_party/dawn/' '!/third_party/models/' '!/third_party/environments/' '!/third_party/textures/' '!/third_party/libsdl2/test/' '!/third_party/glslang/Test/' '!/third_party/spirv-cross/reference/' '!/android/' '!/ios/' '!/art/' '!/web/'
      git -C "$NR_DEPS_DIR/$name" checkout "$tag"
    else
      git -c core.autocrlf=false clone --depth 1 --branch "$tag" "$url" "$NR_DEPS_DIR/$name"
    fi
  fi
  # Resume a clone interrupted after metadata acquisition.
  if [[ "$name" == filament && ! -f "$NR_DEPS_DIR/$name/CMakeLists.txt" ]]; then
    git -C "$NR_DEPS_DIR/$name" checkout "$tag"
  fi
  [[ $(git -C "$NR_DEPS_DIR/$name" rev-parse HEAD) == "$commit" ]] || { echo "$name commit mismatch" >&2; exit 1; }
  [[ $(git -C "$NR_DEPS_DIR/$name" describe --tags --exact-match HEAD) == "$tag" ]] || { echo "$name must be $tag" >&2; exit 1; }
}
clone Vulkan-Headers v1.4.363 https://github.com/KhronosGroup/Vulkan-Headers.git 6802bb4733b63ed5efd3adb308a6c885ef180ea1
clone volk vulkan-sdk-1.4.357.0 https://github.com/zeux/volk.git 776893306c5d3b22b6185b5d4a258b81d94572bf
clone filament v1.77.0 https://github.com/google/filament.git b01d8a12f283876b337d678e4c74081ae14e20da
if git -C "$NR_DEPS_DIR/filament" apply --check "$NR_ROOT/third_party/filament.patch" 2>/dev/null; then
  git -C "$NR_DEPS_DIR/filament" apply "$NR_ROOT/third_party/filament.patch"
else
  git -C "$NR_DEPS_DIR/filament" apply --reverse --check "$NR_ROOT/third_party/filament.patch"
fi
mkdir -p "$NR_DEPS_DIR/downloads"
if [[ -z "${CXX:-}" ]]; then
  if [[ ! -x "$NR_DEPS_DIR/llvm/usr/bin/clang++-18" ]]; then
    command -v apt-get >/dev/null || { echo 'Install Clang >=17 and libc++ development files, or provide CC/CXX.' >&2; exit 1; }
    local_packages=()
    for package in clang-18 libclang-cpp18 libllvm18 libclang-common-18-dev libclang1-18 llvm-18-linker-tools libc++-18-dev libc++1-18 libc++abi-18-dev libc++abi1-18 libunwind-18-dev libunwind-18 libclang-rt-18-dev; do
      local_packages+=("$package=1:18.1.3-1ubuntu1")
    done
    (cd "$NR_DEPS_DIR/downloads" && apt-get download "${local_packages[@]}")
    mkdir -p "$NR_DEPS_DIR/llvm"
    for deb in "$NR_DEPS_DIR"/downloads/*.deb; do dpkg-deb -x "$deb" "$NR_DEPS_DIR/llvm"; done
  fi
fi
# Some hosts have a damaged generated glibc syscall header. Test it with the
# selected compiler and supply a pristine user-local header only when needed.
source "$NR_ROOT/scripts/linux/common.sh"
if ! printf '#include <sys/syscall.h>\n' | "$CC" -x c -fsyntax-only - >/dev/null 2>&1; then
  if [[ ! -f "$NR_DEPS_DIR/compat-include/bits/syscall.h" ]]; then
    command -v apt-get >/dev/null || { echo 'System sys/syscall.h is broken; repair development headers or supply a local include overlay.' >&2; exit 1; }
    (cd "$NR_DEPS_DIR/downloads" && apt-get download libc6-dev)
    for deb in "$NR_DEPS_DIR"/downloads/libc6-dev_*.deb; do dpkg-deb -x "$deb" "$NR_DEPS_DIR/glibc-header-package"; done
    mkdir -p "$NR_DEPS_DIR/compat-include/bits"
    cp "$NR_DEPS_DIR/glibc-header-package/usr/include/x86_64-linux-gnu/bits/syscall.h" "$NR_DEPS_DIR/compat-include/bits/syscall.h"
  fi
  printf '#include <sys/syscall.h>\n' | "$CC" -I"$NR_DEPS_DIR/compat-include" -x c -fsyntax-only -
fi
# glslang's prebuilt Linux release avoids a second SPIR-V toolchain build.
if [[ ! -x "$NR_DEPS_DIR/glslang/bin/glslang" ]]; then
  curl -fL --retry 3 https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-linux-x86_64-release.zip -o "$NR_DEPS_DIR/downloads/glslang.zip"
  printf '%s  %s\n' a3fc4f083b1793eb53e55fa3577ac9649ffbe0340715e30d116290fb5382393f "$NR_DEPS_DIR/downloads/glslang.zip" | sha256sum -c -
  mkdir -p "$NR_DEPS_DIR/glslang"
  unzip -q -o "$NR_DEPS_DIR/downloads/glslang.zip" -d "$NR_DEPS_DIR/glslang"
fi
mkdir -p "$NR_ROOT/tools" "$NR_ROOT/third_party"
for name in Vulkan-Headers volk glslang; do ln -sfn "$NR_DEPS_DIR/$name" "$NR_ROOT/tools/$name"; done
ln -sfn "$NR_DEPS_DIR/filament" "$NR_ROOT/third_party/filament"
mkdir -p "$NR_DEPS_DIR/filament-install"
ln -sfn "$NR_DEPS_DIR/filament-install" "$NR_ROOT/third_party/filament-install"
glslang_version=$("$NR_DEPS_DIR/glslang/bin/glslang" --version)
[[ "$glslang_version" == *'16.6.0'* ]] || { echo 'glslang16.6.0 required' >&2; exit 1; }
printf '%s\n' "$glslang_version"
