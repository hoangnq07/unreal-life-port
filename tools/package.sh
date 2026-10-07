#!/bin/bash
# Build the aarch64 binary and assemble dist/ ready to copy to the R36S SD card (ports/ folder).
# Needs the APK in the project root: ../APK-UnrealLife-AowVN-20.apk  and extracted libs in ./libs (tools/m1_check.sh does that).
set -e
cd "$(dirname "$0")/.."
make
D=dist/unreallife
SRC_APK="../APK-UnrealLife-AowVN-20.apk"
# The shipped game.apk is the *repacked* one: armeabi-v7a and the duplicate assetpack copy
# are stripped (330 MB -> 129 MB). NOTE: storing assets/bin/Data uncompressed was tried and
# measured *slower* on the device (fps 2.5 -> 1.9), so bin/Data stays deflated. Rebuild with:
#   python3 tools/repack_apk.py --no-store "$SRC_APK" scratch/game-lean.apk
REPACKED="scratch/game-lean.apk"
rm -rf dist
mkdir -p "$D/libs" "$D/userdata"
cp build/unityhost "$D/"
cp "package/Unreal Life.sh" dist/
cp libs/*.so "$D/libs/"
if [ -f "$REPACKED" ]; then
    cp "$REPACKED" "$D/game.apk"
else
    echo "WARNING: $REPACKED missing - shipping the original APK (330 MB, includes armeabi-v7a)" >&2
    cp "$SRC_APK" "$D/game.apk"
fi
# assetpack is read from disk (AssetPackManager -> direct file), not from the APK, so it
# is extracted here. The original APK is used because the repacked one drops the copy.
mkdir -p "$D/assets"
unzip -q -o "$SRC_APK" "assets/assetpack/*" -d "$D/" 2>/dev/null || true
sed -i 's/\r$//' dist/"Unreal Life.sh"
du -sh dist
echo "Copy dist/* to the SD card: roms/ports/  (=> roms/ports/Unreal Life.sh + roms/ports/unreallife/)"
