# ReedBrowser settings

Open the application menu at the right end of the navigation toolbar and choose **Settings & extensions**.

## Appearance

- Theme: System default, Light, or Dark. The choice is saved between launches.
- Accent color: choose from ten presets or enter any six-digit HEX color (for example `#8b5cf6`). The accent is used by browser controls and the new-tab page.
- Interface density: Compact, Comfortable, or Spacious toolbar spacing.
- UI text size: scale browser toolbar text from 80% to 140%.
- Tab placement: Left, Top, Right, or Bottom.

Preferences are stored in `~/.local/share/reedbrowser/preferences.ini`.

## Extensions

View installed extensions, install an add-on from a Firefox Add-ons (AMO) page URL or a local XPI/ZIP path, and remove an installed extension by name or ID. Extension installation has an explicit confirmation step. Extensions load after restarting ReedBrowser. Firefox extension API compatibility is incomplete; installation does not guarantee that uBlock Origin, Proton Pass, or another Firefox extension will function correctly.

## Privacy and About

The Privacy page is currently an informational placeholder; per-site permissions and advanced privacy controls are not yet implemented. About identifies the browser and rendering stack.

Settings controls described above are wired to saved preferences. This is still an evolving browser, not yet a fully featured daily-driver replacement.
