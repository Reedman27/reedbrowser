# ReedBrowser WebExtensions runtime (0.5.0 preview)

WebKitGTK only parses manifests and match patterns. Everything else is `src/extensions.cpp` + `src/ext/api.js`.

## Installing from Firefox Add-ons (AMO)
Open addons.mozilla.org in ReedBrowser. The browser identifies as Firefox to that site only and provides `navigator.mozAddonManager`, so the real **Add to Firefox** button starts a native install. A blue **Install in ReedBrowser** button is also injected on every add-on page as a fallback, and any `.xpi` link or `application/x-xpinstall` response is intercepted. The package is downloaded over HTTPS in the background, a consent dialog is shown, and the extension starts immediately (reload open tabs so content scripts attach). The CLI (`--install-extension <AMO URL|amo:slug|file>`) is unchanged. The web flow has **not** been verified against the live site; it was written against AMO's documented install API.

## How it works
- Extensions live in `~/.local/share/reedbrowser/extensions/<id>/`; registry in `registry.json`; storage in `extension-data/<uuid>/`.
- `moz-extension://<uuid>/...` served by a custom URI scheme (path traversal blocked).
- Each extension with a background script gets a hidden WebView (own content manager, no page access).
- Content scripts/CSS are injected into an isolated JS world `reed-ext-<uuid>` limited by match patterns. Websites cannot see the bridge: only per-extension handlers registered in that world exist on tabs.
- Extensions do not run in private tabs. Install shows requested permissions and asks consent; zip path traversal is rejected.
- Messages queue until the background page has loaded.

## Supported
runtime (id, getURL, getManifest, sendMessage, onMessage, **connect/onConnect ports**, openOptionsPage, onInstalled, onStartup, getPlatformInfo), storage (local/sync/session + onChanged), tabs (query/get/create/update/reload/remove/sendMessage/**connect/executeScript/insertCSS/removeCSS** + events), **scripting** (executeScript with func/files, insertCSS/removeCSS, register/unregister/update/getRegisteredContentScripts), **contentScripts.register**, **declarativeNetRequest** (static rulesets, dynamic and session rules, enable/disable rulesets), i18n, alarms, permissions (stub), action/browserAction/pageAction (**toolbar button, badge, title, popup**), **options page window**, windows basics. Unsupported APIs are rejected and reported, not silently ignored.

### declarativeNetRequest
Rules are converted to WebKit content rule lists (`src/dnr.hpp`). Supported: block, allow, allowAllRequests (as "ignore on these sites"), upgradeScheme, urlFilter (`||`, `|`, `*`, `^`), regexFilter limited to WebKit's regex subset, requestDomains, initiator domains, resource types (main/sub frame via `load-context`), domainType, case sensitivity, priorities. Skipped and logged: redirect, modifyHeaders, regex with alternation or `{n}`, requestMethods, tabIds, excludedRequestDomains on block rules. Extensions do not affect private tabs.

## Not supported yet
1. **webRequest / webRequestBlocking** — WebKitGTK has no synchronous request hook. **uBlock Origin (MV2, the default AMO listing) installs but cannot block.** Use **uBlock Origin Lite** (MV3, declarativeNetRequest) instead, or ReedBrowser's own blocker. Full uBO would need a WebKit web-process extension.
2. `filterResponseData`, `dns`, native messaging (`connectNative`/`sendNativeMessage`), `webNavigation` events, context menus, commands/shortcuts, `cookies`, notifications.
3. Per-site permission prompts, optional permissions, extension auto-update. All manifest permissions are granted at install.
4. Messages from the background page to a popup/options page (`runtime.sendMessage` goes only to the background). Ports work in both directions.
5. scripting `allFrames`/frame targeting: injection is top frame only. MV3 service-worker lifecycle: the background runs as a persistent page.
6. **Proton Pass**: popup, options, ports, scripting, and storage are now in place, but it also uses APIs listed above (e.g. native messaging is not required; `webNavigation`, `cookies`, `webRequest` may be). Untested with the real extension.

## Tested
`tests/extension/` (MV2 basics) and `tests/extension-mv3/` (popup, options, ports both ways, scripting.executeScript, tabs.executeScript, insertCSS, registerContentScripts, static + dynamic declarativeNetRequest blocking, badge) under Xvfb on Ubuntu with WebKitGTK 2.52. Run the MV3 one with the files in `tests/extension-mv3/site/` served on 127.0.0.1:8099. Not tested on Fedora 45 or with real store extensions.
