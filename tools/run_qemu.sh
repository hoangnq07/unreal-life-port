#!/bin/bash
# Build and run the host under qemu-aarch64. Usage: tools/run_qemu.sh [extra args to unityhost]
cd "$(dirname "$0")/.." || exit 1
make -j8 2>&1 | grep -E 'error|Error'
exec timeout "${TIMEOUT:-300}" qemu-aarch64 -L /usr/aarch64-linux-gnu ./build/unityhost libs "$@"
