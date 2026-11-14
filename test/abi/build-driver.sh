#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build the driver with debug info against a server staged by
# build-server.sh.
#
#   build-driver.sh <server-workdir> <driver-builddir> [<driver-srcdir>]
#
# Only the xorg-server package is redirected to the staged root (pkg-config's
# per-package PKG_CONFIG_XORG_SERVER_PREFIX override); every other
# dependency keeps coming from the system.

set -eu

[ $# -ge 2 ] && [ $# -le 3 ] || {
	echo "usage: $0 <server-workdir> <driver-builddir> [<driver-srcdir>]" >&2
	exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$1/root" && pwd)
builddir=$2
srcdir=${3:-$here/../..}

pcdirs=
for d in "$root"/usr/lib*/pkgconfig "$root"/usr/lib/*/pkgconfig; do
	[ -f "$d/xorg-server.pc" ] && pcdirs=${pcdirs:+$pcdirs:}$d
done
if [ -z "$pcdirs" ]; then
	echo "build-driver: no xorg-server.pc under $root" >&2
	exit 1
fi

PKG_CONFIG_PATH=$pcdirs${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
PKG_CONFIG_XORG_SERVER_PREFIX=$root/usr
export PKG_CONFIG_PATH PKG_CONFIG_XORG_SERVER_PREFIX

echo "build-driver: xorg-server $(pkg-config --modversion xorg-server)" \
	"sdkdir $(pkg-config --variable=sdkdir xorg-server)"

rm -rf "$builddir"
meson setup "$builddir" "$srcdir" -Dbuildtype=debug
ninja -C "$builddir"
