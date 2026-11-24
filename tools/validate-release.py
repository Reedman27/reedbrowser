#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
checks = {
    "CMakeLists.txt": (root / "CMakeLists.txt").is_file(),
    "main.cpp": (root / "src/main.cpp").is_file(),
    "store.hpp": (root / "src/store.hpp").is_file(),
    "license": (root / "LICENSE").is_file(),
}
source = (root / "src/main.cpp").read_text()
checks["GTK4/WebKitGTK 6 API"] = '"network-session"' in source and '"user-content-manager"' in source
checks["no FLTK/offscreen framebuffer architecture"] = not any(x in source for x in ("Fl_Double_Window", "GtkOffscreenWindow", "memfd_create"))
checks["private ephemeral session"] = "webkit_network_session_new_ephemeral" in source
checks["content filter compilation"] = "webkit_user_content_filter_store_save" in source
checks["local homepage"] = "webkit_web_view_load_html" in source and "A quieter place to browse" in source
checks["no external homepage assets"] = not re.search(r'<(?:script|img|link)[^>]+(?:src|href)=[\"\']https?://', source, re.I)
failed = False
for name, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}  {name}")
    failed |= not ok
sys.exit(1 if failed else 0)
