import logging
logging.disable(logging.CRITICAL)
from androguard.core.apk import APK
from androguard.core.dex import DEX

apk = APK(r'd:\Code\Unreal Life\APK-UnrealLife-AowVN-20.apk')
for dex_bytes in apk.get_all_dex():
    d = DEX(dex_bytes)
    for c in d.get_classes():
        name = c.get_name()
        if 'assetpacks' in name.lower():
            if any(k in name for k in ['Factory', 'Manager', 'Location', 'State', 'Status']):
                print('CLASS', name, 'super', c.get_superclassname())
                for m in c.get_methods():
                    print('  ', m.get_name(), m.get_descriptor())
