import zipfile

with open("../APK-UnrealLife-AowVN-20.apk", "rb") as f:
    z = zipfile.ZipFile(f)
    for info in z.infolist():
        if "assetpack" in info.filename:
            # find file offset
            header_offset = info.header_offset
            f.seek(header_offset)
            # local file header is 30 bytes + filename len + extra len
            f.seek(header_offset + 26)
            fn_len = int.from_bytes(f.read(2), "little")
            extra_len = int.from_bytes(f.read(2), "little")
            data_offset = header_offset + 30 + fn_len + extra_len
            print(f"{info.filename}: size={info.file_size}, data_offset={data_offset}, header_offset={header_offset}")
