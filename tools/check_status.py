import logging
logging.disable(logging.CRITICAL)
from androguard.core.apk import APK
from androguard.core.dex import DEX

apk = APK(r'd:\Code\Unreal Life\APK-UnrealLife-AowVN-20.apk')
for dex_bytes in apk.get_all_dex():
    d = DEX(dex_bytes)
    for c in d.get_classes():
        if 'AssetPackStatus' in c.get_name():
            print('CLASS', c.get_name())
            for f in c.get_fields():
                print('  field:', f.get_name(), f.get_init_value())
