import zipfile
import re

with zipfile.ZipFile("../APK-UnrealLife-AowVN-20.apk") as z:
    for name in z.namelist():
        if name.endswith(".dex"):
            data = z.read(name)
            matches = re.findall(b"L[a-zA-Z0-9_/]*(?:asset|Asset|byfen|Byfen|archive|Archive|PlayAsset|download|Download)[a-zA-Z0-9_/]*;", data)
            print(f"=== {name} ===")
            for m in sorted(set(matches))[:50]:
                try:
                    print(m.decode("utf-8", "ignore"))
                except:
                    pass
