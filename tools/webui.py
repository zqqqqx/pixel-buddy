# PlatformIO pre-script: gzip web/index.html and web/setup.html into src/web_ui.h (only rewritten when it changed).
import gzip, os
Import("env")
proj = env.subst("$PROJECT_DIR")
dst = os.path.join(proj, "src", "web_ui.h")
body = "// generated from web/*.html by tools/webui.py - do not edit\n#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
for name, sym in (("index.html", "WEB_UI"), ("setup.html", "SETUP_UI")):
    data = gzip.compress(open(os.path.join(proj, "web", name), "rb").read(), 9, mtime=0)
    body += "static const uint8_t %s_GZ[] = {%s};\n" % (sym, ",".join(str(b) for b in data))
    body += "static const size_t %s_GZ_LEN = %d;\n" % (sym, len(data))
if not os.path.exists(dst) or open(dst).read() != body:
    open(dst, "w").write(body)
    print("webui: %d bytes" % len(body))
