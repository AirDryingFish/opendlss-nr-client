#!/usr/bin/env bash
# Build the optional in-process RIFE Vulkan backend and stage the v4.6 weights.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${XDG_CACHE_HOME:-$HOME/.cache}/opendlss-nr-client
deps=${NR_DEPS_DIR:-$cache/native-build-deps}
objects=${NR_BUILD_DIR:-$cache/native-build}/rife
revision=a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7
mkdir -p "$deps" "$objects"
deps=$(cd -- "$deps" && pwd)
objects=$(cd -- "$objects" && pwd)
upstream=$deps/rife-ncnn-vulkan
if [[ ! -d $upstream/.git ]]; then
    git clone --filter=blob:none --no-checkout --depth 1 --branch 20221029 \
        https://github.com/nihui/rife-ncnn-vulkan.git "$upstream"
fi
[[ $(git -C "$upstream" rev-parse HEAD) == "$revision" ]] || { echo "Unexpected RIFE revision at $upstream" >&2; exit 1; }
if [[ ! -f $upstream/models/rife-v4.6/flownet.bin || ! -f $upstream/src/rife.cpp ]]; then
    git -C "$upstream" sparse-checkout set src models/rife-v4.6
    git -C "$upstream" checkout HEAD
fi
# The pinned source tree supplies the exact ncnn, glslang and libwebp revisions.
# libwebp is required by upstream CMake; the live plugin only links inference.
git -C "$upstream" submodule update --init --depth 1 src/ncnn src/libwebp
git -C "$upstream/src/ncnn" submodule update --init --depth 1 glslang
patch=$root/scripts/linux/rife/live-rgba.patch
if ! git -C "$upstream" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$upstream" apply "$patch"
fi
(
    cd "$upstream/models/rife-v4.6"
    sha256sum -c <<'HASHES'
f334ed2260149ce0188a6dcf049844e8b0cdd912e01cbcfb63553157d2508958  flownet.bin
724569596bcd1e7b9fa50455c604777ebed99746d2ef40aa86e31b5725f1053c  flownet.param
HASHES
)
# This plugin has a C ABI, so it can use the system C++ compiler independently of Filament.
if [[ -f $deps/compat-include/bits/syscall.h ]]; then
    export CFLAGS="-I\"$deps/compat-include\" ${CFLAGS:-}"
    export CXXFLAGS="-I\"$deps/compat-include\" ${CXXFLAGS:-}"
fi
cmake -S "$root/scripts/linux/rife" -B "$objects" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRIFE_SOURCE="$upstream"
cmake --build "$objects" --target live_rife --parallel "${NR_BUILD_JOBS:-4}"
model=$root/build/data/rife-v4.6
licenses=$root/build/licenses/rife
mkdir -p "$model" "$licenses" "$root/build/lib"
install -m 644 "$upstream/LICENSE" "$licenses/rife-ncnn-vulkan.LICENSE"
install -m 644 "$upstream/src/ncnn/LICENSE.txt" "$licenses/ncnn.LICENSE"
install -m 644 "$upstream/src/ncnn/glslang/LICENSE.txt" "$licenses/glslang.LICENSE"
# Readers only open complete files. An already running client can continue using its mapped plugin.
staged=
trap '[[ -z $staged ]] || rm -f -- "$staged"' EXIT
stage_file() {
    local source=$1 destination=$2 mode=$3
    if cmp -s "$source" "$destination"; then return; fi
    staged=$(mktemp "${destination}.XXXXXX")
    install -m "$mode" "$source" "$staged"
    mv -f "$staged" "$destination"
    staged=
}
for name in flownet.bin flownet.param; do
    stage_file "$upstream/models/rife-v4.6/$name" "$model/$name" 644
done
stage_file "$objects/liblive_rife.so" "$root/build/lib/liblive_rife.so" 755
echo "RIFE v4.6 Vulkan is ready for Live."
