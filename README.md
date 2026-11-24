# ReedBrowser 0.5.2 — Fedora 45 development build (AMO XPI installer preview)

ReedBrowser is a small, independent Linux browser built on **GTK 4 + WebKitGTK 6.0**. This release replaces the earlier FLTK/offscreen-framebuffer architecture with native WebKit widgets.

The target is a responsive browser for modest systems, including laptops with about 5 GB of usable RAM. The browser UI does not create a separate application process for each tab. Normal tabs share one persistent `WebKitNetworkSession`; WebKit controls its own network and web-content processes, including process swaps and site isolation. Private tabs use ephemeral sessions.

## Why this architecture

The previous 0.3.x design rendered each tab into an offscreen GTK window and copied frames through shared memory to FLTK. That forced software-style frame capture, complicated input forwarding, and was a poor fit for video, WebGL, and modern compositing.

The 0.4.0 design embeds `WebKitWebView` widgets directly in GTK4. This removes the frame-copy bridge and lets WebKit use its normal rendering/compositing path. It also follows the useful architecture seen in Epiphany: one WebKit view per tab, shared normal browsing session, and ephemeral private sessions. WebKit—not ReedBrowser—decides when renderer processes are shared or replaced.

## Firefox extensions (preview)

ReedBrowser 0.5.2 can resolve an add-on listing from the official Firefox Add-ons website (AMO), download its XPI over HTTPS, and pass it through ReedBrowser’s existing manifest and permission installer. This is package installation support, **not full Firefox WebExtensions compatibility**. The runtime remains WebKitGTK-based; uBlock Origin and Proton Pass still require additional API and UI work.

ReedBrowser has its own WebExtensions runtime on top of WebKitGTK (the approach Orion takes). Install directly from a Firefox Add-ons (AMO) listing URL, or use a local XPI/ZIP/folder:

    reedbrowser --install-extension https://addons.mozilla.org/firefox/addon/ublock-origin/  # install directly from AMO
    reedbrowser --install-extension /path/to/addon.xpi  # or install a local XPI/ZIP/folder
    reedbrowser --list-extensions
    reedbrowser --remove-extension <id-or-name>

Working today: `moz-extension://` pages, background scripts/pages, content scripts + CSS (isolated JS world, match patterns), `runtime.sendMessage`/`onMessage`, `storage.local/sync/session`, `tabs.*` basics, `tabs.sendMessage`, `i18n`, `alarms`. **Not yet: `webRequest` blocking (what uBlock Origin MV2 needs), ports, popups/options pages, toolbar buttons.** uBlock Origin and Proton Pass are NOT confirmed working. See docs/EXTENSIONS.md.

## Implemented in this source build

- Native GTK4 window and WebKitGTK 6.0 views (no offscreen capture or FLTK).
- Left-side vertical tabs, close buttons, tab reordering, popup/new-window links opening in tabs.
- Shared persistent session for normal tabs; one ephemeral session shared by private tabs; it is discarded after the last private tab closes.
- Lazy loading of restored background tabs: their WebViews are created but navigation starts when selected.
- Back, forward, reload, stop, home, address/search bar.
- Local-only new-tab page. It makes no requests until the user submits a search or opens a bookmark.
- SQLite bookmarks, history, recent pages, and session restore.
- Download destination handling to the user's Downloads directory.
- Find in page with live match count, zoom controls, common keyboard shortcuts.
- WebKit Intelligent Tracking Prevention (ITP).
- Compiled WebKit content filter with a small built-in tracker/ad-domain starter list.
- Site permission requests are denied by default in this first build.
- Fedora RPM spec and desktop integration.

## Fedora 45 build

Install dependencies:

```bash
sudo dnf install gcc-c++ cmake make pkgconf-pkg-config gtk4-devel webkitgtk6.0-devel sqlite-devel json-glib-devel libarchive-devel libcurl-devel
```

Build:

```bash
./build.sh
```

Run:

```bash
./build/reedbrowser
```

Install locally:

```bash
sudo cmake --install build --prefix /usr/local
```

The build script defaults to two parallel jobs to avoid exhausting low-memory machines. Override with `JOBS=1` or `JOBS=3` if needed.

## Fedora RPM

From the project root:

```bash
mkdir -p ~/rpmbuild/SOURCES
 tar -czf ~/rpmbuild/SOURCES/reedbrowser-0.5.1.tar.gz --transform 's,^,reedbrowser-0.5.1/,' . --exclude=./build --exclude=./.git
rpmbuild -ba packaging/reedbrowser.spec
```

## Data locations

- Profile/database: `~/.local/share/reedbrowser/`
- WebKit cache: `~/.cache/reedbrowser/`
- Downloads: the XDG Downloads directory, falling back to `~/Downloads`

## Shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+Shift+N | New private tab |
| Ctrl+L / F6 | Focus address bar |
| Ctrl+R / F5 | Reload |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+D | Bookmark current page |
| Ctrl+F | Find in page (type to search, Enter / Ctrl+G next, Ctrl+Shift+G previous) |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset |
| Esc | Stop loading / close find bar |

## Current limitations — please test before relying on it

This is an early development build, not a finished browser release. As of 0.5.1 it **compiles cleanly and has been run and driven end to end** (Ubuntu 24.04, GTK 4.14, WebKitGTK 2.52, under Xvfb). It has **not** yet been built or run on Fedora 45, and not on real hardware with GPU/video. See `BUILD-STATUS.md` for exactly what was and was not tested.

- The built-in filter is a starter list, not EasyList/uBlock Origin-class filtering.
- Firefox WebExtensions are not implemented. Epiphany's extension code is a possible future donor/reference, but its current extension support is experimental and tightly integrated with Epiphany.
- Password manager, autofill, sync, extension UI, permissions UI, print UI, and downloads manager window are not implemented yet.
- Private tabs are ephemeral, but their existence is visible in the current browser window and they share the same browser UI process.
- Permission prompts are denied rather than shown in a permission manager.
- Media playback and hardware acceleration depend on the Fedora WebKitGTK/GStreamer/Mesa stack and must be tested on the Radeon 610M laptop.
- No performance claims are made until startup, memory, CPU, tab switching, and video playback are measured on the target machine.

## Epiphany source use

The uploaded Epiphany source was inspected as an architectural reference, especially its tab/WebKit integration, browser session model, downloads, history, and extension subsystem. This 0.4.0 implementation does **not** copy Epiphany source files. It uses the public WebKitGTK API and the existing ReedBrowser AGPL-3.0-only project license.

## License

GNU Affero General Public License version 3 only (`AGPL-3.0-only`). See `LICENSE`.

## Settings and appearance

Open the application menu at the right end of the navigation toolbar and select **Settings & extensions**. The settings window includes an Appearance page with a persistent dark-mode toggle, an Extensions page for viewing/installing/removing extensions, and an About page. See [Settings UI](docs/SETTINGS-UI.md) for current behavior and limitations.

