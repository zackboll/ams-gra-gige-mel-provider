#!/usr/bin/env bash
# Explicit, opt-in provisioning of the official EMVA GenTL 1.6 C header for the
# optional GIGE_MEL_BUILD_GENTL_PROBE feasibility build (GIGE-MEL-GENTL-001).
# Normal CMake configuration and builds never call this script or download
# anything. The header is NOT committed: the GenICam License permits
# redistribution only unmodified, and this repository vendors no SDK files.
#
# Usage:
#   scripts/provision_gentl_header.sh PREFIX [--source emva|aravis] [--archive PATH]
#
# Installs PREFIX/include/GenTL.h and PREFIX/PROVENANCE.txt. Either source
# yields byte-identical content; the header itself is always SHA-256 verified.
#   emva   (default) GenICam_Package_2026.07.zip from emva.org (about 130 MiB),
#          checked against the SHA-256 that EMVA publishes on its download page.
#   aravis aravis-0.9.2.tar.xz from the Aravis GitHub release (under 1 MiB),
#          which redistributes the same unmodified header as src/gentl/GenTL_v1_6.h.
# Afterwards configure with -DGIGE_MEL_BUILD_GENTL_PROBE=ON
#   -DGIGE_MEL_GENTL_INCLUDE_DIR=PREFIX/include
set -euo pipefail

readonly HEADER_SHA256="5b656a413f4cdc347e80494b87f94b09ac925e391a9022edb7f028d996e35e4e"
readonly EMVA_URL="https://www.emva.org/wp-content/uploads/GenICam_Package_2026.07.zip"
readonly EMVA_SHA256="e74b8d982c3e7697399b03c254b6e8c596eddf0279959eba7de18734e279b64f"
readonly EMVA_MEMBER="GenTL/GenTL.h"
readonly ARAVIS_URL="https://github.com/AravisProject/aravis/releases/download/0.9.2/aravis-0.9.2.tar.xz"
readonly ARAVIS_SHA256="3bf0e79e8978c561d5af04c9958350f91fdeecaa2c2d96ff48420f0ae017af86"
readonly ARAVIS_MEMBER="aravis-0.9.2/src/gentl/GenTL_v1_6.h"

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; }

if [[ $# -lt 1 || "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    [[ $# -lt 1 ]] && exit 2
    exit 0
fi
prefix="$1"
shift
source="emva"
archive=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source) source="${2:?--source needs emva or aravis}"; shift 2 ;;
        --archive) archive="${2:?--archive needs a path}"; shift 2 ;;
        *) echo "provision_gentl_header: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case "$source" in
    emva) url="$EMVA_URL"; archive_sha="$EMVA_SHA256"; member="$EMVA_MEMBER" ;;
    aravis) url="$ARAVIS_URL"; archive_sha="$ARAVIS_SHA256"; member="$ARAVIS_MEMBER" ;;
    *) echo "provision_gentl_header: --source must be emva or aravis" >&2; exit 2 ;;
esac
for tool in sha256sum tar; do
    command -v "$tool" >/dev/null || { echo "provision_gentl_header: '$tool' is required" >&2; exit 1; }
done
[[ "$source" != emva ]] || command -v unzip >/dev/null || {
    echo "provision_gentl_header: 'unzip' is required for --source emva" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/gentl-header.XXXXXX")"
trap 'rm -rf "$work"' EXIT
if [[ -z "$archive" ]]; then
    command -v curl >/dev/null || { echo "provision_gentl_header: 'curl' is required to download" >&2; exit 1; }
    archive="$work/$(basename "$url")"
    echo "provision_gentl_header: downloading $url"
    curl --fail --location --silent --show-error --retry 3 --max-time 900 -o "$archive" "$url"
fi
echo "${archive_sha}  ${archive}" | sha256sum --check --status || {
    echo "provision_gentl_header: SHA-256 mismatch for $archive (expected $archive_sha)" >&2; exit 1; }
echo "provision_gentl_header: verified archive SHA-256 $archive_sha"

if [[ "$source" == emva ]]; then
    unzip -q -o "$archive" "$member" -d "$work/x"
else
    mkdir -p "$work/x"
    tar -xJf "$archive" -C "$work/x" "$member"
fi
header="$work/x/$member"
echo "${HEADER_SHA256}  ${header}" | sha256sum --check --status || {
    echo "provision_gentl_header: extracted header SHA-256 differs from $HEADER_SHA256" >&2; exit 1; }

mkdir -p "$prefix/include"
prefix="$(cd "$prefix" && pwd)"
install -m 0644 "$header" "$prefix/include/GenTL.h"
cat > "$prefix/PROVENANCE.txt" <<EOF
EMVA GenICam GenTL 1.6 C header, unmodified (GenICam License; see the EMVA package)
installed: $prefix/include/GenTL.h
header sha256: $HEADER_SHA256
source: $source
archive: $url
archive sha256: $archive_sha
member: $member
provisioned by scripts/provision_gentl_header.sh (GIGE-MEL-GENTL-001)
EOF
echo "provision_gentl_header: installed $prefix/include/GenTL.h"
echo "provision_gentl_header: configure with -DGIGE_MEL_BUILD_GENTL_PROBE=ON -DGIGE_MEL_GENTL_INCLUDE_DIR=$prefix/include"
