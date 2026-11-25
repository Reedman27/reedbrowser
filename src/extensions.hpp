// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
//
// WebExtensions runtime for ReedBrowser (Firefox-style `browser.*` / `chrome.*`).
//
// WebKitGTK only parses manifests and match patterns; everything else lives here:
// the extension registry and installer, the moz-extension:// file scheme, a hidden
// background page per extension, content-script injection into an isolated JS world,
// and a native bridge for runtime messaging, storage and tabs.
// See docs/EXTENSIONS.md for what is and is not supported.
#pragma once

#include <webkit/webkit.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class ExtensionManager {
public:
    struct TabInfo {
        int id = 0;
        std::string url, title;
        bool active = false;
    };
    // The browser supplies its tab model; the runtime never touches browser internals.
    struct Host {
        std::function<std::vector<TabInfo>()> list_tabs;
        std::function<int(const std::string &url, bool active)> open_tab;  // returns new tab id
        std::function<void(int id)> close_tab;
        std::function<void(int id)> activate_tab;
    };

    ExtensionManager(std::string data_dir, WebKitNetworkSession *session, Host host);
    ~ExtensionManager();

    // CLI-side management (no display needed).
    bool install(const std::string &path, bool assume_yes, std::string *message);
    // Thread-safe: download an AMO page/amo:slug or a direct https XPI to `destination`.
    static bool fetch_package(const std::string &source, const std::string &destination, std::string *err);
    // Unpack to a scratch directory and describe what the extension asks for (for the consent dialog).
    bool inspect(const std::string &path, std::string *summary, std::string *err);
    bool remove(const std::string &id_or_name, std::string *message);
    std::string describe() const;  // human-readable list

    // Runtime (needs GTK/WebKit running).
    void start();                                       // register scheme, start background pages
    void attach_view(WebKitWebView *view, WebKitUserContentManager *ucm, int tab_id, bool is_private);
    void detach_view(WebKitWebView *view);
    void tab_created(int id);
    void tab_removed(int id);
    void tab_activated(int id);
    void tab_committed(int id, const std::string &url);  // top-frame navigation committed (webNavigation events)
    void tab_updated(int id, const std::string &status, const std::string &url, const std::string &title);
    int extension_count() const;

    // Toolbar buttons, popups, options pages, management UI.
    struct ActionInfo { std::string uuid, name, title, badge, badge_color, icon_path, popup; };
    struct Summary { std::string uuid, id, name, version; bool enabled; bool has_options; };
    std::vector<ActionInfo> actions() const;
    std::vector<Summary> list() const;
    WebKitWebView *create_popup(const std::string &uuid);    // nullptr if the extension has no popup; floating widget
    WebKitWebView *create_options(const std::string &uuid);  // nullptr if none; floating widget
    void action_clicked(const std::string &uuid, int tab_id);  // for buttons without a popup
    void set_ui_callbacks(std::function<void()> actions_changed, std::function<void(const std::string &uuid)> open_options);
    void start_new();                                    // start extensions installed while running
    void set_enabled(const std::string &uuid, bool on);  // stops / starts it immediately, including in open tabs

    // Extension entries for the page context menu (menus.create / contextMenus.create).
    struct MenuItem { std::string uuid, ext_name, id, title, parent, type; bool enabled = true, checked = false; };
    struct MenuClick { std::string menu_id, page_url, link_url, src_url, selection, media_type; bool editable = false, checked = false; };
    // `contexts` holds what was right-clicked: page, link, image, video, audio, selection, editable, frame.
    std::vector<MenuItem> context_menu_items(const std::vector<std::string> &contexts) const;
    void context_menu_clicked(const std::string &uuid, const MenuClick &click, int tab_id);

    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};
