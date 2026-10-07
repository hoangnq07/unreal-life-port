#!/bin/bash
# M2: build and run the host under qemu with the real APK. Log -> build/m2.log (project dir; /tmp is not persistent)
cd "$(dirname "$0")/.." || exit 1
make -j8 2>&1 | grep -E 'error|warning: (implicit|incompatible)|undefined' | head -40
export UNITY_APK="${UNITY_APK:-/mnt/d/Code/Unreal Life/APK-UnrealLife-AowVN-20.apk}"
export UNITY_DATA="${UNITY_DATA:-/tmp/unity_data}"
export UNITY_LIBS=libs
export UNITY_FRAMES="${UNITY_FRAMES:-5}"
mkdir -p build
timeout "${TIMEOUT:-240}" qemu-aarch64 -L /usr/aarch64-linux-gnu ./build/unityhost libs "$@" > build/m2.log 2>&1
echo "exit=$?"
wc -l build/m2.log
grep -E 'not emulated' build/m2.log | sed 's/.*ID(//; s/)   <-- not emulated//' | sort -u > build/missing.txt
grep -v -E 'GetMethodID|GetFieldID|GetStaticMethodID|GetStaticFieldID' build/m2.log | tail -${TAIL:-60}
echo "--- missing (not emulated) ---"
cat build/missing.txt
