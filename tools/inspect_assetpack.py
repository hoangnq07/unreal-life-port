import zipfile

with zipfile.ZipFile("../APK-UnrealLife-AowVN-20.apk") as z:
    for info in z.infolist():
        if "assetpack" in info.filename or "byfen" in info.filename:
            print(f"{info.filename}: size={info.file_size / 1024 / 1024:.2f}MB, compress={info.compress_type}")
