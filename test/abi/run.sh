#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Run one of the xserver SDK's driver ABI tools on this driver.
#
#   ABI_WORK=<dir> [ABI_SERVERS=...] test/abi/run.sh <tool> [<args>]
#
# <tool> is e.g. run-matrix.sh (symbols across server releases) or
# gate-driver.sh (layout gate for the working tree against BEFORE_REF).
# The tools come from the xorg-server SDK found by pkg-config (variable
# abi_tools_dir); their README.md there describes them.

set -eu

[ $# -ge 1 ] || { echo "usage: $0 <tool> [<args>]" >&2; exit 2; }

tools=$(pkg-config --variable=abi_tools_dir xorg-server 2>/dev/null || true)
if [ -z "$tools" ] || [ ! -d "$tools" ]; then
	echo "run.sh: the xorg-server SDK found by pkg-config has no ABI tools" \
		"(abi_tools_dir); it needs an XLibre xserver with abi/ installed" >&2
	exit 2
fi

tool=$1
shift
ABI_MODULE=src/amdgpu_drv.so
ABI_DRIVER_SRC=$(cd "$(dirname "$0")/../.." && pwd)
export ABI_MODULE ABI_DRIVER_SRC
exec "$tools/$tool" "$@"
