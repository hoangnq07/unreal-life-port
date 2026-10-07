#!/usr/bin/env python3
"""
Repack the Unreal Life APK so Unity can read its data without inflating it.

Why: libunity.so imports `inflate` and opens game.apk directly (it does NOT use
AAssetManager). assets/bin/Data is 903 MB uncompressed but only ~97 MB in the APK,
so every asset read costs a zlib inflate on the Cortex-A35 *and* the inflated bytes
land in anonymous heap -> swap -> per-frame SD card page faults.

Storing assets/bin/Data/** uncompressed (method 0) lets Unity read/mmap straight from
the file: no inflate, and the pages come from the page cache so they are evictable.

Also drops:
  lib/armeabi-v7a/**   - 32-bit libs, never loaded on aarch64 (~27 MB in the APK)
  assets/assetpack/**  - shipped separately in dist/unreallife/assets/assetpack and
                         read from disk (AssetLocation.path() -> direct file), so the
                         copy inside the APK is dead weight (~176 MB)

Everything else is copied byte-for-byte with its original compression method.
"""
import os
import shutil
import sys
import zipfile

# stored uncompressed so the engine can mmap instead of inflate
STORE_PREFIXES = ("assets/bin/Data/",)
# dropped entirely
DROP_PREFIXES = ("lib/armeabi-v7a/", "assets/assetpack/")

# already-compressed payloads: re-deflating them wastes time for ~0 gain
INCOMPRESSIBLE_EXT = (".so", ".png", ".jpg", ".jpeg", ".ogg", ".mp3", ".wav", ".aac",
                      ".m4a", ".webp", ".ttf", ".otf", ".dex", ".unity3d", ".bundle")


def decide(name: str, orig_method: int) -> int | None:
    if name.startswith(DROP_PREFIXES):
        return None
    if name.startswith(STORE_PREFIXES):
        return zipfile.ZIP_STORED
    return orig_method


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    no_store = "--no-store" in flags
    if len(args) < 2:
        print(f"usage: {sys.argv[0]} [--no-store] <in.apk> <out.apk>", file=sys.stderr)
        print("  --no-store  keep assets/bin/Data deflated (only strip dead weight)", file=sys.stderr)
        return 2
    src, dst = args[0], args[1]
    if not os.path.isfile(src):
        print(f"no such file: {src}", file=sys.stderr)
        return 2
    if os.path.abspath(src) == os.path.abspath(dst):
        print("refusing to overwrite the input in place", file=sys.stderr)
        return 2

    tmp = dst + ".part"
    if os.path.exists(tmp):
        os.remove(tmp)

    zin = zipfile.ZipFile(src, "r")
    # allowZip64: the archive stays well under 4 GB, but a >4 GB total is possible
    # once bin/Data is stored, so keep the door open.
    zout = zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, allowZip64=True)

    kept = dropped = stored = 0
    bytes_out = bytes_in = 0
    try:
        for info in zin.infolist():
            name = info.filename
            method = decide(name, info.compress_type)
            if method is None:
                dropped += 1
                continue
            if no_store and name.startswith(STORE_PREFIXES):
                method = info.compress_type

            out = zipfile.ZipInfo(name, date_time=info.date_time)
            out.compress_type = method
            out.external_attr = info.external_attr
            out.internal_attr = info.internal_attr
            out.create_system = info.create_system
            out.comment = info.comment
            # Unity's zip reader relies on the local header CRC/sizes; let zipfile fill them.
            if name.endswith("/") or info.is_dir():
                out.compress_type = zipfile.ZIP_STORED
                zout.writestr(out, b"")
                kept += 1
                continue

            data = zin.read(info)
            zout.writestr(out, data, compress_type=method, compresslevel=6)
            kept += 1
            bytes_in += len(data)
            if method == zipfile.ZIP_STORED:
                stored += 1
    finally:
        zin.close()
        zout.close()

    os.replace(tmp, dst)
    bytes_out = os.path.getsize(dst)
    print(f"  kept    {kept:5d} entries")
    print(f"  dropped {dropped:5d} entries")
    print(f"  stored  {stored:5d} entries uncompressed (assets/bin/Data)")
    print(f"  raw     {bytes_in / 1048576:8.1f} MB of payload rewritten")
    print(f"  in      {os.path.getsize(src) / 1048576:8.1f} MB")
    print(f"  out     {bytes_out / 1048576:8.1f} MB  -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
