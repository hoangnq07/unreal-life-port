import zipfile

with zipfile.ZipFile("../APK-UnrealLife-AowVN-20.apk") as z:
    names = z.namelist()
    print("Total files:", len(names))
    total_uncompressed = sum(info.file_size for info in z.infolist())
    total_compressed = sum(info.compress_size for info in z.infolist())
    print(f"Compressed: {total_compressed / 1024 / 1024:.1f} MB, Uncompressed: {total_uncompressed / 1024 / 1024:.1f} MB")
    
    dirs = {}
    for info in z.infolist():
        d = info.filename.split('/')[0]
        dirs[d] = dirs.get(d, 0) + info.file_size
    for d, s in sorted(dirs.items(), key=lambda x: -x[1]):
        print(f"  {d}: {s / 1024 / 1024:.1f} MB")

    print("\nAssets top-level:")
    assets_dirs = {}
    for info in z.infolist():
        if info.filename.startswith("assets/"):
            parts = info.filename.split('/')
            sub = parts[1] if len(parts) > 1 else "root"
            assets_dirs[sub] = assets_dirs.get(sub, 0) + info.file_size
    for d, s in sorted(assets_dirs.items(), key=lambda x: -x[1]):
        print(f"  assets/{d}: {s / 1024 / 1024:.1f} MB")
