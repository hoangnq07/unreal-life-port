import re

with open(r"C:\Users\Hoang\.gemini\antigravity-ide\brain\de153c78-de78-419e-833c-d4a8992af0c8\scratch\apk\global-metadata.dat", "rb") as f:
    data = f.read()

# Let's search for "PlayAssetDelivery" methods in the metadata
idx = data.find(b"PlayAssetDelivery")
while idx != -1:
    start = max(0, idx - 50)
    end = min(len(data), idx + 200)
    chunk = data[start:end]
    s = "".join([chr(b) if 32 <= b <= 126 else " " for b in chunk])
    print(s)
    idx = data.find(b"PlayAssetDelivery", idx + 1)
