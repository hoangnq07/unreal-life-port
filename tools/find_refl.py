import zipfile
import re

with zipfile.ZipFile("../APK-UnrealLife-AowVN-20.apk") as z:
    for name in z.namelist():
        if name.endswith(".dex"):
            data = z.read(name)
            matches = re.findall(b"ReflectionHelper[^\x00]{1,100}", data)
            print(f"=== {name} ===")
            for m in set(matches):
                try:
                    print(m.decode("utf-8", "ignore"))
                except:
                    pass
