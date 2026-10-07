import re

with open(r"C:\Users\Hoang\.gemini\antigravity-ide\brain\de153c78-de78-419e-833c-d4a8992af0c8\scratch\apk\global-metadata.dat", "rb") as f:
    data = f.read()

for kw in [b"PlayAsset", b"AssetPack"]:
    for m in re.finditer(kw, data):
        start = max(0, m.start() - 50)
        end = min(len(data), m.end() + 80)
        chunk = data[start:end]
        # extract printable strings
        s = "".join([chr(b) if 32 <= b <= 126 else " " for b in chunk])
        print(f"[{kw.decode()}] {s}")
