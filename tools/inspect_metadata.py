import re

with open(r"C:\Users\Hoang\.gemini\antigravity-ide\brain\de153c78-de78-419e-833c-d4a8992af0c8\scratch\apk\global-metadata.dat", "rb") as f:
    data = f.read()

keywords = [b"PlayAsset", b"AssetPack", b"Download", b"bgm_bundle", b"volta_bundle", b"MobileManager", b"TitleManager", b"LoadScene", b"SceneManager", b"Addressable"]
for kw in keywords:
    matches = [m.start() for m in re.finditer(kw, data)]
    print(f"Keyword {kw.decode()}: {len(matches)} occurrences")

# Find strings near bgm_bundle or volta_bundle
for m in re.finditer(b"bgm_bundle", data):
    start = max(0, m.start() - 100)
    end = min(len(data), m.end() + 100)
    print("Near bgm_bundle:", data[start:end])
