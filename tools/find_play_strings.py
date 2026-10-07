import re

with open(r"C:\Users\Hoang\.gemini\antigravity-ide\brain\de153c78-de78-419e-833c-d4a8992af0c8\scratch\apk\global-metadata.dat", "rb") as f:
    data = f.read()

matches = re.findall(b"com/google/android/play[^\x00]{1,100}|com\\.google\\.android\\.play[^\x00]{1,100}", data)
for m in sorted(set(matches)):
    try:
        print(m.decode("utf-8", "ignore"))
    except:
        pass
