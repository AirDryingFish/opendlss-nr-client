#!/usr/bin/env bash
# Install MAKO FG by Eugenio Segala for local, non-commercial use.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cache=${XDG_CACHE_HOME:-$HOME/.cache}/opendlss-nr-client
deps=${NR_DEPS_DIR:-$cache/native-build-deps}
objects=${NR_BUILD_DIR:-$cache/native-build}/mako
revision=18d7d690515a658c993f84029b245cb710cb83f2
sdk=$deps/mako-fg-$revision
src=$root/scripts/linux/mako
if [[ $(uname -sm) != 'Linux x86_64' ]]; then
    echo "The MAKO Live adapter requires x86_64 Linux." >&2
    exit 1
fi
mkdir -p "$sdk" "$objects"
sdk=$(cd -- "$sdk" && pwd)
scratch=$(mktemp -d "$sdk/.download.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
while read -r digest file; do
    if [[ -f $sdk/$file ]] && (cd "$sdk" && echo "$digest  $file" | sha256sum -c --status); then
        continue
    fi
    mkdir -p "$sdk/$(dirname -- "$file")"
    curl --fail --location --show-error --silent --retry 3 -o "$scratch/file" \
        "https://huggingface.co/eugeniosegala/MAKO-FG/resolve/$revision/$file"
    echo "$digest  $scratch/file" | sha256sum -c --status || { echo "MAKO checksum mismatch: $file" >&2; exit 1; }
    mv "$scratch/file" "$sdk/$file"
done < "$src/SHA256SUMS"
cmake -S "$src" -B "$objects" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAKO_SDK="$sdk"
cmake --build "$objects" --target live_mako --parallel "${NR_BUILD_JOBS:-4}"

model=$root/build/data/mako-fg-v1.24/model
notices=$root/build/licenses/mako
mkdir -p "$model" "$notices" "$root/build/lib"
install -m 644 "$sdk"/model/{weights.bin,ops.spv,transport.spv,mixture.spv} "$model/"
install -m 644 "$sdk"/{LICENSE,MODEL_CARD.md} "$sdk/runtime/THIRD_PARTY_NOTICES" "$notices/"
staged=$(mktemp "$root/build/lib/.live_mako.XXXXXX")
trap 'rm -rf "$scratch"; rm -f "$staged"' EXIT
install -m 755 "$objects/liblive_mako.so" "$staged"
mv -f "$staged" "$root/build/lib/liblive_mako.so"
echo "MAKO FG v1.24 is ready for Live. Model and runtime are for non-commercial use."
