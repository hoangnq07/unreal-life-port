#!/bin/bash
# Unreal Life - ArkOS / R36S launcher (experimental Unity-Android loader port)
# Copy the whole "unreallife" folder next to this script:  /roms/ports/unreallife/  (or /roms2/ports/...)

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
else
  controlfolder=""
fi

if [ -d "/roms2/ports" ]; then ROMS=/roms2; else ROMS=/roms; fi
GAMEDIR="$ROMS/ports/unreallife"
cd "$GAMEDIR" || exit 1

LOG="$GAMEDIR/log.txt"
exec > "$LOG" 2>&1
set -x

# --- environment for the loader -------------------------------------------------------------------------
export UNITY_GL=sdl
export UNITY_APK="$GAMEDIR/game.apk"
export UNITY_DATA="$GAMEDIR/userdata"
export UNITY_LIBS="$GAMEDIR/libs"
export LD_LIBRARY_PATH="$GAMEDIR/lib:/usr/lib/aarch64-linux-gnu:/usr/lib:$LD_LIBRARY_PATH"
export SDL_AUDIODRIVER=alsa
# --- glibc allocator: cap the per-thread arenas ----------------------------------------------------------
# glibc gives each thread its own malloc arena (up to 8 * ncores = 32 here), and each arena can hold
# freed memory it never returns to the OS. Unity is heavily threaded, so RSS can carry hundreds of MB
# of memory that is effectively free. Capping the arenas lowers RSS, which directly reduces the memory
# pressure that pushes the heap into swap and evicts the AssetBundle page cache.
export MALLOC_ARENA_MAX=2
# Native 60 FPS VSYNC (smooth gameplay, no double-throttling)
# export UNITY_FPS=30
# Print a frame-time histogram + fps + cpu% + rss/swap every 300 frames to log.txt
export UNITY_PERF=1
# --- STALL FORENSICS -------------------------------------------------------------------------------------
# For every frame slower than 100 ms, log one line: how long the frame took, how much CPU it burned,
# and how many bytes the process read from storage during it. This tells the two causes apart:
#   read_bytes large + cpu small  -> I/O stall (reading the AssetBundle / swap off the microSD)
#   read_bytes small + cpu large  -> compute (zram compression, shader compile, GC)
# Set to 0 to silence it once the hitching is understood.
export UNITY_STALL_LOG=1
# --- AUDIO DIAGNOSTIC (B2) ------------------------------------------------------------------------------
# Keep a rolling capture of the most recent PCM buffers FMOD produces and write it to
# /tmp/pcm_dump.raw (copied next to log.txt below). The first buffers are silence, so this keeps
# the LAST ~16 buffers - the window that actually contains music - and is rewritten every second.
# Copy it back with:  cp /tmp/pcm_dump.raw /roms/ports/unreallife/   (then send me the file)
# Set to 1 only when diagnosing; it writes to the SD card and can itself add I/O load.
export UNITY_DUMP_PCM=0
# --- D-PAD FALLBACK --------------------------------------------------------------------------------------
# The D-pad is injected as KEYCODE_DPAD_* by default (same path as A/B). If the D-pad is still
# dead, set this to 1 to ALSO drive the AXIS_HAT_X/Y axes (for a Rewired definition that binds
# the D-pad to axes instead of keycodes).
export UNITY_DPAD_HAT=0
# --- PCM ENDIANNESS FALLBACK -----------------------------------------------------------------------------
# If the rolling PCM dump shows the samples are byte-swapped (every other byte looks like a low/high
# swap, e.g. quiet music reads as huge alternating values), set this to 1 to byte-swap before queueing.
export UNITY_SWAP_PCM=0
# --- AUDIO ISOLATION TEST --------------------------------------------------------------------------------
# Set to 1 to bypass FMOD and push a clean 440 Hz sine straight to SDL/ALSA for ~4 s. A clean tone
# means the SDL/ALSA path is fine and the buzzing comes from FMOD's PCM; a buzzing tone means the
# output path itself is at fault. Leave at 0 for normal play.
export UNITY_TONE=0
# --- ALSA UNDER RUN DIAGNOSTIC ---------------------------------------------------------------------------
# The ALSA error handler is silenced by default (so underruns don't spam the SD card). Set to 1 to
# keep ALSA errors ON: underruns ("underrun occurred", "XRUN") are the classic cause of crackle, so
# if the music is still buzzing this tells us whether the output is starving. Set back to 0 after.
export UNITY_ALSA_LOG=0
# Hardware ALSA buffer size in samples (default 1024 = known-good). Larger = more tolerant of
# render-thread stalls (fewer underruns -> less crackle) but more audio latency. Try 4096 or 8192
# if music still crackles.
export UNITY_ALSA_BUFFER=1024
# --- AUDIO PUMP TUNING (FMOD -> SDL) ---------------------------------------------------------------------
# The pump pulls PCM out of FMOD and queues it to SDL/ALSA. The known-good build used
# target=16384 bytes, ONE buffer per tick, every 5000us. A previous regression pulled up to 8
# buffers per 4ms tick, which forced FMOD's mixer ~10x faster than real-time; the 176 MB streaming
# BGM bundle could not be read off the microSD that fast -> starvation -> static/crackle.
# Only touch these if you are experimenting; defaults are the known-good values.
# export UNITY_AUDIO_TARGET=16384    # bytes to keep queued (~85 ms @ 48kHz stereo s16)
# export UNITY_AUDIO_REFILLS=1       # FMOD callback pulls per tick (1 = no burst)
# export UNITY_AUDIO_INTERVAL=5000   # pump tick in microseconds
mkdir -p "$UNITY_DATA"
mkdir -p "$GAMEDIR/assets/assetpack"
# The shipped game.apk has assets/bin/Data stored uncompressed and no longer carries the
# assetpack copy, so the bundles are extracted to disk and read from there (AssetPackManager
# reports a direct file). If extraction yields nothing, fall back to the APK copy so an
# older, unrepacked game.apk still works.
if [ ! -f "$GAMEDIR/assets/assetpack/bgm_bundle" ]; then
    echo "Extracting asset packs from game.apk..."
    unzip -q -o "$GAMEDIR/game.apk" "assets/assetpack/*" -d "$GAMEDIR/" 2>/dev/null || true
fi
if [ ! -f "$GAMEDIR/assets/assetpack/bgm_bundle" ]; then
    echo "WARNING: assetpack bundles are missing. Put bgm_bundle + volta_bundle in"
    echo "         $GAMEDIR/assets/assetpack/ (extract them from the original APK)."
fi

# Ensure both relative and combined asset paths resolve to the extracted files
ln -sf "$GAMEDIR/assets/assetpack" "$GAMEDIR/assets/assetpack/assetpack" 2>/dev/null || true
ln -sf "$GAMEDIR/assets/assetpack/bgm_bundle" "$GAMEDIR/assets/bgm_bundle" 2>/dev/null || true
ln -sf "$GAMEDIR/assets/assetpack/volta_bundle" "$GAMEDIR/assets/volta_bundle" 2>/dev/null || true

# --- zram: DO NOT enlarge by default (measured: enlarging makes hitching WORSE) --------------------------
# The theory was "keep the ~640 MB heap compressed in RAM instead of spilling to the microSD swapfile".
# On the device that backfired, measurably: after enlarging zram 448 -> 1024 MB, anonymous RSS fell
# 110 -> 47 MB while anon swap rose 510 -> 590 MB, page cache collapsed 186 -> 48 MB, and major faults
# per frame roughly doubled (30-95 -> 45-191). The 5-8 s hitches did NOT go away.
# Why: ArkOS's default 448 MB zram acts as a natural FLOOR - the kernel can only swap ~448 MB out, so
# the remaining ~190 MB of hot heap stays resident for free. Give it 1 GB and, at swappiness=100, it
# happily swaps the *hot* pages too (thrash), and the RAM those compressed pages occupy evicts the
# AssetBundle page cache - so Unity re-reads the bundle off the microSD, which is the real multi-second
# cost. Leave zram at the ArkOS default. Set UNITY_ZRAM_MB to experiment only.
ZRAM=/dev/zram0
ZRAM_MB=${UNITY_ZRAM_MB:-0}
if [ "$ZRAM_MB" != "0" ] && [ -b "$ZRAM" ]; then
    CUR=$(cat /sys/block/zram0/disksize 2>/dev/null || echo 0)
    WANT=$((ZRAM_MB * 1024 * 1024))
    if [ "$CUR" -lt "$WANT" ]; then
        echo "Enlarging zram0: $CUR -> $WANT bytes (${ZRAM_MB} MB)"
        # zram must be swapped off and reset before its size can change. At launch it is empty
        # (the log shows swap used = 0), so this is instant and loses nothing.
        sudo swapoff "$ZRAM" 2>/dev/null || true
        echo 1 | sudo tee /sys/block/zram0/reset >/dev/null 2>&1 || true
        echo "$WANT" | sudo tee /sys/block/zram0/disksize >/dev/null 2>&1 || true
        sudo mkswap "$ZRAM" >/dev/null 2>&1 || true
        # zram must outrank the microSD swapfile. `-p` is not in every busybox, so fall back to a
        # plain swapon and give the swapfile the lower priority below instead.
        sudo swapon -p 100 "$ZRAM" 2>/dev/null || sudo swapon "$ZRAM" 2>/dev/null || true
        if grep -q "$ZRAM" /proc/swaps 2>/dev/null; then
            echo "zram0 resized to ${ZRAM_MB} MB and enabled"
        else
            # Resize failed: restore the original size so we are no worse off than before.
            echo "WARNING: zram resize failed; restoring original size"
            echo 1 | sudo tee /sys/block/zram0/reset >/dev/null 2>&1 || true
            echo "$CUR" | sudo tee /sys/block/zram0/disksize >/dev/null 2>&1 || true
            sudo mkswap "$ZRAM" >/dev/null 2>&1 || true
            sudo swapon -p 5 "$ZRAM" 2>/dev/null || sudo swapon "$ZRAM" 2>/dev/null || true
        fi
    else
        echo "zram0 already ${CUR} bytes (>= ${ZRAM_MB} MB), leaving as is"
    fi
    cat /proc/swaps
fi

# --- Setup 1024MB swapfile on SD card so large AssetBundles don't trigger the Linux OOM-killer ---
if [ ! -f "$GAMEDIR/swapfile" ]; then
    echo "Creating 1024MB swapfile on SD card for Unreal Life..."
    fallocate -l 1024M "$GAMEDIR/swapfile" 2>/dev/null || dd if=/dev/zero of="$GAMEDIR/swapfile" bs=1M count=1024
    chmod 600 "$GAMEDIR/swapfile"
    mkswap "$GAMEDIR/swapfile"
fi
if [ -f "$GAMEDIR/swapfile" ]; then
    # Low priority: the kernel only reaches for the microSD swapfile once zram is full, so the
    # slow I/O path is a last resort rather than the normal home for the game's heap.
    swapon -p 1 "$GAMEDIR/swapfile" 2>/dev/null || sudo swapon -p 1 "$GAMEDIR/swapfile" 2>/dev/null || \
        swapon "$GAMEDIR/swapfile" 2>/dev/null || sudo swapon "$GAMEDIR/swapfile" 2>/dev/null || true
fi
# swappiness MUST stay high: the game has ~600 MB of anonymous heap but the device has
# only 897 MB RAM, so the kernel has to move cold heap pages somewhere. At swappiness=100
# it compresses them into zram (fast, costs CPU -> the console runs warm) and keeps the
# asset page-cache resident. At swappiness=1 the kernel instead pins the heap in RAM and
# evicts the asset cache, so Unity re-reads assets off the microSD every frame -> 230
# major faults/frame, ~2 fps, and the console runs cool because it is idle waiting on I/O.
# (This was regressed from 100 -> 1 during the A/B button fix; 100 is the known-good value.)
sudo sh -c 'echo 100 > /proc/sys/vm/swappiness' 2>/dev/null || true
sudo sh -c 'echo 1 > /proc/sys/vm/overcommit_memory' 2>/dev/null || true

# --- diagnostics (the 1 GB R36S is tight on RAM: please send me log.txt if the game does not start) ----
free -m
cat /proc/swaps
# zram state: the compression algorithm and ratio decide how expensive a swap-out is. A slow algorithm
# (lzo/lzo-rle) burns CPU in the reclaim path and shows up as long frames with high stime, no disk I/O.
echo "zram0 algo: $(cat /sys/block/zram0/comp_algorithm 2>/dev/null)"
echo "zram0 mm_stat: $(cat /sys/block/zram0/mm_stat 2>/dev/null)"
uname -a
ls -la "$GAMEDIR" "$GAMEDIR/libs"

# make sure the console doesn't draw over us
printf "\033c" > /dev/tty1
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true

# Set CPU governor to performance for faster loading and rock-solid framerate
ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "ondemand")
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null || true
# The GPU has its own governor on RK3326 - without this it stays on simple_ondemand and
# downclocks between frames, which shows up as jitter at 60 Hz.
for g in /sys/class/devfreq/*gpu*/governor /sys/class/devfreq/*mali*/governor /sys/class/devfreq/dmc/governor; do
    [ -e "$g" ] && echo performance | sudo tee "$g" >/dev/null 2>&1
done
cat /sys/class/devfreq/*gpu*/cur_freq 2>/dev/null | sed 's/^/gpu cur_freq: /'
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null | sed 's/^/cpu cur_freq: /'

chmod +x ./unityhost
./unityhost "$UNITY_LIBS"
RC=$?
echo "unityhost exited with $RC"

# B2 diagnostic: preserve the dumped PCM next to the log so it can be copied off the SD card.
if [ -f /tmp/pcm_dump.raw ]; then
    cp -f /tmp/pcm_dump.raw "$GAMEDIR/pcm_dump.raw" 2>/dev/null && \
        echo "PCM dump saved to $GAMEDIR/pcm_dump.raw ($(wc -c < "$GAMEDIR/pcm_dump.raw") bytes)"
fi

echo "=== Dmesg OOM check ==="
dmesg | tail -n 40
free -m
swapoff "$GAMEDIR/swapfile" 2>/dev/null || sudo swapoff "$GAMEDIR/swapfile" 2>/dev/null || true
echo "$ORIG_GOV" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null || true
printf "\033c" > /dev/tty1
exit $RC
