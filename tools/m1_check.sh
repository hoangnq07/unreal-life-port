#!/bin/bash
cd "$(dirname "$0")/.." || exit 1
sed -i 's/\r$//' tools/*.sh
tools/run_qemu.sh > /tmp/m1.log 2>&1
echo "exit code: $?"
grep -E 'il2cpp|FATAL|M1 done|initialized|unresolved=' /tmp/m1.log
echo "--- non-graphics unresolved imports:"
grep UNRESOLVED /tmp/m1.log | grep -v -E "'(egl|gl|A[A-Z])" | head -20
echo "--- tail:"
tail -5 /tmp/m1.log
