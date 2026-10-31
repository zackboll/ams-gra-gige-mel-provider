#!/usr/bin/env bash
# Explicit, opt-in provisioning of the qualified Aravis SDK baseline for the
# optional GIGE_MEL_BUILD_ARAVIS integration. Normal CMake configuration and
# builds never call this script and never download anything.
#
# Usage:
#   scripts/provision_aravis.sh PREFIX [--archive PATH]
#
# Downloads (or uses --archive) the official Aravis 0.8.36 release archive,
# verifies its SHA-256, and builds only the shared library and headers into
# PREFIX with meson/ninja. Viewer, GStreamer plugin, USB3 Vision, packet
# socket, introspection, documentation and upstream tests are disabled, and
# the upstream command-line programs are not built.
# Afterwards:
#   export PKG_CONFIG_PATH="PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
#
# Build prerequisites (not installed by this script): a C compiler, meson
# (>= 0.57), ninja, pkg-config and the GLib 2 (>= 2.52, gio/gobject), libxml2
# and zlib development files.
set -euo pipefail

readonly ARAVIS_VERSION="0.8.36"
readonly ARAVIS_URL="https://github.com/AravisProject/aravis/releases/download/${ARAVIS_VERSION}/aravis-${ARAVIS_VERSION}.tar.xz"
readonly ARAVIS_SHA256="246deaa0042a387ff1bc00332d0fb80537ce14abde2c28d1a54c91f17adc51bf"
# Release source commit (tag 0.8.36), recorded for provenance only.
readonly ARAVIS_COMMIT="97ff5e330919d7a44b871df4eda669feeafdf342"

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

if [[ $# -lt 1 || "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    [[ $# -lt 1 ]] && exit 2
    exit 0
fi

prefix="$1"
shift
archive=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --archive) archive="${2:?--archive needs a path}"; shift 2 ;;
        *) echo "provision_aravis: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

for tool in meson ninja pkg-config sha256sum tar; do
    command -v "$tool" >/dev/null || { echo "provision_aravis: '$tool' is required" >&2; exit 1; }
done

mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/aravis-provision.XXXXXX")"
trap 'rm -rf "$work"' EXIT

if [[ -z "$archive" ]]; then
    command -v curl >/dev/null || { echo "provision_aravis: 'curl' is required to download" >&2; exit 1; }
    archive="$work/aravis-${ARAVIS_VERSION}.tar.xz"
    echo "provision_aravis: downloading $ARAVIS_URL"
    curl --fail --location --silent --show-error --retry 3 --max-time 300 -o "$archive" "$ARAVIS_URL"
fi

echo "${ARAVIS_SHA256}  ${archive}" | sha256sum --check --status || {
    echo "provision_aravis: SHA-256 mismatch for $archive (expected $ARAVIS_SHA256)" >&2
    exit 1
}
echo "provision_aravis: verified SHA-256 $ARAVIS_SHA256"

tar -xJf "$archive" -C "$work"
src="$work/aravis-${ARAVIS_VERSION}"
grep -q "version: '${ARAVIS_VERSION}'" "$src/meson.build" || {
    echo "provision_aravis: extracted source does not declare version $ARAVIS_VERSION" >&2
    exit 1
}

meson setup "$work/build" "$src" \
    --prefix "$prefix" --libdir lib --buildtype release \
    -Dviewer=disabled -Dgst-plugin=disabled -Dusb=disabled -Dpacket-socket=disabled \
    -Dintrospection=disabled -Ddocumentation=disabled -Dtests=false
# Build only the shared library target (and its generated headers). The
# upstream arv-tool/arv-test/arv-camera-test/arv-fake-gv-camera programs are
# neither built nor installed.
meson compile -C "$work/build" -j "${ARAVIS_JOBS:-2}" aravis-0.8
meson introspect "$work/build" --install-plan > "$work/install-plan.json"
python3 - "$work/install-plan.json" "$prefix" <<'PY'
import json, os, shutil, sys

plan = json.load(open(sys.argv[1]))
prefix = sys.argv[2]
# Placeholders for the options passed to meson setup above.
places = {"{prefix}": prefix, "{libdir_shared}": prefix + "/lib", "{libdir}": prefix + "/lib",
          "{includedir}": prefix + "/include"}
installed = 0
for section, entries in plan.items():
    if section not in ("targets", "headers", "configure", "data"):
        raise SystemExit(f"unexpected install-plan section {section}")
    for source, info in entries.items():
        name = os.path.basename(source)
        destination = info["destination"]
        if destination.startswith("{bindir}"):
            continue  # upstream tools are deliberately not provisioned
        if destination.startswith("{"):
            placeholder = destination.split("/", 1)[0]
            if placeholder not in places:
                raise SystemExit(f"unexpected install destination {destination}")
            destination = places[placeholder] + destination[len(placeholder):]
        elif destination.startswith("include/aravis-0.8/"):
            destination = os.path.join(prefix, destination)  # prefix-relative headers
        else:
            raise SystemExit(f"unexpected install destination {destination}")
        if not os.path.exists(source):
            raise SystemExit(f"install-plan source was not built: {source}")
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copy2(source, destination)
        installed += 1
        if section == "targets" and name.startswith("libaravis-0.8.so."):
            directory = os.path.dirname(destination)
            for link in ("libaravis-0.8.so.0", "libaravis-0.8.so"):
                path = os.path.join(directory, link)
                if os.path.lexists(path):
                    os.remove(path)
                os.symlink(name, path)
print(f"provision_aravis: installed {installed} files (library, headers, pkg-config)")
PY

PKG_CONFIG_PATH="$prefix/lib/pkgconfig" pkg-config --exact-version="$ARAVIS_VERSION" aravis-0.8 || {
    echo "provision_aravis: installed pkg-config module is not aravis-0.8 $ARAVIS_VERSION" >&2
    exit 1
}
cat > "$prefix/PROVENANCE.txt" <<EOF
Aravis ${ARAVIS_VERSION} provisioned by scripts/provision_aravis.sh
archive: ${ARAVIS_URL}
sha256: ${ARAVIS_SHA256}
release commit: ${ARAVIS_COMMIT}
meson options: --buildtype release -Dviewer=disabled -Dgst-plugin=disabled -Dusb=disabled
  -Dpacket-socket=disabled -Dintrospection=disabled -Ddocumentation=disabled -Dtests=false
compiler: $(cc --version | head -n 1)
instrumentation: none (the SDK is not built with sanitizers)
EOF
echo "provision_aravis: installed Aravis ${ARAVIS_VERSION} into $prefix"
echo "provision_aravis: export PKG_CONFIG_PATH=\"$prefix/lib/pkgconfig\${PKG_CONFIG_PATH:+:\$PKG_CONFIG_PATH}\""
