#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Check that every server symbol the driver needs is exported by a server.
#
#   check-symbols.sh <amdgpu_drv.so> <server-workdir>
#
# The driver's strong undefined symbols, minus those its own libraries and
# the server's libraries provide, must each be defined by the staged Xorg
# or a module under the staged module directory. Exit 1 lists the rest.

set -eu

[ $# -eq 2 ] || {
	echo "usage: $0 <amdgpu_drv.so> <server-workdir>" >&2
	exit 2
}

drv=$1
root=$(cd "$2/root" && pwd)
xorg=$root/usr/bin/Xorg
[ -f "$xorg" ] || { echo "check-symbols: no $xorg" >&2; exit 2; }

pcdirs=
for d in "$root"/usr/lib*/pkgconfig "$root"/usr/lib/*/pkgconfig; do
	[ -f "$d/xorg-server.pc" ] && pcdirs=${pcdirs:+$pcdirs:}$d
done
moduledir=$(PKG_CONFIG_PATH=$pcdirs PKG_CONFIG_XORG_SERVER_PREFIX=$root/usr \
	pkg-config --variable=moduledir xorg-server)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

defined() {
	nm -D --defined-only "$1" 2>/dev/null | tr -s ' ' | cut -d' ' -f3 | cut -d@ -f1
}

nm -D --undefined-only "$drv" | grep ' U ' | tr -s ' ' | cut -d' ' -f3 |
	cut -d@ -f1 | sort -u >"$tmp/undef"

{ ldd "$drv"; ldd "$xorg"; } | grep '=> /' | tr -s ' \t' ' ' | cut -d' ' -f4 |
	sort -u | while read -r lib; do defined "$lib"; done | sort -u >"$tmp/libs"

{
	defined "$xorg"
	find "$moduledir" -name '*.so' | while read -r mod; do defined "$mod"; done
} | sort -u >"$tmp/server"

comm -23 "$tmp/undef" "$tmp/libs" | comm -23 - "$tmp/server" >"$tmp/missing"

if [ -s "$tmp/missing" ]; then
	echo "check-symbols: FAIL: $drv needs symbols $root does not export:"
	sed 's/^/  /' "$tmp/missing"
	exit 1
fi
echo "check-symbols: PASS ($(comm -12 "$tmp/undef" "$tmp/server" | wc -l)" \
	"server symbols resolved)"
