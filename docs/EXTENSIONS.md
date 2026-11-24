# ReedBrowser WebExtensions runtime (0.5.0 preview)

WebKitGTK only parses manifests and match patterns. Everything else is `src/extensions.cpp` + `src/ext/api.js`.

## Installing from Firefox Add-ons (AMO)

The CLI installer accepts a public AMO add-on page URL (for example `https://addons.mozilla.org/firefox/addon/ublock-origin/`) or `amo:<slug>`. It resolves the current release through the official AMO API, downloads the XPI over HTTPS, then runs the same manifest/permission/consent checks as a local XPI. This installs the package; it does **not** imply full Firefox API compatibility. In this preview, the user supplies the listing URL to the CLI; clicking AMO’s in-page “Add to Firefox” button is not yet connected to the native installer.

## How it works
- Extensions live in `~/.local/share/reedbrowser/extensions/<id>/`; registry in `registry.json`; storage in `extension-data/<uuid>/`.
- `moz-extension://<uuid>/...` served by a custom URI scheme (path traversal blocked).
- Each extension with a background script gets a hidden WebView (own content manager, no page access).
- Content scripts/CSS are injected into an isolated JS world `reed-ext-<uuid>` limited by match patterns. Websites cannot see the bridge: only per-extension handlers registered in that world exist on tabs.
- Extensions do not run in private tabs. Install shows requested permissions and asks consent; zip path traversal is rejected.
- Messages queue until the background page has loaded.

## Supported
runtime (id, getURL, getManifest, sendMessage, onMessage, onInstalled, onStartup, getPlatformInfo), storage (local/sync/session + onChanged), tabs (query/get/create/update/reload/remove/sendMessage + events), i18n, alarms, permissions (stub), action/browserAction state, windows basics. Unsupported APIs are rejected and reported, not silently ignored.

## Not supported yet
1. **webRequest / webRequestBlocking** — WebKitGTK has no synchronous request hook. Blocks uBlock Origin (MV2) as shipped. Options: (a) convert filter lists to WebKit content rules natively (what ReedBrowser's own blocker already uses), (b) a web-process extension hooking requests, (c) target uBlock Origin Lite (MV3, declarativeNetRequest).
2. `runtime.connect` ports, `tabs.executeScript/insertCSS`, `contentScripts.register`, `filterResponseData`, `dns`.
3. Popup/options UI, toolbar buttons, context menus, commands.
4. Proton Pass needs popup UI + ports + autofill content scripts; untested.
5. Per-site permission prompts and extension auto-update. AMO package download/install is implemented in the 0.5.2 CLI preview; in-page Add to Firefox integration is not yet connected.

## Tested
`tests/extension/` (content script, CSS, content→background message with reply, background→content `tabs.sendMessage`, storage persistence, onInstalled) under Xvfb on Ubuntu with WebKitGTK 2.52. Not tested on Fedora 45 or with real extensions.
