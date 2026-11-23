# ReedBrowser 0.4.0 architecture

## Process and rendering model

```text
ReedBrowser GTK4 application
  ├── Tab A: WebKitWebView ─┐
  ├── Tab B: WebKitWebView ─┼── shared persistent WebKitNetworkSession
  └── Tab C: WebKitWebView ─┘
                           │
                 WebKit-managed network process
                 WebKit-managed web-content processes
                 WebKit GPU/compositing path

Private tabs: WebKitWebViews ── shared ephemeral WebKitNetworkSession (destroyed when last private tab closes)
```

The application deliberately does not try to force all tabs into one renderer. Modern WebKitGTK 6.0 uses `WebKitNetworkSession` instead of the old WebKitWebContext/WebsiteDataManager networking setup. Multiple views can use the same session. WebKit's process model and cross-site process swaps remain in control.

## Memory strategy

- Avoid a separate ReedBrowser process for each tab.
- Avoid offscreen rendering and per-frame shared-memory copies.
- Reuse one normal network session across normal tabs.
- Create background tabs without immediately navigating them during session restore.
- Keep the new-tab page local and script-free.
- Let WebKit manage renderer lifetime and site isolation rather than inventing a less secure single-renderer mode.

A low application-process count does not imply one web process or lower total memory in every workload. Measure the whole process tree and PSS/RSS on the target Fedora laptop.

## Epiphany-derived architectural lessons

The uploaded Epiphany source demonstrates the value of keeping a browser view per tab while sharing browser-wide state and WebKit infrastructure. ReedBrowser reuses that high-level design but intentionally avoids importing GNOME/libadwaita UI dependencies. The extension subsystem is not copied: it is a separate, incomplete Epiphany feature with its own integration and security work.

## Privacy boundary

All private tabs in the current window share a single ephemeral session so cookies and sign-ins work across private tabs. When the last private tab closes, ReedBrowser releases that session; the next private tab receives a fresh ephemeral session. Private tabs do not contribute to persistent history or session restore.
