import glob, re
from collections import defaultdict

all_methods = []
for f in glob.glob('src/*.c'):
    with open(f, 'r', encoding='latin1') as fp:
        for line in fp:
            for m in re.finditer(r'"([a-zA-Z0-9_$/]+)\.([a-zA-Z0-9_$<]+)\(([^)]*)\)([^" ]+)"', line):
                cls, name, args, ret = m.groups()
                all_methods.append((cls, name, args, ret))

print(f"Total registered methods: {len(all_methods)}")
by_cn = defaultdict(list)
for cls, name, args, ret in all_methods:
    by_cn[(cls, name)].append((args, ret))

for (cls, name), l in by_cn.items():
    if len(l) > 1:
        print(f"{cls}.{name}: {l}")
