#!/bin/sh
# Install the same official Linux x86-64 Doxygen binary locally and in CI.
set -eu

destination=${1:-build/doc-tools}
release=1.14.0
archive=doxygen-$release.linux.bin.tar.gz
sha=e5d6ae24d0bf3f0cdc4d8f146726b89ca323922f19441af99b1872d503665ad6

mkdir -p "$destination"
cd "$destination"
curl --fail --location --silent --show-error --retry 3 \
    "https://github.com/doxygen/doxygen/releases/download/Release_1_14_0/$archive" \
    --output "$archive"
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check -
tar -xzf "$archive" "doxygen-$release/bin/doxygen"
"$PWD/doxygen-$release/bin/doxygen" --version
printf 'Doxygen binary: %s\n' "$PWD/doxygen-$release/bin/doxygen"