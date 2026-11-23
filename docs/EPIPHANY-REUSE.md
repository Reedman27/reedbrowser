# What we can reuse from the uploaded Epiphany source

The uploaded Epiphany source was inspected directly. It is useful, but it is not a standalone browser SDK that can simply be linked into ReedBrowser.

## Reusable design decisions adopted in 0.4.0

- One WebKit view per tab.
- A shared persistent `WebKitNetworkSession` for normal tabs.
- An ephemeral session for private browsing, shared by private tabs and destroyed when the last private tab closes.
- WebKit owns renderer process management and cross-site process swaps.
- Downloads are observed through `WebKitNetworkSession::download-started` in WebKitGTK 6.0.
- Content filtering uses WebKit's compiled `WebKitUserContentFilterStore` and per-view `WebKitUserContentManager`.

## Why the Epiphany extension manager is not copied wholesale

`src/webextension/ephy-web-extension-manager.c` is tightly integrated with Epiphany-specific objects and services, including `EphyEmbedShell`, `EphyWindow`, `EphyWebView`, `EphyHeaderBar`, and `EphyLocationEntry`. It also uses libadwaita, archive handling, JSON-GLib, and Epiphany's own extension storage and UI.

The manager registers handlers for these API groups in the supplied source:

- alarms
- browserAction
- commands
- cookies
- downloads
- menus
- notifications
- pageAction
- runtime
- storage
- tabs
- windows

That is useful implementation material for a later selective port, but importing the manager directly would pull a large portion of Epiphany's application architecture into ReedBrowser. The sensible route is to port the underlying API behavior behind ReedBrowser-owned interfaces, then add the UI pieces that make sense for this browser.

## Important WebKit distinction

WebKitGTK's `WebKitWebExtensionMode` (Manifest V2/V3) does not itself provide a Firefox-compatible extension runtime. It configures a WebView to treat content as an extension page, including extension-specific Content Security Policy behavior. ReedBrowser still needs its own manifest loader, extension background execution, content-script lifecycle, API/message bridge, permissions model, and extension UI.

## Port order

1. Manifest parsing and extension package validation.
2. Content-script registration and isolated-world injection.
3. Background page/service-worker lifecycle.
4. `runtime` messaging and `storage`.
5. `tabs` and `windows` APIs.
6. Browser/page actions and commands.
7. Downloads, cookies, alarms, notifications, menus.
8. Request interception/declarative rules and extension permissions.
9. Test real extensions; only then advertise compatibility.
