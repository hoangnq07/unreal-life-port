import re

with open(r"C:\Users\Hoang\.gemini\antigravity-ide\brain\de153c78-de78-419e-833c-d4a8992af0c8\scratch\apk\global-metadata.dat", "rb") as f:
    data = f.read()

# Let's search around "AssetPackFolderName" or "RetrieveAssetBundleAsyncInternal"
for m in re.finditer(b"AssetPackFolderName", data):
    start = max(0, m.start() - 200)
    end = min(len(data), m.end() + 400)
    chunk = data[start:end]
    s = "".join([chr(b) if 32 <= b <= 126 else " " for b in chunk])
    print(s)
