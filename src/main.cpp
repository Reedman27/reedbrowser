// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
// ReedBrowser 0.5.0: native GTK4 + WebKitGTK 6.0 browser.
// WebKit owns rendering, GPU compositing, networking and renderer isolation.

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit/webkit.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "store.hpp"
#include "extensions.hpp"

#ifndef REEDBROWSER_VERSION
#define REEDBROWSER_VERSION "0.5.0"
#endif

namespace {

struct Browser;
struct Tab {
    int id{0};
    Browser *browser{};
    WebKitWebView *view{};
    WebKitNetworkSession *session{};
    WebKitUserContentManager *content_manager{};
    GtkWidget *tab_label{};
    GtkWidget *title_label{};
    bool private_mode{false};
    bool needs_load{false};
    std::string title{"New tab"};
    std::string uri;
};

struct Browser {
    GtkApplication *app{};
    GtkWidget *window{};
    GtkWidget *toolbar{};
    GtkWidget *notebook{};
    GtkWidget *address{};
    GtkWidget *status{};
    GtkWidget *progress{};
    GtkWidget *find_revealer{};
    GtkWidget *find_entry{};
    GtkWidget *find_count{};
    WebKitNetworkSession *normal_session{};
    WebKitNetworkSession *private_session{};
    WebKitUserContentFilterStore *filter_store{};
    WebKitUserContentFilter *content_filter{};
    Store store;
    std::unique_ptr<ExtensionManager> ext;
    int next_tab_id{1};
    std::vector<std::unique_ptr<Tab>> tabs;
    std::unordered_map<WebKitDownload *, std::string> downloads;
    std::string data_dir;
    std::string cache_dir;
    std::string download_dir;
    bool shutting_down{false};
};

static std::string home_path(const char *leaf) {
    const char *home = g_get_home_dir();
    return std::string(home ? home : ".") + "/" + leaf;
}

static void set_status(Browser *b, const std::string &s) {
    if (b->status) gtk_label_set_text(GTK_LABEL(b->status), s.c_str());
}

static std::string html_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

static std::string uri_escape(const std::string &s) {
    gchar *escaped = g_uri_escape_string(s.c_str(), nullptr, TRUE);
    std::string out = escaped ? escaped : "";
    g_free(escaped);
    return out;
}

static bool starts_with(const std::string &s, const char *prefix) {
    return s.rfind(prefix, 0) == 0;
}

static bool is_local_host(const std::string &host) {
    if (host == "localhost" || host == "[::1]") return true;
    if (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0) return true;
    if (starts_with(host, "127.") || starts_with(host, "10.") || starts_with(host, "192.168.")) return true;
    if (starts_with(host, "172.")) {  // 172.16.0.0/12
        int second = atoi(host.c_str() + 4);
        return second >= 16 && second <= 31;
    }
    return false;
}

static std::string normalize_input(const std::string &input) {
    std::string value = input;
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    // "localhost:8080" parses as a URI with scheme "localhost", so only accept explicit
    // "://" URLs and a short list of schemes that have no authority part.
    if (value.find("://") != std::string::npos || starts_with(value, "about:") || starts_with(value, "data:") ||
        starts_with(value, "view-source:") || starts_with(value, "file:"))
        return value;
    if (value.find(' ') != std::string::npos || value.find('\t') != std::string::npos)
        return "https://duckduckgo.com/?q=" + uri_escape(value);
    std::string host = value.substr(0, value.find_first_of("/?#"));
    if (!starts_with(host, "[")) host = host.substr(0, host.find(':'));
    else host = host.substr(0, host.find(']') + 1);
    bool has_port = value.find(':') != std::string::npos && value.find(':') < value.find_first_of("/?#");
    if (is_local_host(host)) return "http://" + value;
    if (host.find('.') != std::string::npos || has_port) return "https://" + value;
    return "https://duckduckgo.com/?q=" + uri_escape(value);
}

static Tab *active_tab(Browser *b) {
    if (!b || !b->notebook) return nullptr;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->notebook),
                                                gtk_notebook_get_current_page(GTK_NOTEBOOK(b->notebook)));
    if (!page) return nullptr;
    for (auto &tab : b->tabs) if (GTK_WIDGET(tab->view) == page) return tab.get();
    return nullptr;
}

static std::string homepage_html(Browser *b, bool private_mode) {
    std::string html = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>New Tab</title><style>
:root{color-scheme:light dark}*{box-sizing:border-box}body{margin:0;background:#f5f6f8;color:#20242b;font:15px system-ui,-apple-system,sans-serif}.wrap{max-width:760px;margin:12vh auto 0;padding:28px}.brand{font-size:34px;font-weight:650;letter-spacing:-1.4px;margin-bottom:8px}.sub{color:#747b86;margin-bottom:28px}.search{display:flex;background:white;border:1px solid #dce0e6;border-radius:12px;padding:14px 17px;box-shadow:0 4px 18px #1720330b}.search input{border:0;outline:0;background:transparent;color:inherit;width:100%;font:inherit}.section{margin-top:34px}.heading{font-size:12px;font-weight:700;letter-spacing:.09em;color:#747b86;margin-bottom:12px}.links{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:9px}.link{display:block;padding:12px 14px;border:1px solid #e0e3e8;border-radius:10px;background:#fff;color:inherit;text-decoration:none;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.link:hover{border-color:#8b9bb6;background:#fafbfc}.muted{color:#858c96;font-size:13px}@media(prefers-color-scheme:dark){body{background:#17191e;color:#e7e9ed}.search,.link{background:#22252c;border-color:#343943}.link:hover{background:#292d35;border-color:#68758a}.sub,.heading,.muted{color:#9ba3b0}}</style></head><body><main class="wrap"><div class="brand">ReedBrowser</div><div class="sub">A quieter place to browse.</div><form class="search" action="https://duckduckgo.com/" method="get"><input name="q" autofocus placeholder="Search the web or enter an address" aria-label="Search"></form>)HTML";
    auto bookmarks = b->store.bookmarks();
    html += "<section class=\"section\"><div class=\"heading\">BOOKMARKS</div><div class=\"links\">";
    if (bookmarks.empty()) html += "<span class=\"muted\">Your saved sites will appear here.</span>";
    for (const auto &row : bookmarks)
        html += "<a class=\"link\" href=\"" + html_escape(row.uri) + "\">" + html_escape(row.title.empty() ? row.uri : row.title) + "</a>";
    html += "</div></section>";
    if (!private_mode) {
        html += "<section class=\"section\"><div class=\"heading\">RECENT</div><div class=\"links\">";
        auto recent = b->store.recent_unique(8);
        if (recent.empty()) html += "<span class=\"muted\">No browsing history yet.</span>";
        for (const auto &row : recent)
            html += "<a class=\"link\" href=\"" + html_escape(row.uri) + "\">" + html_escape(row.title.empty() ? row.uri : row.title) + "</a>";
        html += "</div></section>";
    }
    html += "</main></body></html>";
    return html;
}

static void load_home(Tab *tab) {
    std::string html = homepage_html(tab->browser, tab->private_mode);
    webkit_web_view_load_html(tab->view, html.c_str(), nullptr);
}

static void update_tab_label(Tab *tab) {
    if (!tab || !tab->title_label) return;
    std::string label = tab->private_mode ? "◉ " : "";
    label += tab->title.empty() ? "New tab" : tab->title;
    if (label.size() > 34) label = label.substr(0, 31) + "…";
    gtk_label_set_text(GTK_LABEL(tab->title_label), label.c_str());
}

static void update_chrome(Browser *b, Tab *tab) {
    if (!tab) return;
    const char *uri = webkit_web_view_get_uri(tab->view);
    // The local new-tab page is loaded via load_html() so WebKit reports about:blank; show an empty bar.
    gtk_editable_set_text(GTK_EDITABLE(b->address), (uri && g_strcmp0(uri, "about:blank") != 0) ? uri : "");
    const char *title = webkit_web_view_get_title(tab->view);
    std::string window_title = title && *title ? title : "ReedBrowser";
    if (tab->private_mode) window_title += " — Private";
    gtk_window_set_title(GTK_WINDOW(b->window), window_title.c_str());
    gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(b->toolbar), "back")),
                             webkit_web_view_can_go_back(tab->view));
    gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(b->toolbar), "forward")),
                             webkit_web_view_can_go_forward(tab->view));
}

static void save_session(Browser *b) {
    if (!b || !b->notebook) return;
    std::vector<std::string> uris;
    int active = 0;
    int current = gtk_notebook_get_current_page(GTK_NOTEBOOK(b->notebook));
    for (int i = 0; i < gtk_notebook_get_n_pages(GTK_NOTEBOOK(b->notebook)); ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(b->notebook), i);
        for (auto &tab : b->tabs) {
            if (GTK_WIDGET(tab->view) == page && !tab->private_mode) {
                const char *uri = webkit_web_view_get_uri(tab->view);
                std::string value = uri && *uri ? uri : tab->uri;
                if (value.empty() || starts_with(value, "file:") || starts_with(value, "about:")) value = "";
                uris.push_back(value);
                if (i == current) active = static_cast<int>(uris.size()) - 1;
                break;
            }
        }
    }
    b->store.save_session(uris, active);
}

static std::string safe_filename(const char *name) {
    std::string out = name && *name ? name : "download";
    for (char &c : out) if (c == '/' || c == '\\' || c == '\0') c = '_';
    if (out == "." || out == "..") out = "download";
    return out;
}

static gboolean on_download_destination(WebKitDownload *download, const char *suggested, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    std::string filename = safe_filename(suggested);
    std::string path = b->download_dir + "/" + filename;
    for (int n = 1; g_file_test(path.c_str(), G_FILE_TEST_EXISTS); ++n) {
        size_t dot = filename.find_last_of('.');
        std::string stem = dot == std::string::npos ? filename : filename.substr(0, dot);
        std::string ext = dot == std::string::npos ? "" : filename.substr(dot);
        path = b->download_dir + "/" + stem + " (" + std::to_string(n) + ")" + ext;
    }
    webkit_download_set_destination(download, path.c_str());
    b->downloads[download] = path;
    set_status(b, "Downloading " + filename);
    return TRUE;
}

static void on_download_finished(WebKitDownload *download, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    auto it = b->downloads.find(download);
    if (it != b->downloads.end()) {
        set_status(b, "Download complete: " + it->second);
        gchar *base = g_path_get_basename(it->second.c_str());
        if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(download), "reedbrowser-private")))
            b->store.add_download(base ? base : "download", it->second, "complete");
        g_free(base);
        b->downloads.erase(it);
    }
}

static void on_download_failed(WebKitDownload *download, GError *error, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    std::string message = error ? error->message : "unknown error";
    set_status(b, "Download failed: " + message);
    auto it = b->downloads.find(download);
    if (it != b->downloads.end()) {
        gchar *base = g_path_get_basename(it->second.c_str());
        if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(download), "reedbrowser-private")))
            b->store.add_download(base ? base : "download", it->second, "failed");
        g_free(base);
        b->downloads.erase(it);
    }
}

static void connect_download_session(Browser *b, WebKitNetworkSession *session, bool private_mode) {
    g_signal_connect(session, "download-started", G_CALLBACK(+[](WebKitNetworkSession *session, WebKitDownload *download, gpointer data) {
        Browser *browser = static_cast<Browser *>(data);
        g_object_set_data(G_OBJECT(download), "reedbrowser-private", g_object_get_data(G_OBJECT(session), "reedbrowser-private-session"));
        g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_destination), browser);
        g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), browser);
        g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), browser);
    }), b);
    g_object_set_data(G_OBJECT(session), "reedbrowser-private-session", GINT_TO_POINTER(private_mode));
}

static const char *default_filter_rules = R"JSON([
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?doubleclick\\.net/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?googlesyndication\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?google-analytics\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?googletagmanager\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?googletagservices\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?adservice\\.google\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?facebook\\.net/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://connect\\.facebook\\.net/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?scorecardresearch\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?hotjar\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?segment\\.io/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?mixpanel\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?amplitude\\.com/","url-filter-is-case-sensitive":false},"action":{"type":"block"}},
 {"trigger":{"url-filter":"^https?://([^/]+\\.)?clarity\\.ms/","url-filter-is-case-sensitive":false},"action":{"type":"block"}}
])JSON";

static void on_filter_compiled(GObject *source, GAsyncResult *result, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    GError *error = nullptr;
    b->content_filter = webkit_user_content_filter_store_save_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);
    if (!b->content_filter) {
        g_warning("ReedBrowser content filter could not be compiled: %s", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return;
    }
    for (auto &tab : b->tabs)
        webkit_user_content_manager_add_filter(tab->content_manager, b->content_filter);
    set_status(b, "Privacy protection enabled (starter filter + WebKit ITP).");
}

static void start_content_filter(Browser *b) {
    std::string filter_dir = b->data_dir + "/content-filters";
    g_mkdir_with_parents(filter_dir.c_str(), 0700);
    b->filter_store = webkit_user_content_filter_store_new(filter_dir.c_str());
    GBytes *source = g_bytes_new_static(default_filter_rules, strlen(default_filter_rules));
    webkit_user_content_filter_store_save(b->filter_store, "reedbrowser-default", source, nullptr,
                                          on_filter_compiled, b);
    g_bytes_unref(source);
}

static void on_title_changed(GObject *object, GParamSpec *, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    const char *title = webkit_web_view_get_title(WEBKIT_WEB_VIEW(object));
    tab->title = title && *title ? title : "New tab";
    update_tab_label(tab);
    if (active_tab(tab->browser) == tab) update_chrome(tab->browser, tab);
    if (tab->browser->ext && !tab->private_mode) tab->browser->ext->tab_updated(tab->id, "", "", tab->title);
}

static void on_uri_changed(GObject *object, GParamSpec *, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    if (active_tab(tab->browser) == tab) update_chrome(tab->browser, tab);
    const char *uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(object));
    tab->uri = uri ? uri : "";
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    Browser *b = tab->browser;
    if (event == WEBKIT_LOAD_STARTED) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->progress), 0.05);
        gtk_widget_set_visible(b->progress, TRUE);
        if (b->ext && !tab->private_mode) b->ext->tab_updated(tab->id, "loading", "", "");
    } else if (event == WEBKIT_LOAD_COMMITTED) {
        const char *uri = webkit_web_view_get_uri(view);
        tab->uri = uri ? uri : "";
        if (active_tab(b) == tab) update_chrome(b, tab);
    } else if (event == WEBKIT_LOAD_FINISHED) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->progress), 1.0);
        gtk_widget_set_visible(b->progress, FALSE);
        const char *uri = webkit_web_view_get_uri(view);
        const char *title = webkit_web_view_get_title(view);
        if (!tab->private_mode && uri && (starts_with(uri, "http://") || starts_with(uri, "https://")))
            b->store.record_visit(title ? title : uri, uri);
        tab->uri = uri ? uri : "";
        update_tab_label(tab);
        if (b->ext && !tab->private_mode) b->ext->tab_updated(tab->id, "complete", tab->uri, "");
        if (active_tab(b) == tab) update_chrome(b, tab);
        set_status(b, tab->private_mode ? "Private browsing" : "Ready");
    }
}

static void on_progress(GObject *object, GParamSpec *, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    if (active_tab(tab->browser) != tab) return;
    double progress = webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(object));
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(tab->browser->progress), std::clamp(progress, 0.0, 1.0));
}

static gboolean on_permission(WebKitWebView *, WebKitPermissionRequest *request, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    webkit_permission_request_deny(request);
    set_status(b, "Site permission denied. Change permissions support is coming later.");
    return TRUE;
}

static gboolean on_load_failed(WebKitWebView *, WebKitLoadEvent, const char *, GError *error, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    set_status(tab->browser, std::string("Page load failed: ") + (error ? error->message : "unknown error"));
    return FALSE;
}

static void on_process_terminated(WebKitWebView *, WebKitWebProcessTerminationReason reason, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    const char *why = reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" :
                      reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "exceeded its memory limit" : "was terminated";
    set_status(tab->browser, std::string("A page renderer ") + why + ". Reload the tab to continue.");
}

static WebKitWebView *create_tab(Browser *b, bool private_mode, const std::string &uri, bool background);

static WebKitWebView *on_create_view(WebKitWebView *, WebKitNavigationAction *, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    return create_tab(tab->browser, tab->private_mode, "", false);
}

static void on_switch_page(GtkNotebook *, GtkWidget *, guint, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    Tab *tab = active_tab(b);
    if (tab) {
        if (tab->needs_load) {
            tab->needs_load = false;
            std::string pending = tab->uri;
            if (!pending.empty()) webkit_web_view_load_uri(tab->view, pending.c_str());
        }
        update_chrome(b, tab);
        if (b->ext && !tab->private_mode) b->ext->tab_activated(tab->id);
    }
}

static void close_tab(Browser *b, Tab *tab) {
    if (!b || !tab) return;
    int page = gtk_notebook_page_num(GTK_NOTEBOOK(b->notebook), GTK_WIDGET(tab->view));
    if (page < 0) return;
    g_signal_handlers_disconnect_by_data(tab->view, tab);
    if (b->ext) { b->ext->detach_view(tab->view); if (!tab->private_mode) b->ext->tab_removed(tab->id); }
    gtk_notebook_remove_page(GTK_NOTEBOOK(b->notebook), page);
    auto it = std::find_if(b->tabs.begin(), b->tabs.end(), [tab](const auto &p) { return p.get() == tab; });
    if (it != b->tabs.end()) {
        if (tab->session) g_object_unref(tab->session);
        b->tabs.erase(it);
    }
    bool any_private = false;
    for (const auto &other : b->tabs) any_private |= other->private_mode;
    if (!any_private && b->private_session) { g_object_unref(b->private_session); b->private_session = nullptr; }
    if (b->tabs.empty() && !b->shutting_down) create_tab(b, false, "", false);
    save_session(b);
    update_chrome(b, active_tab(b));
}

static void on_close_tab(GtkButton *, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    close_tab(tab->browser, tab);
}

static GtkWidget *make_tab_label(Tab *tab) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    tab->title_label = gtk_label_new(tab->private_mode ? "◉ New tab" : "New tab");
    gtk_label_set_ellipsize(GTK_LABEL(tab->title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(tab->title_label, 112, -1);
    gtk_widget_set_size_request(box, 158, 32);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_add_css_class(close, "circular");
    gtk_widget_set_tooltip_text(close, "Close tab");
    gtk_widget_set_focusable(close, FALSE);
    gtk_box_append(GTK_BOX(box), tab->title_label);
    gtk_box_append(GTK_BOX(box), close);
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_tab), tab);
    tab->tab_label = box;
    return box;
}

static WebKitWebView *create_tab(Browser *b, bool private_mode, const std::string &uri, bool background) {
    auto holder = std::make_unique<Tab>();
    Tab *tab = holder.get();
    tab->browser = b;
    tab->id = b->next_tab_id++;
    tab->private_mode = private_mode;
    if (private_mode) {
        if (!b->private_session) {
            b->private_session = webkit_network_session_new_ephemeral();
            webkit_network_session_set_itp_enabled(b->private_session, TRUE);
            connect_download_session(b, b->private_session, true);
        }
        tab->session = WEBKIT_NETWORK_SESSION(g_object_ref(b->private_session));
    } else {
        tab->session = WEBKIT_NETWORK_SESSION(g_object_ref(b->normal_session));
    }

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    tab->content_manager = webkit_user_content_manager_new();
    if (b->content_filter) webkit_user_content_manager_add_filter(tab->content_manager, b->content_filter);
    tab->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "network-session", tab->session,
        "settings", settings,
        "user-content-manager", tab->content_manager,
        nullptr));
    g_object_unref(settings);
    if (b->ext) b->ext->attach_view(tab->view, tab->content_manager, tab->id, private_mode);
    g_object_unref(tab->content_manager);
    gtk_widget_set_hexpand(GTK_WIDGET(tab->view), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(tab->view), TRUE);

    g_signal_connect(tab->view, "notify::title", G_CALLBACK(on_title_changed), tab);
    g_signal_connect(tab->view, "notify::uri", G_CALLBACK(on_uri_changed), tab);
    g_signal_connect(tab->view, "notify::estimated-load-progress", G_CALLBACK(on_progress), tab);
    g_signal_connect(tab->view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->view, "load-failed", G_CALLBACK(on_load_failed), tab);
    g_signal_connect(tab->view, "permission-request", G_CALLBACK(on_permission), b);
    g_signal_connect(tab->view, "web-process-terminated", G_CALLBACK(on_process_terminated), tab);
    g_signal_connect(tab->view, "create", G_CALLBACK(on_create_view), tab);

    GtkWidget *label = make_tab_label(tab);
    int page = gtk_notebook_append_page(GTK_NOTEBOOK(b->notebook), GTK_WIDGET(tab->view), label);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(b->notebook), GTK_WIDGET(tab->view), TRUE);
    b->tabs.push_back(std::move(holder));
    if (!background) gtk_notebook_set_current_page(GTK_NOTEBOOK(b->notebook), page);
    if (b->ext && !private_mode) b->ext->tab_created(tab->id);

    if (!uri.empty() && background) {
        tab->uri = uri;
        tab->needs_load = true;
        tab->title = uri;
        update_tab_label(tab);
    } else if (!uri.empty()) webkit_web_view_load_uri(tab->view, uri.c_str());
    else load_home(tab);
    return tab->view;
}

static void navigate(Browser *b) {
    Tab *tab = active_tab(b);
    if (!tab) return;
    std::string uri = normalize_input(gtk_editable_get_text(GTK_EDITABLE(b->address)));
    if (uri.empty()) return;
    gtk_widget_grab_focus(GTK_WIDGET(tab->view));
    webkit_web_view_load_uri(tab->view, uri.c_str());
}

static void on_address_activate(GtkEntry *, gpointer data) { navigate(static_cast<Browser *>(data)); }

static void on_back(GtkButton *, gpointer data) {
    Browser *b = static_cast<Browser *>(data); if (auto *t = active_tab(b)) webkit_web_view_go_back(t->view);
}
static void on_forward(GtkButton *, gpointer data) {
    Browser *b = static_cast<Browser *>(data); if (auto *t = active_tab(b)) webkit_web_view_go_forward(t->view);
}
static void on_reload(GtkButton *, gpointer data) {
    Browser *b = static_cast<Browser *>(data); if (auto *t = active_tab(b)) webkit_web_view_reload(t->view);
}
static void on_home(GtkButton *, gpointer data) {
    Browser *b = static_cast<Browser *>(data); if (auto *t = active_tab(b)) load_home(t);
}
static void on_new_tab(GtkButton *, gpointer data) { create_tab(static_cast<Browser *>(data), false, "", false); }

static void toggle_bookmark(Browser *b) {
    Tab *tab = active_tab(b);
    if (!tab || tab->private_mode) { set_status(b, "Bookmarks are disabled in private tabs."); return; }
    const char *uri = webkit_web_view_get_uri(tab->view);
    if (!uri || !starts_with(uri, "http")) { set_status(b, "Only website pages can be bookmarked."); return; }
    if (b->store.is_bookmarked(uri)) {
        b->store.remove_bookmark_uri(uri);
        set_status(b, "Bookmark removed.");
    } else {
        const char *title = webkit_web_view_get_title(tab->view);
        b->store.add_bookmark(title ? title : uri, uri);
        set_status(b, "Bookmark saved.");
    }
}
static void on_bookmark(GtkButton *, gpointer data) { toggle_bookmark(static_cast<Browser *>(data)); }

static void show_find(Browser *b) {
    gtk_revealer_set_reveal_child(GTK_REVEALER(b->find_revealer), TRUE);
    gtk_widget_grab_focus(b->find_entry);
    gtk_editable_select_region(GTK_EDITABLE(b->find_entry), 0, -1);
}
static const WebKitFindOptions kFindOpts = static_cast<WebKitFindOptions>(WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND);

static void on_counted_matches(WebKitFindController *, guint count, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    std::string label = count == 0 ? "No matches" : std::to_string(count) + (count == 1 ? " match" : " matches");
    gtk_label_set_text(GTK_LABEL(b->find_count), label.c_str());
}
static void on_failed_to_find(WebKitFindController *, gpointer data) {
    gtk_label_set_text(GTK_LABEL(static_cast<Browser *>(data)->find_count), "No matches");
}
static void ensure_find_signals(Browser *b, WebKitFindController *find) {
    if (g_object_get_data(G_OBJECT(find), "reed-find-signals")) return;
    g_object_set_data(G_OBJECT(find), "reed-find-signals", GINT_TO_POINTER(1));
    g_signal_connect(find, "counted-matches", G_CALLBACK(on_counted_matches), b);
    g_signal_connect(find, "failed-to-find-text", G_CALLBACK(on_failed_to_find), b);
}
// Start a fresh search (called as the user types).
static void find_start(Browser *b) {
    Tab *tab = active_tab(b); if (!tab) return;
    WebKitFindController *find = webkit_web_view_get_find_controller(tab->view);
    ensure_find_signals(b, find);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(b->find_entry));
    if (!text || !*text) {
        webkit_find_controller_search_finish(find);
        gtk_label_set_text(GTK_LABEL(b->find_count), "");
        return;
    }
    webkit_find_controller_search(find, text, kFindOpts, 1000);
    webkit_find_controller_count_matches(find, text, kFindOpts, 1000);
}
// Step to the next/previous match of the current search.
static void find_step(Browser *b, bool forward) {
    Tab *tab = active_tab(b); if (!tab) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(b->find_entry));
    if (!text || !*text) return;
    WebKitFindController *find = webkit_web_view_get_find_controller(tab->view);
    ensure_find_signals(b, find);
    const char *current = webkit_find_controller_get_search_text(find);
    if (!current || g_strcmp0(current, text) != 0) { find_start(b); return; }
    if (forward) webkit_find_controller_search_next(find); else webkit_find_controller_search_previous(find);
}
static void close_find(Browser *b) {
    if (auto *t = active_tab(b)) {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(t->view));
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(b->find_revealer))) gtk_widget_grab_focus(GTK_WIDGET(t->view));
    }
    gtk_label_set_text(GTK_LABEL(b->find_count), "");
    gtk_revealer_set_reveal_child(GTK_REVEALER(b->find_revealer), FALSE);
}
static void on_find_changed(GtkEditable *, gpointer data) { find_start(static_cast<Browser *>(data)); }
static void on_find_activate(GtkEntry *, gpointer data) { find_step(static_cast<Browser *>(data), true); }
static void on_find_next(GtkButton *, gpointer data) { find_step(static_cast<Browser *>(data), true); }
static void on_find_prev(GtkButton *, gpointer data) { find_step(static_cast<Browser *>(data), false); }
static void on_find_close(GtkButton *, gpointer data) { close_find(static_cast<Browser *>(data)); }

static gboolean on_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    const bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    const bool shift = (state & GDK_SHIFT_MASK) != 0;
    const bool alt = (state & GDK_ALT_MASK) != 0;
    Tab *tab = active_tab(b);
    if (ctrl && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) { gtk_widget_grab_focus(b->address); gtk_editable_select_region(GTK_EDITABLE(b->address), 0, -1); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_t || keyval == GDK_KEY_T)) { create_tab(b, false, "", false); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_w || keyval == GDK_KEY_W)) { if (tab) close_tab(b, tab); return TRUE; }
    if (ctrl && shift && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) { create_tab(b, true, "", false); set_status(b, "Private tab: history and site data are not saved."); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_r || keyval == GDK_KEY_R)) { if (tab) webkit_web_view_reload(tab->view); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_d || keyval == GDK_KEY_D)) { toggle_bookmark(b); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) { show_find(b); return TRUE; }
    if (ctrl && shift && (keyval == GDK_KEY_Tab || keyval == GDK_KEY_Page_Up)) {
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(b->notebook)); if (n > 1) gtk_notebook_prev_page(GTK_NOTEBOOK(b->notebook)); return TRUE;
    }
    if (ctrl && (keyval == GDK_KEY_Tab || keyval == GDK_KEY_Page_Down)) {
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(b->notebook)); if (n > 1) gtk_notebook_next_page(GTK_NOTEBOOK(b->notebook)); return TRUE;
    }
    if (alt && keyval == GDK_KEY_Left) { if (tab && webkit_web_view_can_go_back(tab->view)) webkit_web_view_go_back(tab->view); return TRUE; }
    if (alt && keyval == GDK_KEY_Right) { if (tab && webkit_web_view_can_go_forward(tab->view)) webkit_web_view_go_forward(tab->view); return TRUE; }
    if (keyval == GDK_KEY_F5) { if (tab) webkit_web_view_reload(tab->view); return TRUE; }
    if (keyval == GDK_KEY_Escape) { if (tab) webkit_web_view_stop_loading(tab->view); close_find(b); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_g || keyval == GDK_KEY_G)) { find_step(b, !shift); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal || keyval == GDK_KEY_KP_Add)) { if (tab) webkit_web_view_set_zoom_level(tab->view, std::min(2.0, webkit_web_view_get_zoom_level(tab->view) + 0.1)); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_minus || keyval == GDK_KEY_KP_Subtract)) { if (tab) webkit_web_view_set_zoom_level(tab->view, std::max(0.5, webkit_web_view_get_zoom_level(tab->view) - 0.1)); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_0 || keyval == GDK_KEY_KP_0)) { if (tab) webkit_web_view_set_zoom_level(tab->view, 1.0); return TRUE; }
    return FALSE;
}

static GtkWidget *icon_button(const char *icon, const char *tooltip, GCallback callback, Browser *b) {
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_add_css_class(button, "flat");
    g_signal_connect(button, "clicked", callback, b);
    return button;
}

static void on_switch_page(GtkNotebook *, GtkWidget *, guint, gpointer data);
static gboolean on_close_request(GtkWindow *, gpointer data);
static void apply_css();

static void activate(GtkApplication *app, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    if (b->window) { gtk_window_present(GTK_WINDOW(b->window)); return; }

    apply_css();
    if (b->ext) b->ext->start();
    b->window = gtk_application_window_new(app);
    g_signal_connect(b->window, "close-request", G_CALLBACK(on_close_request), b);
    gtk_window_set_title(GTK_WINDOW(b->window), "ReedBrowser");
    gtk_window_set_default_size(GTK_WINDOW(b->window), 1120, 760);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(b->window), root);

    b->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_add_css_class(b->toolbar, "toolbar");
    gtk_widget_set_margin_start(b->toolbar, 8);
    gtk_widget_set_margin_end(b->toolbar, 8);
    gtk_widget_set_margin_top(b->toolbar, 6);
    gtk_widget_set_margin_bottom(b->toolbar, 6);
    gtk_box_append(GTK_BOX(root), b->toolbar);

    GtkWidget *back = icon_button("go-previous-symbolic", "Back", G_CALLBACK(on_back), b);
    GtkWidget *forward = icon_button("go-next-symbolic", "Forward", G_CALLBACK(on_forward), b);
    GtkWidget *reload = icon_button("view-refresh-symbolic", "Reload", G_CALLBACK(on_reload), b);
    GtkWidget *home = icon_button("go-home-symbolic", "Home", G_CALLBACK(on_home), b);
    GtkWidget *bookmark = icon_button("starred-symbolic", "Bookmark this page (Ctrl+D)", G_CALLBACK(on_bookmark), b);
    GtkWidget *newtab = icon_button("tab-new-symbolic", "New tab (Ctrl+T)", G_CALLBACK(on_new_tab), b);
    g_object_set_data(G_OBJECT(b->toolbar), "back", back);
    g_object_set_data(G_OBJECT(b->toolbar), "forward", forward);
    gtk_box_append(GTK_BOX(b->toolbar), back);
    gtk_box_append(GTK_BOX(b->toolbar), forward);
    gtk_box_append(GTK_BOX(b->toolbar), reload);
    gtk_box_append(GTK_BOX(b->toolbar), home);
    b->address = gtk_entry_new();
    gtk_widget_set_hexpand(b->address, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->address), "Search or enter address");
    gtk_widget_add_css_class(b->address, "address-entry");
    gtk_box_append(GTK_BOX(b->toolbar), b->address);
    gtk_box_append(GTK_BOX(b->toolbar), bookmark);
    gtk_box_append(GTK_BOX(b->toolbar), newtab);
    g_signal_connect(b->address, "activate", G_CALLBACK(on_address_activate), b);

    b->progress = gtk_progress_bar_new();
    gtk_widget_set_visible(b->progress, FALSE);
    gtk_widget_set_size_request(b->progress, -1, 2);
    gtk_box_append(GTK_BOX(root), b->progress);

    b->notebook = gtk_notebook_new();
    gtk_widget_add_css_class(b->notebook, "notebook");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(b->notebook), TRUE);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(b->notebook), GTK_POS_LEFT);
    gtk_widget_set_hexpand(b->notebook, TRUE);
    gtk_widget_set_vexpand(b->notebook, TRUE);
    gtk_box_append(GTK_BOX(root), b->notebook);
    g_signal_connect(b->notebook, "switch-page", G_CALLBACK(on_switch_page), b);

    b->find_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(b->find_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget *find_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(find_box, 10); gtk_widget_set_margin_end(find_box, 10);
    gtk_widget_set_margin_top(find_box, 5); gtk_widget_set_margin_bottom(find_box, 5);
    b->find_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->find_entry), "Find in page");
    gtk_widget_set_size_request(b->find_entry, 260, -1);
    b->find_count = gtk_label_new("");
    GtkWidget *find_prev = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(find_prev, "Previous match (Ctrl+Shift+G)");
    GtkWidget *find_next = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(find_next, "Next match (Enter / Ctrl+G)");
    GtkWidget *find_close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_box_append(GTK_BOX(find_box), b->find_entry); gtk_box_append(GTK_BOX(find_box), b->find_count);
    gtk_box_append(GTK_BOX(find_box), find_prev); gtk_box_append(GTK_BOX(find_box), find_next); gtk_box_append(GTK_BOX(find_box), find_close);
    gtk_revealer_set_child(GTK_REVEALER(b->find_revealer), find_box);
    gtk_box_append(GTK_BOX(root), b->find_revealer);
    gtk_revealer_set_reveal_child(GTK_REVEALER(b->find_revealer), FALSE);
    g_signal_connect(b->find_entry, "activate", G_CALLBACK(on_find_activate), b);
    g_signal_connect(b->find_entry, "changed", G_CALLBACK(on_find_changed), b);
    g_signal_connect(find_prev, "clicked", G_CALLBACK(on_find_prev), b);
    g_signal_connect(find_next, "clicked", G_CALLBACK(on_find_next), b);
    g_signal_connect(find_close, "clicked", G_CALLBACK(on_find_close), b);

    b->status = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(b->status), 0.0f);
    gtk_widget_set_margin_start(b->status, 10); gtk_widget_set_margin_end(b->status, 10);
    gtk_widget_set_margin_top(b->status, 4); gtk_widget_set_margin_bottom(b->status, 4);
    gtk_widget_add_css_class(b->status, "dim-label");
    gtk_box_append(GTK_BOX(root), b->status);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), b);
    gtk_widget_add_controller(b->window, keys);

    int restore_active = 0;
    auto restore = b->store.load_session(&restore_active);
    if (restore.empty()) create_tab(b, false, "", false);
    else {
        for (const auto &uri : restore) create_tab(b, false, uri, true);
        int count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(b->notebook));
        gtk_notebook_set_current_page(GTK_NOTEBOOK(b->notebook), std::clamp(restore_active, 0, std::max(0, count - 1)));
        if (auto *restored = active_tab(b); restored && restored->needs_load) {
            restored->needs_load = false;
            std::string pending = restored->uri;
            if (!pending.empty()) webkit_web_view_load_uri(restored->view, pending.c_str());
        }
    }
    gtk_window_present(GTK_WINDOW(b->window));
}

static gboolean on_close_request(GtkWindow *, gpointer data) {
    save_session(static_cast<Browser *>(data));
    return FALSE;
}

static void on_open_files(GApplication *application, GFile **files, gint count, const gchar *, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    activate(GTK_APPLICATION(application), data);
    for (gint i = 0; i < count; ++i) {
        gchar *uri = g_file_get_uri(files[i]);
        if (uri && *uri) {
            if (i == 0) {
                if (auto *tab = active_tab(b)) webkit_web_view_load_uri(tab->view, uri);
            } else {
                create_tab(b, false, uri, false);
            }
        }
        g_free(uri);
    }
}

static void shutdown_app(GApplication *, gpointer data) {
    Browser *b = static_cast<Browser *>(data);
    b->shutting_down = true;
}

static void apply_css() {
    const char *css = R"CSS(.toolbar{background:alpha(@theme_bg_color,.94);border-bottom:1px solid alpha(@theme_fg_color,.10);}.address-entry{border-radius:10px;min-height:34px;padding-left:12px;padding-right:12px;}.notebook tab{padding:5px 8px;}.notebook tab label{font-size:0.92em;})CSS";
    GtkCssProvider *provider = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, css);
#else
    gtk_css_provider_load_from_data(provider, css, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

} // namespace

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-V") { printf("ReedBrowser %s\n", REEDBROWSER_VERSION); return 0; }
        if (arg == "--help" || arg == "-h") {
            puts("Usage: reedbrowser [URL...]\n"
                 "       reedbrowser --install-extension <file.xpi|.zip|folder> [--yes]\n"
                 "       reedbrowser --list-extensions\n"
                 "       reedbrowser --remove-extension <id-or-name>\n"
                 "       reedbrowser --version");
            return 0;
        }
    }
    Browser browser;
    browser.data_dir = home_path(".local/share/reedbrowser");
    browser.cache_dir = home_path(".cache/reedbrowser");
    g_mkdir_with_parents(browser.data_dir.c_str(), 0700);
    g_mkdir_with_parents(browser.cache_dir.c_str(), 0700);
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    browser.download_dir = downloads && *downloads ? downloads : home_path("Downloads");
    g_mkdir_with_parents(browser.download_dir.c_str(), 0700);

    // Extension management from the command line (no window needed).
    {
        bool yes = false;
        std::string action, arg;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--yes" || a == "-y") yes = true;
            else if ((a == "--install-extension" || a == "--remove-extension") && i + 1 < argc) { action = a; arg = argv[++i]; }
            else if (a == "--list-extensions") action = a;
        }
        if (!action.empty()) {
            ExtensionManager mgr(browser.data_dir, nullptr, {});
            std::string msg;
            if (action == "--list-extensions") { fputs(mgr.describe().c_str(), stdout); return 0; }
            bool ok = action == "--install-extension" ? mgr.install(arg, yes, &msg) : mgr.remove(arg, &msg);
            fprintf(ok ? stdout : stderr, "%s\n", msg.c_str());
            return ok ? 0 : 1;
        }
    }

    std::string db_path = browser.data_dir + "/browser.sqlite3";
    if (!browser.store.open(db_path)) g_warning("Could not open ReedBrowser database: %s", db_path.c_str());

    browser.normal_session = webkit_network_session_new(browser.data_dir.c_str(), browser.cache_dir.c_str());
    webkit_network_session_set_itp_enabled(browser.normal_session, TRUE);
    connect_download_session(&browser, browser.normal_session, false);
    start_content_filter(&browser);

    {
        Browser *bp = &browser;
        ExtensionManager::Host host;
        host.list_tabs = [bp]() {
            std::vector<ExtensionManager::TabInfo> out;
            Tab *cur = active_tab(bp);
            for (auto &t : bp->tabs) {
                if (t->private_mode) continue;
                out.push_back({t->id, t->uri, t->title, t.get() == cur});
            }
            return out;
        };
        host.open_tab = [bp](const std::string &url, bool active) {
            WebKitWebView *v = create_tab(bp, false, url, !active);
            for (auto &t : bp->tabs) if (t->view == v) return t->id;
            return -1;
        };
        host.close_tab = [bp](int id) {
            for (auto &t : bp->tabs) if (t->id == id) { close_tab(bp, t.get()); return; }
        };
        host.activate_tab = [bp](int id) {
            for (auto &t : bp->tabs) if (t->id == id) {
                int page = gtk_notebook_page_num(GTK_NOTEBOOK(bp->notebook), GTK_WIDGET(t->view));
                if (page >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(bp->notebook), page);
                return;
            }
        };
        browser.ext = std::make_unique<ExtensionManager>(browser.data_dir, browser.normal_session, std::move(host));
    }

    GtkApplication *app = gtk_application_new("org.reedbrowser.Browser", G_APPLICATION_HANDLES_OPEN);
    browser.app = app;
    g_signal_connect(app, "activate", G_CALLBACK(activate), &browser);
    g_signal_connect(app, "open", G_CALLBACK(on_open_files), &browser);
    g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_app), &browser);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    for (auto &tab : browser.tabs) {
        if (tab->session) g_object_unref(tab->session);
    }
    browser.ext.reset();
    if (browser.content_filter) g_object_unref(browser.content_filter);
    if (browser.filter_store) g_object_unref(browser.filter_store);
    if (browser.private_session) g_object_unref(browser.private_session);
    if (browser.normal_session) g_object_unref(browser.normal_session);
    g_object_unref(app);
    return status;
}
