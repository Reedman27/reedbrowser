# 0.5.1 build status

## Verified (Ubuntu 24.04 container: GCC 13.3, GTK 4.14.5, WebKitGTK 2.52.6, json-glib 1.8, libarchive 3.7, SQLite 3.45)

- `./build.sh` configures and compiles cleanly. **Zero warnings** with `-Wall -Wextra -Wpedantic`.
- `tools/validate-release.py` passes (12/12).
- Browser launches under Xvfb, renders the local new-tab page, toolbar icons, vertical tabs, status bar.
- Address-bar navigation to an http page works (local test server).
- Bookmarks (Ctrl+D) persist and appear on the new-tab page; history appears under Recent.
- New tab (Ctrl+T) and private tab (Ctrl+Shift+N, marked, "Private browsing" status) work.
- Extensions: `--install-extension`, `--list-extensions` work from the CLI. With `tests/extension` installed: content script ran,
  injected CSS applied, content -> background `runtime.sendMessage` round-trip returned a reply, and the extension
  did **not** run in the private tab.
- Find in page: live search, match count, Enter / Ctrl+G next, Ctrl+Shift+G previous, Esc clears highlights.

## Fixed in 0.5.1

- Find in page: Enter used to restart the search instead of advancing; the match-count label was never updated;
  Esc left highlights on the page; there was no previous-match control or search-as-you-type.
- New-tab page showed `about:blank` in the address bar.

## NOT verified

- Fedora 45 build (different WebKitGTK/GTK versions than the Ubuntu container above).
- GPU compositing, video playback and hardware acceleration (tests ran with software rendering, WebKit sandbox disabled for the container).
- Real internet sites, downloads, session restore across a clean quit, zoom, and `--remove-extension`.
- Memory/CPU numbers on the target laptop. No performance claims are made.
- Real-world extensions (uBlock Origin, Proton Pass): still not working or untested; see `docs/EXTENSIONS.md`.
