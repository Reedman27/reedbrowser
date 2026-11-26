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
runtime (id, getURL, getManifest, sendMessage, onMessage, **connect/onConnect ports**, openOptionsPage, onInstalled, onStartup, getPlatformInfo), storage (local/sync/session + onChanged), tabs (query/get/create/update/reload/remove/sendMessage/**connect/executeScript/insertCSS/removeCSS** + events), **scripting** (executeScript with func/files, insertCSS/removeCSS, register/unregister/update/getRegisteredContentScripts), **contentScripts.register**, **declarativeNetRequest** (static rulesets, dynamic and session rules, enable/disable rulesets), **webRequest (observe-only: onBeforeRequest, onCompleted, onErrorOccurred with URL/type/tab filters; cannot block or modify)**, **cookies** (get/getAll/set/remove, needs the `cookies` permission), **webNavigation** (top-frame onBeforeNavigate/onCommitted/onDOMContentLoaded/onCompleted), i18n, alarms, permissions (stub), action/browserAction/pageAction (**toolbar button, badge, title, popup**), **options page window**, windows basics. Unsupported APIs are rejected and reported, not silently ignored.

### declarativeNetRequest
Rules are converted to WebKit content rule lists (`src/dnr.hpp`). Supported: block, allow, allowAllRequests (as "ignore on these sites"), upgradeScheme, urlFilter (`||`, `|`, `*`, `^`), regexFilter limited to WebKit's regex subset, requestDomains, initiator domains, resource types (main/sub frame via `load-context`), domainType, case sensitivity, priorities. Skipped and logged: redirect, modifyHeaders, regex with alternation or `{n}`, requestMethods, tabIds, excludedRequestDomains on block rules. Extensions do not affect private tabs.

## Not supported yet
1. **webRequestBlocking, header and redirect events** — WebKitGTK has no synchronous request hook, so listeners that block or rewrite requests have no effect (request types are guessed from the URL). **uBlock Origin (MV2, the default AMO listing) installs and its popup/dashboard work (verified against a build from gorhill/uBlock source), but it cannot block.** Use **uBlock Origin Lite** (MV3, declarativeNetRequest) instead, or ReedBrowser's own blocker. Full uBO would need a WebKit web-process extension.
2. `filterResponseData`, `dns`, native messaging (`connectNative`/`sendNativeMessage`), `cookies.onChanged`, sub-frame `webNavigation` events, commands/shortcuts, notifications.
3. Per-site permission prompts, optional permissions, extension auto-update. All manifest permissions are granted at install.
4. Popup/options pages can message the background and the background can message open popup/options pages. Page-to-page messages (popup to options) are not delivered.
5. scripting `allFrames`/frame targeting: injection is top frame only. MV3 service-worker lifecycle: the background runs as a persistent page.
6. **Proton Pass**: popup, options, ports, scripting, storage and context menus are now in place, but it may use other APIs listed above. Untested with the real extension.

## Tested
`tests/extension/` (MV2 basics) and `tests/extension-mv3/` (popup, options, ports both ways, scripting.executeScript, tabs.executeScript, insertCSS, registerContentScripts, static + dynamic declarativeNetRequest blocking, badge, background-to-popup messages, page context-menu items) under Xvfb on Ubuntu with WebKitGTK 2.52. Run the MV3 one with the files in `tests/extension-mv3/site/` served on 127.0.0.1:8099. Not tested on Fedora 45 or with real store extensions.

## Permissions (enforced at runtime)

- API namespaces need their manifest permission (`storage`, `cookies`, `menus`/`contextMenus`, `scripting`, `webRequest`, `declarativeNetRequest*`); otherwise the call is refused and logged.
- `tabs.query/get` and tab events only include `url`/`title` for extensions with the `tabs` permission, host access to that page, or an `activeTab` grant.
- `scripting.*` / `tabs.executeScript` / `insertCSS` need host access (or `activeTab`) for the target tab; `cookies.*` need host access for the cookie's site.
- `activeTab` is granted when the user clicks the toolbar button, opens the popup or uses a context-menu item, and ends when that tab navigates or closes.
- `optional_permissions` are never granted (`permissions.request()` declines).
- Disabling or removing an extension stops its background page, closes its popup/options pages and removes its scripts, styles, handlers and network rules from open tabs immediately. Installing attaches to open tabs without a restart.
- Every file path named by a manifest or by `scripting.*` goes through one resolver that rejects `..`, absolute paths and symlinks leaving the extension directory.

## Ad & tracker blocking (native)

uBlock Origin's own engine cannot run on WebKitGTK (no synchronous request interception), so ReedBrowser
blocks natively: `src/filters.cpp` downloads EasyList, EasyPrivacy and the uBlock filter lists (Settings >
Privacy), `src/abp.hpp` converts Adblock Plus/uBlock syntax into WebKit content rules (network filters,
exceptions, element hiding, `$third-party`, `$domain=`, resource types, `$badfilter`, ...), and WebKit compiles
them. Compiled rules are cached on disk; lists refresh every few days. "My filters" accepts your own rules,
and the main menu can switch blocking off for the current site. Not supported: `$redirect`, `$removeparam`,
`$csp`, procedural cosmetics (`:has-text` ...), HTML filtering.
