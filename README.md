# Unreal Life -> R36S (ArkOS) port: progress

Goal: run the Android Unity 2018.4.15f1 IL2CPP arm64 build of *Unreal Life* natively on ArkOS (glibc 2.30, aarch64)
with a custom ELF loader + bionic shims + fake JNI. See `feasibility_analysis.md` (artifact) for the full analysis.

## Layout
- `src/loader.c`   ELF loader for bionic aarch64 .so (RELA only, DT_HASH, init_array, unresolved-import stubs)
- `src/shim.c`     bionic->glibc ABI shims (pthread, sem, signals, stdio `__sF`, ctype, dl*, setjmp, ...)
- `src/shim_asm.S` setjmp/longjmp with a 256-byte bionic-compatible buffer
- `src/fakejni.c`  fake JavaVM/JNIEnv (233 slots; unimplemented ones log their name)
- `src/main.c`     driver
- `libs/`          arm64-v8a libs extracted from the APK (not redistributable)
- `tools/`         `gen_jni_slots.py`, `run_qemu.sh`, `m1_check.sh`

## Build / run (WSL2 Ubuntu)
```
make                      # zig cc -target aarch64-linux-gnu.2.30  (needs /opt/zigenv with `pip install ziglang`)
tools/run_qemu.sh         # runs under qemu-aarch64
```

## Milestones
| | | status |
|--|--|--|
| M0 | cross toolchain targeting glibc 2.30, runs under qemu | done |
| M1 | load libmain/libunity/libil2cpp, run all constructors, `JNI_OnLoad`, `NativeLoader.load` | **done** (489 + 44 constructors OK) |
| M2 | fake Java layer: `UnityPlayer.initJni`, surface, `nativeRender` loop (headless, null GL) | **done** (boots scenes + C# under qemu) |
| M3 | EGL/GLES via SDL2 (`UNITY_GL=sdl`) on real R36S, measure RAM/FPS -> go/no-go | **code written, needs device test** |
| M4 | audio (FMOD -> SDL), gamepad input | |
| M5 | video, Firebase stubs, save data, RAM tuning, PortMaster packaging | |

## Packaging for the R36S
```
bash tools/package.sh     # -> dist/"Unreal Life.sh" + dist/unreallife/{unityhost,libs,game.apk,userdata}
```
Copy `dist/*` into `roms/ports/` on the SD card. Run it from the Ports menu. Send back `unreallife/log.txt`.
The APK and its libs are not redistributable - always bring your own copy.
Env vars: `UNITY_GL=sdl|null`, `UNITY_APK`, `UNITY_DATA`, `UNITY_LIBS`, `UNITY_FRAMES`, `UNITY_SAMPLE`, `UNITY_THREADLOG`.

## Findings worth remembering
- **"Smooth but hitches for seconds" - enlarging zram made it WORSE (measured), reverted.** The first
  theory was "keep the ~640 MB heap compressed in RAM instead of spilling to the microSD swapfile", so the
  launcher grew `zram0` 448 -> 1024 MB at priority 100 and dropped the swapfile to priority 1. On-device the
  hitches did *not* go away and the numbers got worse: anonymous RSS fell 110 -> 47 MB while anon **swap rose
  510 -> 590 MB**, page cache collapsed **186 -> 48 MB**, and major faults/frame roughly doubled (30-95 ->
  45-191). ArkOS's default 448 MB zram is a *feature*: it is a natural floor, so the kernel can only swap
  ~448 MB out and the remaining ~190 MB of hot heap stays resident for free. Give it 1 GB and, at
  swappiness=100, the kernel swaps the *hot* pages too (thrash) and the RAM the compressed pages occupy
  evicts the AssetBundle page cache - so Unity re-reads the bundle off the microSD, which is the real
  multi-second cost. **Leave zram at the ArkOS default** (`UNITY_ZRAM_MB=0`). The `zram`/priority code is
  kept behind `UNITY_ZRAM_MB` for experiments only.
- **Hitches are events, not a constant tax, and happen when the game loads new assets (choice menus).**
  The perf log separates the two states cleanly: smooth blocks run **major faults ~0.8/frame at cpu 190-236%**;
  hitchy blocks show **30-95 major faults/frame at cpu 130-165%** - cpu *drops*, i.e. the CPU is idle waiting
  on storage, not busy. `UNITY_STALL_LOG=1` now logs every frame >100 ms with its cpu time and the
  `read_bytes` delta from `/proc/self/io`: big read_bytes + small cpu = I/O stall (SD card); small read_bytes
  + big cpu = compute (zram compression, shader compile, GC). Also set `MALLOC_ARENA_MAX=2` to stop glibc
  from holding dozens of per-thread arenas' worth of freed memory (Unity is heavily threaded; each arena can
  keep memory it never returns to the OS), which lowers RSS and so the memory pressure that feeds the swapping.
- **Do not set `vm.swappiness=1`.** The device has 897 MB RAM but the game holds ~600 MB of anonymous heap,
  so the kernel *must* move cold pages somewhere. At `swappiness=100` they go to zram (compressed, fast,
  burns CPU -> console runs warm) while the asset page-cache stays resident. At `swappiness=1` the kernel
  pins the heap and evicts the asset cache instead, so Unity re-reads assets off the microSD every frame:
  230 major faults/frame, ~2 fps, and the console runs *cool* because it is idle waiting on I/O.
  The launcher regressed this from 100 -> 1 during the A/B button fix and it cost days of "why is it slower
  but cooler" debugging. Symptom "smoother build runs hot, slower build runs cool" == swap policy.
- The emulated JNI has **no GC**: `jnew`/`jarray`/`get_field` never free. Any accessor that allocates a fresh
  object per call (device-id arrays, motion ranges, `GetFieldID`) leaks one allocation per poll. Cache them.
- **D-pad must be sent as `KEYCODE_DPAD_*` KeyEvents, not HAT axes.** On RK3326 the physical D-pad is
  `ABS_HAT0X/Y`, and the ArkOS controller db binds `dpup:b8..dpright:b11` as *buttons* - but SDL only maps a
  hat to `dpup` when the mapping says `dpup:h0.1`, so the game-controller layer emits **nothing** for the
  D-pad and the raw `SDL_JOYHATMOTION` is the only signal. More importantly, a real Android gamepad
  delivers the D-pad as `KEYCODE_DPAD_*` KeyEvents (source `SOURCE_DPAD`), which Unity turns into joystick
  buttons - that is what Rewired reads. The old code injected only `AXIS_HAT_X/Y` MotionEvents, which
  Rewired's gamepad definition ignores, so the D-pad did nothing while A/B (plain KeyEvents) worked.
  Fix: `handle_dpad()` now calls `android_inject_key()` (the same proven path as A/B), with the HAT axes
  available behind `UNITY_DPAD_HAT=1` if a build turns out to bind the D-pad to axes instead.
- **Audio "static" ROOT CAUSE: an over-aggressive pump forced FMOD to mix ~10x faster than real-time.**
  `bq_Enqueue` was silent, so "no Enqueue in the log" was a blind spot; it now logs, and the log proved FMOD
  *was* producing real little-endian 48 kHz stereo s16 PCM. The fault was the pump loop: a regression changed
  it to `while (queued < 32768 && refills < 8)` with a 4 ms tick, i.e. up to 8 buffers (170 ms of audio) pulled
  per tick. That makes FMOD's DSP mixer render ~10x real-time, and the 176 MB streaming `bgm_bundle` cannot be
  read off the microSD that fast -> sample starvation -> static/pops. The known-good build used a *gentle* pump
  (`if (queued < 16384) cb()` once per 5 ms) and `desired.samples = 1024`. Both were regressed together
  (samples 1024->2048, pump -> burst); the fix reverts both. Tunables: `UNITY_AUDIO_TARGET`,
  `UNITY_AUDIO_REFILLS`, `UNITY_AUDIO_INTERVAL` (defaults 16384 / 1 / 5000) and `UNITY_ALSA_BUFFER` (default
  1024). Lesson: **a callback-driven audio backend must be pulled at the *playback* rate, never ahead of it** -
  buffering more "to avoid underruns" is exactly what causes them when the source streams from slow storage.
- **FMOD reports `SLAndroidDataFormat_PCM_EX` (type=2) `ch=2 rate=48000 bits=16 mask=0x3 endian=2`.** The
  `endian=2` field looked like big-endian but the OpenSL enum is `SL_BYTEORDER_BIGENDIAN=1,
  SL_BYTEORDER_LITTLEENDIAN=2`, so `endian=2` = little-endian = correct. The data decodes as little-endian.
  `UNITY_SWAP_PCM=1` remains as a valve but is not needed. `UNITY_DUMP_PCM=1` keeps a rolling capture of the
  last 16 buffers (the game exits via `_exit()` and skips cleanup, so it is rewritten periodically) for offline
  analysis; both are off by default because the dump writes to the SD card.
- bionic `pthread_mutex_t/cond_t` are only 4-byte aligned: glibc's 64-bit atomics on them raise **SIGBUS**. Guest objects
  therefore hold a lazily created pointer to a real glibc object (32-bit atomics only).
- glibc < 2.33 does not export `stat/fstat/lstat` nor `pthread_atfork` -> shimmed via raw syscalls / no-op.
- Android reads its stack canary from `[tpidr_el0 + 40]`; `main.c` forces a 64-byte aligned TLS block so that slot is
  inert padding on glibc/aarch64.
- Unity's `libmain` appends `/libunity.so` itself: `NativeLoader.load()` takes the *directory*.
