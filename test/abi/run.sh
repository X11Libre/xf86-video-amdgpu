#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Driver ABI check across the release branch tips: build the driver against
# each server in ABI_SERVERS and check that every server symbol the driver
# needs exists in every one of them (a driver built against one release
# must load into the others).
#
#   ABI_WORK=<dir> [ABI_SERVERS=...] run.sh
#
# Layout changes are checked per change by gate-driver.sh.

set -eu
. "$(dirname "$0")/common.sh"

repo=$(cd "$abi_dir/../.." && pwd)
fail=0

for s in $ABI_SERVERS; do
	wd=$(abi_server "$s")
	echo "run: $s -> $wd"
	"$abi_dir/build-driver.sh" "$wd" "$wd/driver" "$repo" >/dev/null
done

for built in $ABI_SERVERS; do
	for target in $ABI_SERVERS; do
		bwd=$(abi_workdir "$built")
		twd=$(abi_workdir "$target")
		out=$bwd/symbols-$(echo "$target" | tr '/' '-').txt
		if "$abi_dir/check-symbols.sh" "$bwd/driver/src/amdgpu_drv.so" "$twd" >"$out"; then
			echo "run: symbols PASS built for $built, loaded into $target"
		else
			echo "run: symbols FAIL built for $built, loaded into $target (see $out)"
			fail=1
		fi
	done
done
exit $fail
