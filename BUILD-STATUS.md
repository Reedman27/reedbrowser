# Build status — 0.5.2 UI work

This source update adds a visible application menu, a Settings window, an Extensions page, extension install/remove controls, an explicit install-permissions confirmation, and a persistent dark-mode preference. The new-tab page also follows ReedBrowser's explicit dark-mode selection.

The previous AMO API response parsing fix remains included.

## Verification

The source was inspected and edited in this environment, but it was **not compiled or runtime-tested here** because this environment does not have the GTK4 or WebKitGTK 6.0 development packages. Build and runtime verification must be done on the Fedora development machine. Do not treat this archive as a verified release build.

## Known limitations

- Extensions installed through the UI are loaded on the next browser launch.
- Per-site extension permission prompts are not implemented; the confirmation screen explains that manifest permissions will be granted.
- Firefox WebExtensions compatibility is incomplete. In particular, uBlock Origin's blocking APIs and Proton Pass's popup/ports/content-script requirements are not fully implemented.
- The Firefox Add-ons website's in-page **Add to Firefox** button is not yet integrated with ReedBrowser's native installer.
- This is an initial settings and extension-management UI, not a feature-complete daily-driver browser yet.

## 0.5.2 customization build fix

- Fixed the custom accent-color callback compile error on GCC 16 / Fedora 45. The callback now uses a dedicated `AccentApplyData` struct instead of placing a comma-containing `std::pair<...>` type inside `G_CALLBACK`, which the C preprocessor incorrectly treated as multiple macro arguments.
- Release validation script passes all 12 checks and the source archive passes ZIP integrity validation.
- A full GTK4/WebKitGTK build was not available in the packaging environment; Fedora runtime build remains the final verification step.
