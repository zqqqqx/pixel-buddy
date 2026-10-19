# PlatformIO pre-script: web/index.html, web/setup.html (gzipped) and the web fonts into src/web_ui.h (only
# rewritten when it changed).
import gzip, os
Import("env")
proj = env.subst("$PROJECT_DIR")
dst = os.path.join(proj, "src", "web_ui.h")
body = "// generated from web/*.html by tools/webui.py - do not edit\n#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
# the pages gzipped; the brand font (web/fonts/*.woff2 from tools/fontgen.py) as it is (already compressed)
for name, sym in (("index.html", "WEB_UI"), ("setup.html", "SETUP_UI"), ("fonts/quicksand-500.woff2", "FONT_500"),
                  ("fonts/quicksand-700.woff2", "FONT_700")):
    raw = open(os.path.join(proj, "web", name), "rb").read()
    data = raw if name.endswith(".woff2") else gzip.compress(raw, 9, mtime=0)
    body += "static const uint8_t %s_GZ[] = {%s};\n" % (sym, ",".join(str(b) for b in data))
    body += "static const size_t %s_GZ_LEN = %d;\n" % (sym, len(data))
if not os.path.exists(dst) or open(dst).read() != body:
    open(dst, "w").write(body)
    print("webui: %d bytes" % len(body))
