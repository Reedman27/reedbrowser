# Roadmap after 0.5.1

1. Build and run on Fedora 45 on the real laptop (0.5.1 already compiles and runs in an Ubuntu 24.04 container); test GPU/video; fix anything Fedora-specific before adding features.
2. Add a proper downloads popover and download history actions.
3. Add a settings window for search provider, homepage, privacy, and permissions.
4. Improve filter lists and add safe update/import workflow.
5. Audit and selectively adapt Epiphany's extension manager architecture; do not claim Firefox compatibility until real extensions pass tests.
6. Add a permission prompt/store, password manager integration, and autofill.
7. Benchmark on the Ryzen 5 7520U / Radeon 610M / approximately 5 GB usable RAM target.
8. Add tab discard only after measuring WebKit lifecycle APIs and verifying that form state/session restore are safe.
