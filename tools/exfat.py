# PlatformIO pre-script: enable exFAT in ESP-IDF's FatFs (SD cards > 32 GB are exFAT formatted).
import os, re
Import("env")
ffconf = os.path.join(env.PioPlatform().get_package_dir("framework-espidf"), "components", "fatfs", "src", "ffconf.h")
src = open(ffconf).read()
if re.search(r"#define FF_FS_EXFAT\s+0", src):
    open(ffconf, "w").write(re.sub(r"#define FF_FS_EXFAT\s+0", "#define FF_FS_EXFAT\t\t1", src))
    print("exfat: enabled FF_FS_EXFAT in", ffconf)
