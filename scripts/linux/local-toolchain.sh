#!/usr/bin/env bash
# Ubuntu 24.04 amd64 packages, extracted only inside the user's dependency cache.
NR_LLVM_VERSION='1:18.1.3-1ubuntu1'
NR_LLVM_MARKER='ubuntu24.04-amd64-llvm18.1.3-1ubuntu1-v1'
NR_LLVM_PACKAGES=(
  clang-18 libclang-cpp18 libllvm18 libclang-common-18-dev libclang1-18
  llvm-18-linker-tools libc++-18-dev libc++1-18 libc++abi-18-dev libc++abi1-18
  libunwind-18-dev libunwind-18 libclang-rt-18-dev
)
nr_toolchain_files_present() {
  local prefix=$1 file
  [[ -x "$prefix/usr/bin/clang-18" && -x "$prefix/usr/bin/clang++-18" ]] || return 1
  for file in \
    usr/lib/llvm-18/lib/libLLVM.so.1 \
    usr/lib/llvm-18/lib/libclang-cpp.so.18.1 \
    usr/lib/llvm-18/lib/libclang-18.so.18 \
    usr/lib/llvm-18/lib/LLVMgold.so \
    usr/lib/llvm-18/include/c++/v1/vector \
    usr/lib/llvm-18/lib/libc++.so usr/lib/llvm-18/lib/libc++.a \
    usr/lib/llvm-18/lib/libc++abi.so usr/lib/llvm-18/lib/libunwind.so \
    usr/lib/llvm-18/lib/clang/18/include/stddef.h \
    usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a; do
    [[ -s "$prefix/$file" ]] || return 1
  done
}
nr_toolchain_ready() {
  local prefix=$1
  [[ -f "$prefix/.nr-toolchain-ready" ]] || return 1
  [[ $(cat "$prefix/.nr-toolchain-ready") == "$NR_LLVM_MARKER" ]] || return 1
  nr_toolchain_files_present "$prefix"
}
nr_prove_toolchain() (
  set -euo pipefail
  local prefix=$1 scratch
  nr_toolchain_files_present "$prefix" || exit 1
  export LD_LIBRARY_PATH="$prefix/usr/lib/x86_64-linux-gnu:$prefix/usr/lib/llvm-18/lib:${LD_LIBRARY_PATH:-}"
  [[ $("$prefix/usr/bin/clang++-18" --version) == *'version 18.1.3'* ]] || exit 1
  scratch=$(mktemp -d) || exit 1
  trap 'rm -rf -- "$scratch"' EXIT
  # Exercise libc++ headers, linking, exception unwinding and the runtime loader.
  printf '%s\n' '#include <vector>' '#include <stdexcept>' \
    'int main(){std::vector<int> v{18}; try{throw std::runtime_error("llvm");}catch(const std::exception&){return v[0]!=18;}}' \
    | "$prefix/usr/bin/clang++-18" -std=c++20 -stdlib=libc++ -xc++ - -o "$scratch/proof" || exit 1
  "$scratch/proof"
)
nr_install_toolchain() (
  set -euo pipefail
  local deps=$1 prefix="$1/llvm" package deb stage backup metadata
  nr_toolchain_ready "$prefix" && exit 0
  # The original complete install can be attested in place; do not re-download
  # or re-extract hundreds of MB just to introduce the readiness marker.
  if nr_prove_toolchain "$prefix"; then
    printf '%s\n' "$NR_LLVM_MARKER" > "$prefix/.nr-toolchain-ready.tmp"
    mv -- "$prefix/.nr-toolchain-ready.tmp" "$prefix/.nr-toolchain-ready"
    exit 0
  fi
  command -v apt-get >/dev/null || { echo 'Provide Clang >=17/libc++, or Ubuntu 24.04 amd64 apt package indexes.' >&2; exit 1; }
  mkdir -p "$deps/downloads"
  local debs=()
  for package in "${NR_LLVM_PACKAGES[@]}"; do
    deb="$deps/downloads/${package}_1%3a18.1.3-1ubuntu1_amd64.deb"
    if [[ ! -s "$deb" ]]; then
      (cd "$deps/downloads" && apt-get download "$package=$NR_LLVM_VERSION")
    fi
    # Never extract unrelated or incorrectly named/versioned cached packages.
    metadata=$(dpkg-deb -f "$deb" Package Version Architecture)
    [[ "$metadata" == "Package: $package"$'\n'"Version: $NR_LLVM_VERSION"$'\n''Architecture: amd64' ]] || {
      echo "Unexpected pinned toolchain package metadata: $deb" >&2; exit 1;
    }
    debs+=("$deb")
  done
  stage=$(mktemp -d "$deps/.llvm-stage.XXXXXX")
  trap 'rm -rf -- "$stage"' EXIT
  for deb in "${debs[@]}"; do dpkg-deb -x "$deb" "$stage"; done
  nr_prove_toolchain "$stage"
  printf '%s\n' "$NR_LLVM_MARKER" > "$stage/.nr-toolchain-ready"
  nr_toolchain_ready "$stage"
  backup="$stage.previous"
  if [[ -e "$prefix" ]]; then mv -- "$prefix" "$backup"; fi
  if ! mv -- "$stage" "$prefix"; then
    if [[ -e "$backup" ]]; then mv -- "$backup" "$prefix"; fi
    exit 1
  fi
  rm -rf -- "$backup"
)
