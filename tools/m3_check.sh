#!/bin/bash
# M3 sanity: package, then (a) null-GL run under qemu, (b) UNITY_GL=sdl must fail cleanly without SDL.
cd "$(dirname "$0")/.."
bash tools/package.sh > build/package.out 2>&1 || { tail -20 build/package.out; exit 1; }
tail -3 build/package.out
ls dist dist/unreallife
cd dist/unreallife
echo "== sdl failure path =="
UNITY_GL=sdl UNITY_APK=game.apk UNITY_DATA=/tmp/ud timeout 150 qemu-aarch64 -L /usr/aarch64-linux-gnu ./unityhost libs > ../../build/sdl_fail.log 2>&1
echo "exit=$?"
tail -4 ../../build/sdl_fail.log
cd ../..
echo "== null GL run =="
TIMEOUT=240 UNITY_FRAMES=60 bash tools/m2_run.sh > build/m3_regress.out 2>&1
grep -E 'frame limit|nativeRender #|SIGSEGV|SIGBUS' build/m2.log | tail -5
