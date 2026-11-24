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
#include <utility>

#include "store.hpp"
#include "extensions.hpp"
#include "importer.hpp"

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
    std::string theme{"system"};
    std::string accent{"#3584e4"};
    std::string density{"comfortable"};
    std::string tab_position{"left"};
    int ui_font_size{100};
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
    GtkWidget *manager_win{};   // bookmark manager window, if open
    GtkWidget *ext_box{};
    GtkWidget *bookmark_btn{}, *bookmark_bar{};
    std::vector<std::string> closed_uris;       // toolbar container for extension buttons
    std::string home_token;     // per-session secret so web pages cannot forge reed:// actions
};

// Forward declarations for features defined after the settings UI.
static void bookmarks_changed(Browser *b);
static void show_bookmark_manager(Browser *b);
static void show_bookmark_editor(Browser *b, long long id);
static void show_import_wizard(Browser *b);
static void refresh_extension_buttons(Browser *b);
static void rebuild_bookmark_bar(Browser *b);
static void toggle_bookmark_bar(Browser *b);
static void show_downloads(Browser *b);
static void reopen_closed_tab(Browser *b);
static void open_extension_options(Browser *b, const std::string &uuid);
static void show_extension_manager(Browser *b);
static void start_extension_install(Browser *b, WebKitWebView *origin, const std::string &source, int install_id);
static void attach_amo_support(Browser *b, WebKitWebView *view, WebKitUserContentManager *ucm);
static gboolean on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer data);

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
    html += "<style>:root{--reed-accent:" + b->accent + ";} .brand{color:var(--reed-accent)}.search:focus-within{border-color:var(--reed-accent);box-shadow:0 0 0 3px color-mix(in srgb,var(--reed-accent) 22%,transparent)}.link:hover{border-color:var(--reed-accent)!important}</style>";
    if (b->theme == "dark") html += "<style>:root{color-scheme:dark}body{background:#17191e!important;color:#e7e9ed!important}.search,.link{background:#22252c!important;border-color:#343943!important;color:#e7e9ed!important}.sub,.heading,.muted{color:#9ba3b0!important}</style>";
    else if (b->theme == "light") html += "<style>:root{color-scheme:light}body{background:#f5f6f8!important;color:#20242b!important}.search,.link{background:white!important;color:#20242b!important}</style>";
    {
        const std::string t = "?t=" + b->home_token;
        html += "<style>.tile{display:inline-flex;align-items:center;gap:2px;margin:0 8px 8px 0}.tile .link{margin:0}"
                ".tile .act{font-size:12px;text-decoration:none;color:inherit;opacity:.35;padding:4px 5px;border-radius:6px}"
                ".tile:hover .act{opacity:.9}.tile .act:hover{background:rgba(127,127,127,.25)}.tile .rm:hover{color:#e5484d}"
                ".folder{margin:14px 0 8px;font-size:12px;opacity:.7}.hl{font-size:12px;margin-left:12px;text-decoration:none;color:var(--reed-accent)}</style>";
        html += "<section class=\"section\"><div class=\"heading\">BOOKMARKS<a class=\"hl\" href=\"reed://manage" + t + "\">Manage</a>"
                "<a class=\"hl\" href=\"reed://import" + t + "\">Import</a></div>";
        auto bookmarks = b->store.bookmarks_full();
        if (bookmarks.empty()) html += "<div class=\"links\"><span class=\"muted\">Your saved sites will appear here. Press Ctrl+D on a page to save it, or use Import.</span></div>";
        std::string current = "\x01";
        bool open = false;
        for (const auto &row : bookmarks) {
            if (row.folder != current) {
                if (open) html += "</div>";
                current = row.folder;
                if (!current.empty()) html += "<div class=\"folder\">&#128193; " + html_escape(current) + "</div>";
                html += "<div class=\"links\">"; open = true;
            }
            std::string id = std::to_string(row.id);
            html += "<span class=\"tile\"><a class=\"link\" href=\"" + html_escape(row.uri) + "\">" + html_escape(row.title.empty() ? row.uri : row.title) + "</a>"
                    "<a class=\"act\" title=\"Edit bookmark\" href=\"reed://edit/" + id + t + "\">&#9998;</a>"
                    "<a class=\"act rm\" title=\"Remove bookmark\" href=\"reed://remove/" + id + t + "\">&#10005;</a></span>";
        }
        if (open) html += "</div>";
        html += "</section>";
    }
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
    if (b->bookmark_btn) {
        bool marked = !tab->private_mode && uri && g_str_has_prefix(uri, "http") && b->store.is_bookmarked(uri);
        if (marked) gtk_widget_add_css_class(b->bookmark_btn, "bookmarked"); else gtk_widget_remove_css_class(b->bookmark_btn, "bookmarked");
    }
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
        if (b->ext && !tab->private_mode && uri) b->ext->tab_committed(tab->id, uri);
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
    if (!tab->private_mode && !b->shutting_down) {
        const char *cu = webkit_web_view_get_uri(tab->view);
        if (cu && (g_str_has_prefix(cu, "http://") || g_str_has_prefix(cu, "https://"))) { b->closed_uris.push_back(cu); if (b->closed_uris.size() > 25) b->closed_uris.erase(b->closed_uris.begin()); }
    }
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
    if (!private_mode) attach_amo_support(b, tab->view, tab->content_manager);
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
    g_signal_connect(tab->view, "decide-policy", G_CALLBACK(on_decide_policy), tab);

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
    bookmarks_changed(b);
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
    if (ctrl && shift && (keyval == GDK_KEY_t || keyval == GDK_KEY_T)) { reopen_closed_tab(b); return TRUE; }
    if (ctrl && shift && (keyval == GDK_KEY_b || keyval == GDK_KEY_B)) { toggle_bookmark_bar(b); return TRUE; }
    if (ctrl && !shift && (keyval == GDK_KEY_j || keyval == GDK_KEY_J)) { show_downloads(b); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_t || keyval == GDK_KEY_T)) { create_tab(b, false, "", false); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_w || keyval == GDK_KEY_W)) { if (tab) close_tab(b, tab); return TRUE; }
    if (ctrl && shift && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) { create_tab(b, true, "", false); set_status(b, "Private tab: history and site data are not saved."); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_r || keyval == GDK_KEY_R)) { if (tab) webkit_web_view_reload(tab->view); return TRUE; }
    if (ctrl && (keyval == GDK_KEY_d || keyval == GDK_KEY_D)) { toggle_bookmark(b); return TRUE; }
    if (ctrl && shift && (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) { show_bookmark_manager(b); return TRUE; }
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


static void save_preferences(Browser *b) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "appearance", "theme", b->theme.c_str());
    g_key_file_set_string(kf, "appearance", "accent", b->accent.c_str());
    g_key_file_set_string(kf, "appearance", "density", b->density.c_str());
    g_key_file_set_integer(kf, "appearance", "ui-font-size", b->ui_font_size);
    g_key_file_set_string(kf, "tabs", "position", b->tab_position.c_str());
    gsize len = 0; gchar *data = g_key_file_to_data(kf, &len, nullptr);
    std::string path = b->data_dir + "/preferences.ini";
    g_file_set_contents(path.c_str(), data, static_cast<gssize>(len), nullptr);
    g_free(data); g_key_file_unref(kf);
}
static bool valid_hex_color(const std::string &v) {
    if (v.size() != 7 || v[0] != '#') return false;
    return std::all_of(v.begin() + 1, v.end(), [](unsigned char c){ return std::isxdigit(c) != 0; });
}
static void apply_custom_css(Browser *b) {
    if (!gdk_display_get_default()) return;
    char *css = g_strdup_printf(
        "button.suggested-action, button:checked { background:%s; color:white; }"
        ".toolbar button:hover, .notebook tab:checked { border-color:%s; }"
        ".address-entry:focus-within { border-color:%s; box-shadow:0 0 0 2px alpha(%s,.22); }"
        ".toolbar { font-size:%d%%; }",
        b->accent.c_str(), b->accent.c_str(), b->accent.c_str(), b->accent.c_str(), b->ui_font_size);
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(provider); g_free(css);
}
static void set_theme(Browser *b, const std::string &theme) {
    b->theme = theme == "dark" || theme == "light" ? theme : "system";
    GtkSettings *settings = gtk_settings_get_default();
    if (settings && g_object_class_find_property(G_OBJECT_GET_CLASS(settings), "gtk-application-prefer-dark-theme"))
        g_object_set(settings, "gtk-application-prefer-dark-theme", b->theme == "dark", nullptr);
    for (auto &tab : b->tabs) {
        const char *uri = webkit_web_view_get_uri(tab->view);
        if (!uri || g_strcmp0(uri, "about:blank") == 0) load_home(tab.get());
    }
    save_preferences(b);
    set_status(b, b->theme == "dark" ? "Dark mode enabled." : b->theme == "light" ? "Light mode enabled." : "Following system appearance.");
}
static void load_theme(Browser *b) {
    GKeyFile *kf = g_key_file_new();
    std::string path = b->data_dir + "/preferences.ini";
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        gchar *v = g_key_file_get_string(kf, "appearance", "theme", nullptr); if (v) { b->theme = v; g_free(v); }
        v = g_key_file_get_string(kf, "appearance", "accent", nullptr); if (v) { if (valid_hex_color(v)) b->accent = v; g_free(v); }
        v = g_key_file_get_string(kf, "appearance", "density", nullptr); if (v) { b->density = v; g_free(v); }
        v = g_key_file_get_string(kf, "tabs", "position", nullptr); if (v) { b->tab_position = v; g_free(v); }
        GError *err = nullptr; int size = g_key_file_get_integer(kf, "appearance", "ui-font-size", &err); if (!err && size >= 80 && size <= 140) b->ui_font_size = size; if (err) g_error_free(err);
    }
    g_key_file_unref(kf);
    if (b->theme != "dark" && b->theme != "light") b->theme = "system";
    if (b->density != "compact" && b->density != "comfortable" && b->density != "spacious") b->density = "comfortable";
    if (b->tab_position != "top" && b->tab_position != "bottom" && b->tab_position != "left" && b->tab_position != "right") b->tab_position = "left";
    GtkSettings *settings = gtk_settings_get_default();
    if (settings && g_object_class_find_property(G_OBJECT_GET_CLASS(settings), "gtk-application-prefer-dark-theme"))
        g_object_set(settings, "gtk-application-prefer-dark-theme", b->theme == "dark", nullptr);
}
static void apply_appearance(Browser *b) {
    apply_custom_css(b);
    if (b->notebook) {
        GtkPositionType pos = b->tab_position == "top" ? GTK_POS_TOP : b->tab_position == "bottom" ? GTK_POS_BOTTOM : b->tab_position == "right" ? GTK_POS_RIGHT : GTK_POS_LEFT;
        gtk_notebook_set_tab_pos(GTK_NOTEBOOK(b->notebook), pos);
    }
    if (b->toolbar) {
        const char *klass = b->density == "compact" ? "compact-ui" : b->density == "spacious" ? "spacious-ui" : "comfortable-ui";
        gtk_widget_remove_css_class(b->toolbar, "compact-ui"); gtk_widget_remove_css_class(b->toolbar, "spacious-ui"); gtk_widget_remove_css_class(b->toolbar, "comfortable-ui");
        gtk_widget_add_css_class(b->toolbar, klass);
    }
    save_preferences(b);
}
static void extension_message(Browser *b, const std::string &title, const std::string &message) {
    GtkWidget *win = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE); gtk_window_set_title(GTK_WINDOW(win), title.c_str()); gtk_window_set_default_size(GTK_WINDOW(win), 420, 150);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    GtkWidget *label = gtk_label_new(message.c_str()); gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_box_append(GTK_BOX(box), label);
    GtkWidget *button = gtk_button_new_with_label("Close"); gtk_widget_set_halign(button, GTK_ALIGN_END); gtk_box_append(GTK_BOX(box), button);
    g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_window_destroy), win);
    gtk_window_set_child(GTK_WINDOW(win), box); gtk_window_present(GTK_WINDOW(win));
}
struct ExtAction { Browser *browser; GtkWidget *entry; };
struct InstallConfirm { Browser *browser; std::string source; GtkWidget *window; };
static void confirm_extension_install(GtkButton *, gpointer data) {
    auto *c = static_cast<InstallConfirm *>(data);
    Browser *browser = c->browser; GtkWidget *window = c->window; std::string source = c->source;
    std::string msg; bool ok = browser->ext && browser->ext->install(source, true, &msg);
    if (ok) browser->ext->start_new();
    gtk_window_destroy(GTK_WINDOW(window));
    extension_message(browser, ok ? "Extension installed" : "Installation failed", msg + (ok ? "\n\nRestart ReedBrowser to load it." : ""));
}
static void install_extension_clicked(GtkButton *, gpointer data) {
    auto *a = static_cast<ExtAction *>(data); const char *v = gtk_editable_get_text(GTK_EDITABLE(a->entry));
    if (!v || !*v) { extension_message(a->browser, "Install extension", "Paste an addons.mozilla.org URL or a local XPI/ZIP path."); return; }
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), a->browser->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(a->browser->window)); gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_title(GTK_WINDOW(win), "Confirm extension installation"); gtk_window_set_default_size(GTK_WINDOW(win), 470, 210);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    GtkWidget *label = gtk_label_new("Install this extension? ReedBrowser will grant the permissions declared in its manifest. Per-site permission prompts are not implemented yet. Only install extensions you trust.");
    gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_widget_set_halign(label, GTK_ALIGN_START); gtk_box_append(GTK_BOX(box), label);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel"); GtkWidget *install = gtk_button_new_with_label("Install and grant permissions");
    gtk_box_append(GTK_BOX(row), cancel); gtk_box_append(GTK_BOX(row), install); gtk_box_append(GTK_BOX(box), row);
    auto *confirm = new InstallConfirm{a->browser, v, win};
    g_signal_connect_data(install, "clicked", G_CALLBACK(confirm_extension_install), confirm, +[](gpointer p, GClosure*){ delete static_cast<InstallConfirm*>(p); }, GConnectFlags(0));
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    gtk_window_set_child(GTK_WINDOW(win), box); gtk_window_present(GTK_WINDOW(win));
}
static void remove_extension_clicked(GtkButton *, gpointer data) {
    auto *a = static_cast<ExtAction *>(data); const char *v = gtk_editable_get_text(GTK_EDITABLE(a->entry));
    if (!v || !*v) { extension_message(a->browser, "Remove extension", "Enter the extension name or ID shown in the installed list."); return; }
    std::string msg; bool ok = a->browser->ext && a->browser->ext->remove(v, &msg);
    extension_message(a->browser, ok ? "Extension removed" : "Removal failed", msg);
}
static GtkWidget *settings_section(const char *title, const char *description, GtkWidget *parent) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *label = gtk_label_new(title); gtk_widget_add_css_class(label, "title-3"); gtk_widget_set_halign(label, GTK_ALIGN_START); gtk_box_append(GTK_BOX(box), label);
    GtkWidget *desc = gtk_label_new(description); gtk_label_set_wrap(GTK_LABEL(desc), TRUE); gtk_widget_add_css_class(desc, "dim-label"); gtk_widget_set_halign(desc, GTK_ALIGN_START); gtk_box_append(GTK_BOX(box), desc);
    gtk_box_append(GTK_BOX(parent), box); return box;
}
struct AccentApplyData { Browser *browser; GtkWidget *entry; };
static void show_settings(Browser *b) {
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->window)); gtk_window_set_title(GTK_WINDOW(win), "ReedBrowser Settings"); gtk_window_set_default_size(GTK_WINDOW(win), 860, 650);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10); gtk_widget_set_margin_start(header, 20); gtk_widget_set_margin_end(header, 20); gtk_widget_set_margin_top(header, 16); gtk_widget_set_margin_bottom(header, 14);
    GtkWidget *title = gtk_label_new("Settings"); gtk_widget_add_css_class(title, "title-1"); gtk_widget_set_hexpand(title, TRUE); gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *close = gtk_button_new_with_label("Done"); gtk_box_append(GTK_BOX(header), title); gtk_box_append(GTK_BOX(header), close); gtk_box_append(GTK_BOX(root), header); g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), win);
    GtkWidget *search = gtk_search_entry_new(); gtk_widget_set_margin_start(search, 20); gtk_widget_set_margin_end(search, 20); gtk_widget_set_margin_bottom(search, 12); gtk_box_append(GTK_BOX(root), search);
    GtkWidget *pages = gtk_notebook_new(); gtk_notebook_set_tab_pos(GTK_NOTEBOOK(pages), GTK_POS_LEFT); gtk_widget_set_vexpand(pages, TRUE); gtk_box_append(GTK_BOX(root), pages);
    auto page = [&](const char *name) { GtkWidget *sc = gtk_scrolled_window_new(); GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18); gtk_widget_set_margin_start(box, 24); gtk_widget_set_margin_end(box, 24); gtk_widget_set_margin_top(box, 22); gtk_widget_set_margin_bottom(box, 22); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), box); gtk_notebook_append_page(GTK_NOTEBOOK(pages), sc, gtk_label_new(name)); return box; };
    GtkWidget *appearance = page("Appearance");
    settings_section("Color theme", "Choose a browser theme independently of your desktop.", appearance);
    const char *theme_options[] = {"System default", "Light", "Dark", nullptr};
    GtkWidget *theme = gtk_drop_down_new_from_strings(theme_options);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(theme), b->theme == "dark" ? 2 : b->theme == "light" ? 1 : 0); gtk_widget_set_halign(theme, GTK_ALIGN_START); gtk_widget_set_size_request(theme, 240, -1); gtk_box_append(GTK_BOX(appearance), theme);
    g_signal_connect(theme, "notify::selected", G_CALLBACK(+[](GObject *o, GParamSpec*, gpointer d){ guint v=gtk_drop_down_get_selected(GTK_DROP_DOWN(o)); set_theme(static_cast<Browser*>(d), v==2?"dark":v==1?"light":"system"); }), b);
    settings_section("Accent color", "Choose a preset or enter any HEX color such as #8b5cf6.", appearance);
    GtkWidget *presets = gtk_flow_box_new(); gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(presets), GTK_SELECTION_NONE); gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(presets), 8); gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(presets), 8); gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(presets), 8);
    const char *colors[] = {"#3584e4","#e66100","#26a269","#9141ac","#c01c28","#1c71d8","#986a44","#5e5c64","#d33682","#00a6a6"};
    for (const char *color : colors) { GtkWidget *swatch=gtk_button_new_with_label("●"); gtk_widget_set_size_request(swatch,42,38); char *css=g_strdup_printf("button{color:%s;font-size:22px;border:2px solid %s;}",color,color); GtkCssProvider *cp=gtk_css_provider_new(); gtk_css_provider_load_from_data(cp,css,-1); gtk_style_context_add_provider(gtk_widget_get_style_context(swatch),GTK_STYLE_PROVIDER(cp),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(cp); g_free(css); g_object_set_data_full(G_OBJECT(swatch),"accent",g_strdup(color),g_free); g_signal_connect(swatch,"clicked",G_CALLBACK(+[](GtkButton *bt,gpointer d){Browser *br=static_cast<Browser*>(d); br->accent=static_cast<const char*>(g_object_get_data(G_OBJECT(bt),"accent")); apply_appearance(br);}),b); gtk_flow_box_append(GTK_FLOW_BOX(presets),swatch); }
    gtk_box_append(GTK_BOX(appearance), presets);
    GtkWidget *custom_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); GtkWidget *hex=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(hex),b->accent.c_str()); gtk_editable_set_text(GTK_EDITABLE(hex),b->accent.c_str()); gtk_widget_set_hexpand(hex,TRUE); GtkWidget *apply=gtk_button_new_with_label("Apply custom color"); gtk_box_append(GTK_BOX(custom_row),hex); gtk_box_append(GTK_BOX(custom_row),apply); gtk_box_append(GTK_BOX(appearance),custom_row);
    auto *hexdata=new AccentApplyData{b,hex}; g_signal_connect_data(apply,"clicked",G_CALLBACK(+[](GtkButton*,gpointer p){auto *v=static_cast<AccentApplyData*>(p); const char *c=gtk_editable_get_text(GTK_EDITABLE(v->entry)); std::string color=c?c:""; if (!valid_hex_color(color)){extension_message(v->browser,"Invalid accent color","Enter a six-digit HEX color, for example #3584e4.");return;} v->browser->accent=color; apply_appearance(v->browser);}),hexdata,+[](gpointer p,GClosure*){delete static_cast<AccentApplyData*>(p);},GConnectFlags(0));
    settings_section("Interface density", "Adjust spacing throughout the browser toolbar.", appearance);
    const char *density_options[] = {"Compact","Comfortable","Spacious",nullptr}; GtkWidget *density=gtk_drop_down_new_from_strings(density_options); gtk_drop_down_set_selected(GTK_DROP_DOWN(density),b->density=="compact"?0:b->density=="spacious"?2:1); gtk_widget_set_halign(density,GTK_ALIGN_START); gtk_widget_set_size_request(density,240,-1); gtk_box_append(GTK_BOX(appearance),density);
    g_signal_connect(density,"notify::selected",G_CALLBACK(+[](GObject*o,GParamSpec*,gpointer d){Browser*br=static_cast<Browser*>(d);guint v=gtk_drop_down_get_selected(GTK_DROP_DOWN(o));br->density=v==0?"compact":v==2?"spacious":"comfortable";apply_appearance(br);}),b);
    settings_section("UI text size", "Scale labels and controls in the browser interface.", appearance);
    GtkWidget *font_scale=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,80,140,5); gtk_range_set_value(GTK_RANGE(font_scale),b->ui_font_size); gtk_scale_set_draw_value(GTK_SCALE(font_scale),TRUE); gtk_widget_set_size_request(font_scale,300,-1); gtk_widget_set_halign(font_scale,GTK_ALIGN_START); gtk_box_append(GTK_BOX(appearance),font_scale);
    g_signal_connect(font_scale,"value-changed",G_CALLBACK(+[](GtkRange*r,gpointer d){Browser*br=static_cast<Browser*>(d);br->ui_font_size=static_cast<int>(gtk_range_get_value(r));apply_appearance(br);}),b);
    settings_section("Tab placement", "Choose where your tabs appear.", appearance);
    const char *tab_options[] = {"Left","Top","Right","Bottom",nullptr}; GtkWidget *tabs=gtk_drop_down_new_from_strings(tab_options); gtk_drop_down_set_selected(GTK_DROP_DOWN(tabs),b->tab_position=="top"?1:b->tab_position=="right"?2:b->tab_position=="bottom"?3:0); gtk_widget_set_halign(tabs,GTK_ALIGN_START); gtk_widget_set_size_request(tabs,240,-1); gtk_box_append(GTK_BOX(appearance),tabs);
    g_signal_connect(tabs,"notify::selected",G_CALLBACK(+[](GObject*o,GParamSpec*,gpointer d){Browser*br=static_cast<Browser*>(d);guint v=gtk_drop_down_get_selected(GTK_DROP_DOWN(o));br->tab_position=v==1?"top":v==2?"right":v==3?"bottom":"left";apply_appearance(br);}),b);
    GtkWidget *exts = page("Extensions");
    GtkWidget *et = gtk_label_new("Installed extensions"); gtk_widget_add_css_class(et,"title-2"); gtk_widget_set_halign(et,GTK_ALIGN_START); gtk_box_append(GTK_BOX(exts),et);
    GtkWidget *scroll=gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll,TRUE); gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),170); GtkWidget *text=gtk_text_view_new(); gtk_text_view_set_editable(GTK_TEXT_VIEW(text),FALSE); gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text),FALSE); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text),GTK_WRAP_WORD_CHAR); std::string list=b->ext?b->ext->describe():"Extension manager unavailable."; gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)),list.c_str(),-1); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),text); gtk_box_append(GTK_BOX(exts),scroll);
    GtkWidget *il=gtk_label_new("Install from Firefox Add-ons or a local package"); gtk_widget_set_halign(il,GTK_ALIGN_START); gtk_box_append(GTK_BOX(exts),il); GtkWidget *ir=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); GtkWidget *ie=gtk_entry_new(); gtk_widget_set_hexpand(ie,TRUE); gtk_entry_set_placeholder_text(GTK_ENTRY(ie),"AMO URL or local .xpi/.zip path"); GtkWidget *ib=gtk_button_new_with_label("Install"); auto *ia=new ExtAction{b,ie}; g_signal_connect_data(ib,"clicked",G_CALLBACK(install_extension_clicked),ia,+[](gpointer p,GClosure*){delete static_cast<ExtAction*>(p);},GConnectFlags(0)); gtk_box_append(GTK_BOX(ir),ie);gtk_box_append(GTK_BOX(ir),ib);gtk_box_append(GTK_BOX(exts),ir);
    GtkWidget *rr=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); GtkWidget *re=gtk_entry_new(); gtk_widget_set_hexpand(re,TRUE); gtk_entry_set_placeholder_text(GTK_ENTRY(re),"Extension name or ID to remove"); GtkWidget *rb=gtk_button_new_with_label("Remove"); auto *ra=new ExtAction{b,re};g_signal_connect_data(rb,"clicked",G_CALLBACK(remove_extension_clicked),ra,+[](gpointer p,GClosure*){delete static_cast<ExtAction*>(p);},GConnectFlags(0));gtk_box_append(GTK_BOX(rr),re);gtk_box_append(GTK_BOX(rr),rb);gtk_box_append(GTK_BOX(exts),rr);
    GtkWidget *warning=gtk_label_new("Installed extensions load after restarting. Firefox API compatibility is still evolving.");gtk_label_set_wrap(GTK_LABEL(warning),TRUE);gtk_widget_add_css_class(warning,"dim-label");gtk_widget_set_halign(warning,GTK_ALIGN_START);gtk_box_append(GTK_BOX(exts),warning);
    GtkWidget *privacy=page("Privacy"); settings_section("Privacy & security", "Privacy controls will be expanded as the underlying WebKit controls are wired up.",privacy);
    GtkWidget *about=page("About"); GtkWidget *abt=gtk_label_new("ReedBrowser");gtk_widget_add_css_class(abt,"title-1");gtk_widget_set_halign(abt,GTK_ALIGN_START);gtk_box_append(GTK_BOX(about),abt);GtkWidget *desc=gtk_label_new("A native GTK4 + WebKitGTK browser.");gtk_widget_set_halign(desc,GTK_ALIGN_START);gtk_box_append(GTK_BOX(about),desc);
    // Settings search filters navigation pages by their visible page names.
    g_signal_connect(search,"search-changed",G_CALLBACK(+[](GtkSearchEntry *entry,gpointer d){GtkNotebook *nb=GTK_NOTEBOOK(d);const char*q=gtk_editable_get_text(GTK_EDITABLE(entry));std::string query=q?q:"";std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return std::tolower(c);});if(query.empty())return;for(int i=0;i<gtk_notebook_get_n_pages(nb);++i){GtkWidget *tab=gtk_notebook_get_tab_label(nb,gtk_notebook_get_nth_page(nb,i));const char*t=GTK_IS_LABEL(tab)?gtk_label_get_text(GTK_LABEL(tab)):"";std::string name=t?t:"";std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return std::tolower(c);});if(name.find(query)!=std::string::npos){gtk_notebook_set_current_page(nb,i);break;}}}),pages);
    gtk_window_set_child(GTK_WINDOW(win),root); gtk_window_present(GTK_WINDOW(win));
}
// ======================================================================= bookmarks UI
static void refresh_home_tabs(Browser *b) {
    for (auto &tab : b->tabs) {
        const char *uri = webkit_web_view_get_uri(tab->view);
        if (!uri || g_strcmp0(uri, "about:blank") == 0) load_home(tab.get());
    }
}

struct ManagerCtx { Browser *browser; GtkWidget *list; GtkWidget *search; GtkWidget *count; };
struct RowCtx { Browser *browser; long long id; };
struct EditCtx { Browser *browser; long long id; GtkWidget *win, *title, *uri, *folder, *error; };

static void manager_populate(Browser *b) {
    if (!b->manager_win) return;
    auto *ctx = static_cast<ManagerCtx *>(g_object_get_data(G_OBJECT(b->manager_win), "ctx"));
    if (!ctx) return;
    while (GtkWidget *c = gtk_widget_get_first_child(ctx->list)) gtk_list_box_remove(GTK_LIST_BOX(ctx->list), c);
    const char *q = gtk_editable_get_text(GTK_EDITABLE(ctx->search));
    auto rows = b->store.bookmarks_full(q ? q : "");
    std::string current = "\x01";
    for (const auto &r : rows) {
        if (r.folder != current) {
            current = r.folder;
            GtkWidget *h = gtk_label_new((current.empty() ? "Bookmarks" : current).c_str());
            gtk_widget_add_css_class(h, "heading"); gtk_widget_set_halign(h, GTK_ALIGN_START);
            gtk_widget_set_margin_top(h, 10); gtk_widget_set_margin_start(h, 6);
            GtkWidget *hr = gtk_list_box_row_new(); gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(hr), h);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(hr), FALSE); gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(hr), FALSE);
            gtk_list_box_append(GTK_LIST_BOX(ctx->list), hr);
        }
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 8); gtk_widget_set_margin_end(row, 8); gtk_widget_set_margin_top(row, 4); gtk_widget_set_margin_bottom(row, 4);
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1); gtk_widget_set_hexpand(col, TRUE);
        GtkWidget *t = gtk_label_new((r.title.empty() ? r.uri : r.title).c_str()); gtk_widget_set_halign(t, GTK_ALIGN_START); gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END); gtk_label_set_xalign(GTK_LABEL(t), 0);
        GtkWidget *u = gtk_label_new(r.uri.c_str()); gtk_widget_set_halign(u, GTK_ALIGN_START); gtk_widget_add_css_class(u, "dim-label"); gtk_label_set_ellipsize(GTK_LABEL(u), PANGO_ELLIPSIZE_END); gtk_label_set_xalign(GTK_LABEL(u), 0);
        gtk_box_append(GTK_BOX(col), t); gtk_box_append(GTK_BOX(col), u);
        GtkWidget *open = gtk_button_new_from_icon_name("go-next-symbolic"); gtk_widget_set_tooltip_text(open, "Open");
        GtkWidget *edit = gtk_button_new_with_label("Edit"); gtk_widget_set_tooltip_text(edit, "Edit");
        GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic"); gtk_widget_set_tooltip_text(del, "Remove");
        for (GtkWidget *w : {open, edit, del}) { gtk_widget_add_css_class(w, "flat"); gtk_widget_set_valign(w, GTK_ALIGN_CENTER); }
        g_object_set_data_full(G_OBJECT(open), "uri", g_strdup(r.uri.c_str()), g_free);
        g_object_set_data(G_OBJECT(open), "browser", b);
        g_signal_connect(open, "clicked", G_CALLBACK(+[](GtkButton *bt, gpointer) {
            auto *br = static_cast<Browser *>(g_object_get_data(G_OBJECT(bt), "browser"));
            create_tab(br, false, static_cast<const char *>(g_object_get_data(G_OBJECT(bt), "uri")), false);
        }), nullptr);
        g_signal_connect_data(edit, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *rc = static_cast<RowCtx *>(d); show_bookmark_editor(rc->browser, rc->id);
        }), new RowCtx{b, r.id}, +[](gpointer p, GClosure *) { delete static_cast<RowCtx *>(p); }, GConnectFlags(0));
        g_signal_connect_data(del, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
            auto *rc = static_cast<RowCtx *>(d); rc->browser->store.delete_bookmark(rc->id);
            set_status(rc->browser, "Bookmark removed."); bookmarks_changed(rc->browser);
        }), new RowCtx{b, r.id}, +[](gpointer p, GClosure *) { delete static_cast<RowCtx *>(p); }, GConnectFlags(0));
        gtk_box_append(GTK_BOX(row), col); gtk_box_append(GTK_BOX(row), open); gtk_box_append(GTK_BOX(row), edit); gtk_box_append(GTK_BOX(row), del);
        gtk_list_box_append(GTK_LIST_BOX(ctx->list), row);
    }
    int total = b->store.count_bookmarks();
    std::string msg = rows.empty() ? (total ? "No bookmarks match your search." : "No bookmarks yet. Press Ctrl+D on a page, or import them.")
                                   : std::to_string(rows.size()) + " of " + std::to_string(total) + " bookmarks";
    gtk_label_set_text(GTK_LABEL(ctx->count), msg.c_str());
}

static void bookmarks_changed(Browser *b) { refresh_home_tabs(b); manager_populate(b); rebuild_bookmark_bar(b); update_chrome(b, active_tab(b)); }

static void show_bookmark_manager(Browser *b) {
    if (b->manager_win) { gtk_window_present(GTK_WINDOW(b->manager_win)); manager_populate(b); return; }
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->window));
    gtk_window_set_title(GTK_WINDOW(win), "Bookmarks"); gtk_window_set_default_size(GTK_WINDOW(win), 640, 560);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12); gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *search = gtk_search_entry_new(); gtk_widget_set_hexpand(search, TRUE); gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search title, address or folder");
    GtkWidget *imp = gtk_button_new_with_label("Import…");
    gtk_box_append(GTK_BOX(top), search); gtk_box_append(GTK_BOX(top), imp); gtk_box_append(GTK_BOX(root), top);
    GtkWidget *list = gtk_list_box_new(); gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(root), scroll);
    GtkWidget *count = gtk_label_new(""); gtk_widget_set_halign(count, GTK_ALIGN_START); gtk_widget_add_css_class(count, "dim-label"); gtk_box_append(GTK_BOX(root), count);
    gtk_window_set_child(GTK_WINDOW(win), root);
    g_object_set_data_full(G_OBJECT(win), "ctx", new ManagerCtx{b, list, search, count}, +[](gpointer p) { delete static_cast<ManagerCtx *>(p); });
    b->manager_win = win;
    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer d) { static_cast<Browser *>(d)->manager_win = nullptr; }), b);
    g_signal_connect(search, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer d) { manager_populate(static_cast<Browser *>(d)); }), b);
    g_signal_connect(imp, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) { show_import_wizard(static_cast<Browser *>(d)); }), b);
    manager_populate(b);
    gtk_window_present(GTK_WINDOW(win));
}

static void show_bookmark_editor(Browser *b, long long id) {
    Bookmark bm;
    if (!b->store.get_bookmark(id, &bm)) return;
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->manager_win ? b->manager_win : b->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE); gtk_window_set_title(GTK_WINDOW(win), "Edit bookmark"); gtk_window_set_default_size(GTK_WINDOW(win), 460, 230);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_start(grid, 16); gtk_widget_set_margin_end(grid, 16); gtk_widget_set_margin_top(grid, 16); gtk_widget_set_margin_bottom(grid, 16);
    const char *labels[] = {"Name", "Address", "Folder"};
    GtkWidget *entries[3];
    const std::string vals[3] = {bm.title, bm.uri, bm.folder};
    for (int i = 0; i < 3; ++i) {
        GtkWidget *l = gtk_label_new(labels[i]); gtk_widget_set_halign(l, GTK_ALIGN_END);
        entries[i] = gtk_entry_new(); gtk_widget_set_hexpand(entries[i], TRUE); gtk_editable_set_text(GTK_EDITABLE(entries[i]), vals[i].c_str());
        gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1); gtk_grid_attach(GTK_GRID(grid), entries[i], 1, i, 1, 1);
    }
    gtk_entry_set_placeholder_text(GTK_ENTRY(entries[2]), "None. Use Parent / Child for subfolders");
    GtkWidget *err = gtk_label_new(""); gtk_widget_set_halign(err, GTK_ALIGN_START); gtk_widget_add_css_class(err, "error");
    gtk_grid_attach(GTK_GRID(grid), err, 1, 3, 1, 1);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel"); GtkWidget *save = gtk_button_new_with_label("Save"); gtk_widget_add_css_class(save, "suggested-action");
    gtk_box_append(GTK_BOX(row), cancel); gtk_box_append(GTK_BOX(row), save); gtk_grid_attach(GTK_GRID(grid), row, 0, 4, 2, 1);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    g_signal_connect_data(save, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
        auto *c = static_cast<EditCtx *>(d);
        std::string title = gtk_editable_get_text(GTK_EDITABLE(c->title));
        std::string uri = normalize_input(gtk_editable_get_text(GTK_EDITABLE(c->uri)));
        std::string folder = gtk_editable_get_text(GTK_EDITABLE(c->folder));
        auto trim = [](std::string &v) { auto f = v.find_first_not_of(" \t"); auto l = v.find_last_not_of(" \t"); v = f == std::string::npos ? "" : v.substr(f, l - f + 1); };
        trim(title); trim(folder);
        if (uri.empty()) { gtk_label_set_text(GTK_LABEL(c->error), "Enter an address."); return; }
        if (!c->browser->store.update_bookmark(c->id, title, uri, folder)) { gtk_label_set_text(GTK_LABEL(c->error), "Another bookmark already uses that address."); return; }
        set_status(c->browser, "Bookmark saved.");
        Browser *br = c->browser; GtkWidget *w = c->win;
        gtk_window_destroy(GTK_WINDOW(w)); bookmarks_changed(br);
    }), new EditCtx{b, id, win, entries[0], entries[1], entries[2], err}, +[](gpointer p, GClosure *) { delete static_cast<EditCtx *>(p); }, GConnectFlags(0));
    gtk_window_set_child(GTK_WINDOW(win), grid); gtk_window_present(GTK_WINDOW(win));
}

// ----------------------------------------------------------------- import wizard
struct ImportCtx {
    Browser *browser; GtkWidget *win, *list, *status;
    std::vector<importer::Source> sources;
    std::vector<GtkWidget *> checks;
};
static void import_add_source(ImportCtx *c, const importer::Source &s, bool checked) {
    GtkWidget *chk = gtk_check_button_new_with_label(s.label.c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), checked);
    gtk_widget_set_tooltip_text(chk, s.path.c_str());
    gtk_box_append(GTK_BOX(c->list), chk);
    c->sources.push_back(s); c->checks.push_back(chk);
}
static void import_run(GtkButton *, gpointer d) {
    auto *c = static_cast<ImportCtx *>(d);
    std::vector<Bookmark> all; std::string errors; int picked = 0;
    for (size_t i = 0; i < c->sources.size(); ++i) {
        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(c->checks[i]))) continue;
        ++picked;
        std::string err; auto items = importer::load(c->sources[i], &err);
        if (items.empty() && !err.empty()) errors += c->sources[i].label + ": " + err + "\n";
        all.insert(all.end(), items.begin(), items.end());
    }
    if (!picked) { gtk_label_set_text(GTK_LABEL(c->status), "Tick at least one source, or choose a file."); return; }
    int added = c->browser->store.import_bookmarks(all);
    std::string msg = "Imported " + std::to_string(added) + " new bookmark" + (added == 1 ? "" : "s");
    if ((int)all.size() > added) msg += " (" + std::to_string(all.size() - added) + " already saved)";
    msg += ".";
    if (!errors.empty()) msg += "\n" + errors;
    gtk_label_set_text(GTK_LABEL(c->status), msg.c_str());
    set_status(c->browser, msg.substr(0, msg.find('\n')));
    bookmarks_changed(c->browser);
}
static void import_choose(GtkButton *, gpointer d) {
    auto *c = static_cast<ImportCtx *>(d);
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Choose a bookmarks file (HTML export, Chrome 'Bookmarks', or Firefox places.sqlite)");
    gtk_file_dialog_open(dlg, GTK_WINDOW(c->win), nullptr, +[](GObject *src, GAsyncResult *res, gpointer ud) {
        auto *ctx = static_cast<ImportCtx *>(ud);
        GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
        if (f) {
            char *path = g_file_get_path(f);
            if (path) { auto s = importer::classify(path); s.label = std::string("File — ") + g_path_get_basename(path); import_add_source(ctx, s, true); }
            g_free(path); g_object_unref(f);
        }
    }, c);
    g_object_unref(dlg);
}
static void show_import_wizard(Browser *b) {
    auto *c = new ImportCtx{b, nullptr, nullptr, nullptr, {}, {}};
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->window));
    gtk_window_set_title(GTK_WINDOW(win), "Import bookmarks"); gtk_window_set_default_size(GTK_WINDOW(win), 520, 380);
    g_object_set_data_full(G_OBJECT(win), "ctx", c, +[](gpointer p) { delete static_cast<ImportCtx *>(p); });
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 18); gtk_widget_set_margin_end(root, 18); gtk_widget_set_margin_top(root, 18); gtk_widget_set_margin_bottom(root, 18);
    GtkWidget *h = gtk_label_new("Bring your bookmarks along"); gtk_widget_add_css_class(h, "title-2"); gtk_widget_set_halign(h, GTK_ALIGN_START);
    GtkWidget *d = gtk_label_new("ReedBrowser found these browsers on your computer. Pick what to import, or choose an exported file. Bookmarks you already have are kept, and nothing is deleted.");
    gtk_label_set_wrap(GTK_LABEL(d), TRUE); gtk_widget_set_halign(d, GTK_ALIGN_START); gtk_label_set_xalign(GTK_LABEL(d), 0);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    GtkWidget *status = gtk_label_new(""); gtk_label_set_wrap(GTK_LABEL(status), TRUE); gtk_widget_set_halign(status, GTK_ALIGN_START); gtk_label_set_xalign(GTK_LABEL(status), 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *file = gtk_button_new_with_label("Choose file…"); GtkWidget *go = gtk_button_new_with_label("Import selected"); GtkWidget *close = gtk_button_new_with_label("Done");
    gtk_widget_add_css_class(go, "suggested-action"); gtk_widget_set_hexpand(file, FALSE);
    GtkWidget *spacer = gtk_label_new(""); gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), file); gtk_box_append(GTK_BOX(row), spacer); gtk_box_append(GTK_BOX(row), go); gtk_box_append(GTK_BOX(row), close);
    for (GtkWidget *w : {h, d, scroll, status, row}) gtk_box_append(GTK_BOX(root), w);
    c->win = win; c->list = list; c->status = status;
    auto found = importer::detect_sources();
    if (found.empty()) { GtkWidget *none = gtk_label_new("No other browsers were detected. Use “Choose file…” to import a bookmarks export (HTML)."); gtk_label_set_wrap(GTK_LABEL(none), TRUE); gtk_widget_set_halign(none, GTK_ALIGN_START); gtk_box_append(GTK_BOX(list), none); }
    for (const auto &s : found) import_add_source(c, s, true);
    g_signal_connect(go, "clicked", G_CALLBACK(import_run), c);
    g_signal_connect(file, "clicked", G_CALLBACK(import_choose), c);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), win);
    gtk_window_set_child(GTK_WINDOW(win), root); gtk_window_present(GTK_WINDOW(win));
}

// ======================================================================= extension install from the web
static bool host_is(const std::string &uri, const char *host) {
    GUri *u = g_uri_parse(uri.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
    bool ok = false;
    if (u) { const char *h = g_uri_get_host(u); ok = h && g_ascii_strcasecmp(h, host) == 0 && g_strcmp0(g_uri_get_scheme(u), "https") == 0; g_uri_unref(u); }
    return ok;
}
static bool is_trusted_install_host(const std::string &uri) {
    GUri *u = g_uri_parse(uri.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
    bool ok = false;
    if (u) {
        std::string h = g_uri_get_host(u) ? g_uri_get_host(u) : "";
        for (auto &ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto ends = [&](const char *suf) { std::string s2 = suf; return h == s2.substr(1) || (h.size() > s2.size() && h.compare(h.size() - s2.size(), s2.size(), s2) == 0); };
        ok = g_strcmp0(g_uri_get_scheme(u), "https") == 0 && (ends(".mozilla.org") || ends(".mozilla.net") || ends(".mozaws.net"));
        g_uri_unref(u);
    }
    return ok;
}
static bool looks_like_xpi(const std::string &uri) {
    std::string path = uri.substr(0, uri.find_first_of("?#"));
    return path.size() > 4 && g_ascii_strcasecmp(path.c_str() + path.size() - 4, ".xpi") == 0;
}

// Tell the AMO page what happened, so its button shows the right state.
static void amo_event(WebKitWebView *view, int install_id, const char *event) {
    if (!view || install_id < 0) return;
    char *js = g_strdup_printf("window.__reedAmoEvent && window.__reedAmoEvent(%d,'%s');", install_id, event);
    webkit_web_view_evaluate_javascript(view, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    g_free(js);
}

struct InstallJob {
    Browser *browser; GObject *view_ref; std::string source; int install_id; std::string temp; bool ok{false}; std::string error;
};
static void install_job_free(InstallJob *j) { if (j->view_ref) g_object_unref(j->view_ref); delete j; }

static void install_download_thread(GTask *task, gpointer, gpointer data, GCancellable *) {
    auto *j = static_cast<InstallJob *>(data);
    j->ok = ExtensionManager::fetch_package(j->source, j->temp, &j->error);
    g_task_return_boolean(task, TRUE);
}
static void install_download_done(GObject *, GAsyncResult *, gpointer data) {
    auto *j = static_cast<InstallJob *>(data);
    Browser *b = j->browser;
    WebKitWebView *view = j->view_ref ? WEBKIT_WEB_VIEW(j->view_ref) : nullptr;
    if (!j->ok) {
        amo_event(view, j->install_id, "onDownloadFailed");
        set_status(b, "Download failed.");
        extension_message(b, "Could not download extension", j->error);
        install_job_free(j); return;
    }
    amo_event(view, j->install_id, "onDownloadEnded");
    // Consent dialog: show what it asks for before installing.
    std::string summary;
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->window)); gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_title(GTK_WINDOW(win), "Add extension to ReedBrowser?"); gtk_window_set_default_size(GTK_WINDOW(win), 480, 220);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 18);
    std::string text = "Source: " + j->source + "\n\nThis extension will be given every permission its manifest declares. ReedBrowser does not show per-permission prompts yet, so only add extensions you trust.";
    GtkWidget *label = gtk_label_new(text.c_str()); gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel"); GtkWidget *add = gtk_button_new_with_label("Add extension"); gtk_widget_add_css_class(add, "suggested-action");
    gtk_box_append(GTK_BOX(row), cancel); gtk_box_append(GTK_BOX(row), add); gtk_box_append(GTK_BOX(box), label); gtk_box_append(GTK_BOX(box), row);
    g_object_set_data(G_OBJECT(add), "win", win); g_object_set_data(G_OBJECT(cancel), "win", win);
    g_signal_connect_data(add, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
        auto *job = static_cast<InstallJob *>(p);
        WebKitWebView *v = job->view_ref ? WEBKIT_WEB_VIEW(job->view_ref) : nullptr;
        amo_event(v, job->install_id, "onInstallStarted");
        std::string msg; bool ok = job->browser->ext && job->browser->ext->install(job->temp, true, &msg);
        if (ok) job->browser->ext->start_new();
        amo_event(v, job->install_id, ok ? "onInstallEnded" : "onInstallFailed");
        set_status(job->browser, ok ? "Extension installed." : "Extension install failed.");
        extension_message(job->browser, ok ? "Extension added" : "Installation failed", ok ? msg + "\n\nReload open tabs so the extension can run on them." : msg);
    }), j, nullptr, GConnectFlags(0));
    // Whatever button is pressed, the dialog closes and the job (and temp file) is released when it is destroyed.
    g_signal_connect_swapped(add, "clicked", G_CALLBACK(gtk_window_destroy), win);
    g_signal_connect_data(cancel, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
        auto *job = static_cast<InstallJob *>(p);
        amo_event(job->view_ref ? WEBKIT_WEB_VIEW(job->view_ref) : nullptr, job->install_id, "onInstallCancelled");
        set_status(job->browser, "Extension install cancelled.");
    }), j, nullptr, GConnectFlags(0));
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    g_signal_connect_data(win, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer p) {
        auto *job = static_cast<InstallJob *>(p);
        std::error_code ec; std::filesystem::remove(job->temp, ec);
        install_job_free(job);
    }), j, nullptr, GConnectFlags(0));
    gtk_window_set_child(GTK_WINDOW(win), box); gtk_window_present(GTK_WINDOW(win));
    amo_event(view, j->install_id, "onInstallStarted");
}

static void start_extension_install(Browser *b, WebKitWebView *origin, const std::string &source, int install_id) {
    if (!b->ext) { extension_message(b, "Extensions unavailable", "The extension runtime is not running."); return; }
    auto *j = new InstallJob{b, origin ? G_OBJECT(g_object_ref(origin)) : nullptr, source, install_id, "", false, ""};
    j->temp = b->data_dir + "/web-install-" + std::to_string(g_get_monotonic_time()) + ".xpi";
    set_status(b, "Downloading extension…");
    amo_event(origin, install_id, "onDownloadStarted");
    GTask *task = g_task_new(nullptr, nullptr, install_download_done, j);
    g_task_set_task_data(task, j, nullptr);
    g_task_run_in_thread(task, install_download_thread);
    g_object_unref(task);
}

// JS injected on addons.mozilla.org: presents the page with the Firefox install API
// (navigator.mozAddonManager) and adds an "Install in ReedBrowser" button as a fallback.
static const char *amo_shim_js = R"JS((function(){
if (window.__reedAmoShim) return; window.__reedAmoShim = true;
function post(m){ try { window.webkit.messageHandlers.reedAmo.postMessage(JSON.stringify(m)); } catch(e){} }
var installs = {}, nextId = 1, mgrListeners = {};
function Install(opts){ this.id = nextId++; this.url = opts.url; this.state = 'STATE_AVAILABLE'; this.error = null; this.maxProgress = -1; this.progress = 0; this._l = {}; installs[this.id] = this; }
Install.prototype.addEventListener = function(t, f){ (this._l[t] = this._l[t] || []).push(f); };
Install.prototype.removeEventListener = function(t, f){ var a = this._l[t] || []; var i = a.indexOf(f); if (i >= 0) a.splice(i, 1); };
Install.prototype.install = function(){ var self = this; return new Promise(function(res, rej){ self._done = {res: res, rej: rej}; post({type: 'install', id: self.id, url: self.url}); }); };
Install.prototype.cancel = function(){ return Promise.resolve(); };
window.__reedAmoEvent = function(id, name){
  var i = installs[id]; if (!i) return;
  if (name === 'onDownloadStarted') i.state = 'STATE_DOWNLOADING';
  else if (name === 'onDownloadEnded') i.state = 'STATE_DOWNLOADED';
  else if (name === 'onInstallStarted') i.state = 'STATE_INSTALLING';
  else if (name === 'onInstallEnded') i.state = 'STATE_INSTALLED';
  else if (name === 'onInstallCancelled') i.state = 'STATE_CANCELLED';
  else { i.state = 'STATE_FAILED'; i.error = name; }
  var ev = {type: name, id: id, state: i.state, error: i.error};
  (i._l[name] || []).slice().forEach(function(f){ try { f.call(i, ev); } catch(e){} });
  if (i._done) {
    if (name === 'onInstallEnded') i._done.res(); else if (name === 'onInstallCancelled' || /Failed/.test(name)) i._done.rej(new Error(name));
  }
};
var mgr = {
  permissionPromptsEnabled: true, abuseReportPanelEnabled: false,
  createInstall: function(opts){ return Promise.resolve(new Install(opts || {})); },
  getAddonByID: function(){ return Promise.resolve(null); },
  addEventListener: function(t, f){ (mgrListeners[t] = mgrListeners[t] || []).push(f); },
  removeEventListener: function(){}
};
try { Object.defineProperty(navigator, 'mozAddonManager', {value: mgr, configurable: true}); } catch(e){}
// Fallback button on add-on pages, in case the site still shows "Download Firefox".
function slug(){ var m = location.pathname.match(/\/addon\/([^\/]+)/); return m ? decodeURIComponent(m[1]) : null; }
function ensureButton(){
  var s = slug(); var b = document.getElementById('reedbrowser-install');
  if (!s) { if (b) b.remove(); return; }
  if (b) { b.dataset.slug = s; return; }
  if (!document.body) return;
  b = document.createElement('button'); b.id = 'reedbrowser-install'; b.dataset.slug = s; b.textContent = 'Install in ReedBrowser';
  b.setAttribute('style', 'position:fixed;right:18px;bottom:18px;z-index:2147483647;padding:12px 18px;border:0;border-radius:10px;background:#0060df;color:#fff;font:600 15px system-ui,sans-serif;box-shadow:0 4px 14px rgba(0,0,0,.35);cursor:pointer');
  b.onclick = function(){ post({type: 'slug', slug: b.dataset.slug}); };
  document.body.appendChild(b);
}
setInterval(ensureButton, 700);
})();)JS";

static void on_amo_message(WebKitUserContentManager *, JSCValue *value, gpointer data) {
    auto *view = WEBKIT_WEB_VIEW(data);
    Browser *b = static_cast<Browser *>(g_object_get_data(G_OBJECT(view), "reed-browser"));
    const char *page = webkit_web_view_get_uri(view);
    if (!b || !page || !host_is(page, "addons.mozilla.org") || !jsc_value_is_string(value)) return;  // only the real AMO site may ask
    char *raw = jsc_value_to_string(value);
    JsonParser *p = json_parser_new();
    if (raw && json_parser_load_from_data(p, raw, -1, nullptr)) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        std::string type = json_object_get_string_member_with_default(o, "type", "");
        if (type == "install") {
            std::string url = json_object_get_string_member_with_default(o, "url", "");
            int id = static_cast<int>(json_object_get_int_member_with_default(o, "id", -1));
            if (is_trusted_install_host(url)) start_extension_install(b, view, url, id);
            else amo_event(view, id, "onInstallFailed");
        } else if (type == "slug") {
            std::string slug = json_object_get_string_member_with_default(o, "slug", "");
            if (!slug.empty() && slug.size() < 200) start_extension_install(b, view, "amo:" + slug, -1);
        }
    }
    g_object_unref(p); g_free(raw);
}

static void attach_amo_support(Browser *b, WebKitWebView *view, WebKitUserContentManager *ucm) {
    g_object_set_data(G_OBJECT(view), "reed-browser", b);
    if (!webkit_user_content_manager_register_script_message_handler(ucm, "reedAmo", nullptr)) return;
    g_signal_connect(ucm, "script-message-received::reedAmo", G_CALLBACK(on_amo_message), view);
    const char *allow[] = {"https://addons.mozilla.org/*", nullptr};
    WebKitUserScript *us = webkit_user_script_new(amo_shim_js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, allow, nullptr);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);
}

struct ReedAction { Browser *browser; int tab_id; std::string action; long long id; std::string xpi; };
static gboolean run_reed_action(gpointer data) {
    std::unique_ptr<ReedAction> a(static_cast<ReedAction *>(data));
    Browser *b = a->browser;
    if (b->shutting_down) return G_SOURCE_REMOVE;
    if (a->action == "remove") { b->store.delete_bookmark(a->id); set_status(b, "Bookmark removed."); bookmarks_changed(b); }
    else if (a->action == "edit") show_bookmark_editor(b, a->id);
    else if (a->action == "manage") show_bookmark_manager(b);
    else if (a->action == "import") show_import_wizard(b);
    else if (a->action == "xpi") start_extension_install(b, nullptr, a->xpi, -1);
    return G_SOURCE_REMOVE;
}
static void queue_reed_action(Browser *b, const std::string &action, long long id, const std::string &xpi = "") {
    g_idle_add(run_reed_action, new ReedAction{b, 0, action, id, xpi});
}

// reed://<action>[/<id>]?t=<token> from the new-tab page.
static void handle_reed_uri(Browser *b, WebKitWebView *view, const std::string &uri) {
    const char *cur = webkit_web_view_get_uri(view);
    if (cur && g_strcmp0(cur, "about:blank") != 0) return;        // only our own local page
    auto q = uri.find("?t=");
    if (q == std::string::npos || uri.substr(q + 3) != b->home_token) return;
    std::string path = uri.substr(7, q - 7), action = path.substr(0, path.find('/'));
    long long id = 0;
    if (path.find('/') != std::string::npos) id = std::atoll(path.c_str() + path.find('/') + 1);
    if ((action == "remove" || action == "edit") && id <= 0) return;
    if (action == "remove" || action == "edit" || action == "manage" || action == "import") queue_reed_action(b, action, id);
}

static void set_site_user_agent(WebKitWebView *view, const std::string &uri) {
    // addons.mozilla.org only shows "Add to Firefox" to browsers that identify as Firefox.
    static const char *ff = "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0";
    WebKitSettings *st = webkit_web_view_get_settings(view);
    bool amo = host_is(uri, "addons.mozilla.org");
    const char *current = webkit_settings_get_user_agent(st);
    bool has = current && g_strcmp0(current, ff) == 0;
    if (amo && !has) webkit_settings_set_user_agent(st, ff);
    else if (!amo && has && !uri.empty() && uri.rfind("http", 0) == 0) webkit_settings_set_user_agent(st, nullptr);
}

static gboolean on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer data) {
    Tab *tab = static_cast<Tab *>(data);
    Browser *b = tab->browser;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationAction *na = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        WebKitURIRequest *req = webkit_navigation_action_get_request(na);
        const char *u = req ? webkit_uri_request_get_uri(req) : nullptr;
        if (!u) return FALSE;
        std::string uri = u;
        if (uri.rfind("reed://", 0) == 0) { webkit_policy_decision_ignore(decision); handle_reed_uri(b, view, uri); return TRUE; }
        if (!tab->private_mode && looks_like_xpi(uri) && uri.rfind("https://", 0) == 0) {
            webkit_policy_decision_ignore(decision); queue_reed_action(b, "xpi", 0, uri); return TRUE;
        }
        set_site_user_agent(view, uri);
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE && !tab->private_mode) {
        WebKitResponsePolicyDecision *rd = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        WebKitURIResponse *resp = webkit_response_policy_decision_get_response(rd);
        const char *mime = webkit_uri_response_get_mime_type(resp);
        const char *u = webkit_uri_response_get_uri(resp);
        if (mime && u && g_strcmp0(mime, "application/x-xpinstall") == 0 && g_str_has_prefix(u, "https://")) {
            webkit_policy_decision_ignore(decision); queue_reed_action(b, "xpi", 0, u); return TRUE;
        }
    }
    return FALSE;
}

// ======================================================================= extension toolbar, popups, options, manager
struct ExtBtnCtx { Browser *browser; std::string uuid; };

static void ext_popover_show(GtkPopover *pop, gpointer d) {
    auto *c = static_cast<ExtBtnCtx *>(d);
    if (!c->browser->ext) return;
    WebKitWebView *v = c->browser->ext->create_popup(c->uuid);
    if (!v) return;
    gtk_widget_set_size_request(GTK_WIDGET(v), 400, 560);
    gtk_popover_set_child(pop, GTK_WIDGET(v));
}
static void ext_popover_closed(GtkPopover *pop, gpointer) { gtk_popover_set_child(pop, nullptr); }  // destroys the popup page

static void refresh_extension_buttons(Browser *b) {
    if (!b->ext_box || !b->ext) return;
    auto actions = b->ext->actions();
    std::string sig;
    for (auto &a : actions) sig += a.uuid + "|" + a.icon_path + "|" + (a.popup.empty() ? "0" : "1") + ";";
    const char *old = static_cast<const char *>(g_object_get_data(G_OBJECT(b->ext_box), "sig"));
    if (!old || sig != old) {
        while (GtkWidget *c = gtk_widget_get_first_child(b->ext_box)) gtk_box_remove(GTK_BOX(b->ext_box), c);
        for (auto &a : actions) {
            GtkWidget *btn;
            GtkWidget *img = a.icon_path.empty() ? nullptr : gtk_image_new_from_file(a.icon_path.c_str());
            if (img) gtk_image_set_pixel_size(GTK_IMAGE(img), 18);
            if (!a.popup.empty()) {
                btn = gtk_menu_button_new();
                if (img) gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), img); else gtk_menu_button_set_label(GTK_MENU_BUTTON(btn), a.name.substr(0, 1).c_str());
                GtkWidget *pop = gtk_popover_new();
                gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), pop);
                g_signal_connect_data(pop, "show", G_CALLBACK(ext_popover_show), new ExtBtnCtx{b, a.uuid}, +[](gpointer p, GClosure *) { delete static_cast<ExtBtnCtx *>(p); }, GConnectFlags(0));
                g_signal_connect(pop, "closed", G_CALLBACK(ext_popover_closed), nullptr);
            } else {
                btn = gtk_button_new();
                if (img) gtk_button_set_child(GTK_BUTTON(btn), img); else gtk_button_set_label(GTK_BUTTON(btn), a.name.substr(0, 1).c_str());
                g_signal_connect_data(btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) {
                    auto *c = static_cast<ExtBtnCtx *>(d);
                    Tab *t = active_tab(c->browser);
                    if (t && c->browser->ext) c->browser->ext->action_clicked(c->uuid, t->id);
                }), new ExtBtnCtx{b, a.uuid}, +[](gpointer p, GClosure *) { delete static_cast<ExtBtnCtx *>(p); }, GConnectFlags(0));
            }
            gtk_widget_add_css_class(btn, "flat");
            GtkWidget *overlay = gtk_overlay_new();
            gtk_overlay_set_child(GTK_OVERLAY(overlay), btn);
            GtkWidget *badge = gtk_label_new("");
            gtk_widget_add_css_class(badge, "ext-badge");
            gtk_widget_set_halign(badge, GTK_ALIGN_END); gtk_widget_set_valign(badge, GTK_ALIGN_END);
            gtk_widget_set_can_target(badge, FALSE); gtk_widget_set_visible(badge, FALSE);
            gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badge);
            g_object_set_data_full(G_OBJECT(overlay), "uuid", g_strdup(a.uuid.c_str()), g_free);
            g_object_set_data(G_OBJECT(overlay), "badge", badge);
            g_object_set_data(G_OBJECT(overlay), "button", btn);
            gtk_box_append(GTK_BOX(b->ext_box), overlay);
        }
        g_object_set_data_full(G_OBJECT(b->ext_box), "sig", g_strdup(sig.c_str()), g_free);
    }
    // cheap in-place update: titles and badges
    for (GtkWidget *w = gtk_widget_get_first_child(b->ext_box); w; w = gtk_widget_get_next_sibling(w)) {
        const char *uuid = static_cast<const char *>(g_object_get_data(G_OBJECT(w), "uuid"));
        for (auto &a : actions) if (uuid && a.uuid == uuid) {
            auto *badge = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(w), "badge"));
            auto *btn = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(w), "button"));
            gtk_widget_set_tooltip_text(btn, a.title.c_str());
            gtk_label_set_text(GTK_LABEL(badge), a.badge.c_str());
            gtk_widget_set_visible(badge, !a.badge.empty());
            if (!a.badge_color.empty() && a.badge_color[0] == '#' && a.badge_color.size() <= 9) {
                char *css = g_strdup_printf(".ext-badge{background:%s;color:white;border-radius:8px;padding:0 4px;font-size:9px;font-weight:700;}", a.badge_color.c_str());
                GtkCssProvider *cp = gtk_css_provider_new();
                gtk_css_provider_load_from_string(cp, css);
                gtk_style_context_add_provider(gtk_widget_get_style_context(badge), GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_unref(cp); g_free(css);
            }
        }
    }
    gtk_widget_set_visible(b->ext_box, !actions.empty());
}

static void open_extension_options(Browser *b, const std::string &uuid) {
    if (!b->ext) return;
    WebKitWebView *v = b->ext->create_options(uuid);
    if (!v) { set_status(b, "This extension has no options page."); return; }
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_title(GTK_WINDOW(win), "Extension options"); gtk_window_set_default_size(GTK_WINDOW(win), 820, 640);
    gtk_window_set_child(GTK_WINDOW(win), GTK_WIDGET(v));
    gtk_window_present(GTK_WINDOW(win));
}

struct ExtRowCtx { Browser *browser; std::string uuid; };
static void show_extension_manager(Browser *b);
static void ext_manager_populate(Browser *b, GtkWidget *list) {
    while (GtkWidget *c = gtk_widget_get_first_child(list)) gtk_box_remove(GTK_BOX(list), c);
    if (!b->ext) return;
    auto items = b->ext->list();
    if (items.empty()) {
        GtkWidget *l = gtk_label_new("No extensions installed yet. Open addons.mozilla.org and use “Add to Firefox”, or “Install in ReedBrowser”.");
        gtk_label_set_wrap(GTK_LABEL(l), TRUE); gtk_widget_set_halign(l, GTK_ALIGN_START); gtk_box_append(GTK_BOX(list), l); return;
    }
    for (auto &it : items) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new((it.name + "  " + it.version).c_str()); gtk_widget_set_hexpand(label, TRUE); gtk_widget_set_halign(label, GTK_ALIGN_START);
        GtkWidget *sw = gtk_switch_new(); gtk_switch_set_active(GTK_SWITCH(sw), it.enabled); gtk_widget_set_valign(sw, GTK_ALIGN_CENTER); gtk_widget_set_tooltip_text(sw, "Enabled (content scripts change after restart)");
        GtkWidget *opt = gtk_button_new_with_label("Options"); gtk_widget_set_sensitive(opt, it.has_options);
        GtkWidget *rm = gtk_button_new_with_label("Remove");
        g_signal_connect_data(sw, "state-set", G_CALLBACK(+[](GtkSwitch *, gboolean st, gpointer d) -> gboolean {
            auto *c = static_cast<ExtRowCtx *>(d); c->browser->ext->set_enabled(c->uuid, st); return FALSE;
        }), new ExtRowCtx{b, it.uuid}, +[](gpointer p, GClosure *) { delete static_cast<ExtRowCtx *>(p); }, GConnectFlags(0));
        g_signal_connect_data(opt, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) { auto *c = static_cast<ExtRowCtx *>(d); open_extension_options(c->browser, c->uuid); }),
            new ExtRowCtx{b, it.uuid}, +[](gpointer p, GClosure *) { delete static_cast<ExtRowCtx *>(p); }, GConnectFlags(0));
        g_object_set_data(G_OBJECT(rm), "list", list);
        g_signal_connect_data(rm, "clicked", G_CALLBACK(+[](GtkButton *bt, gpointer d) {
            auto *c = static_cast<ExtRowCtx *>(d);
            std::string msg; bool ok = c->browser->ext->remove(c->uuid, &msg);
            set_status(c->browser, ok ? "Extension removed. Restart ReedBrowser to finish unloading it." : "Could not remove extension.");
            ext_manager_populate(c->browser, GTK_WIDGET(g_object_get_data(G_OBJECT(bt), "list")));
            refresh_extension_buttons(c->browser);
        }), new ExtRowCtx{b, it.uuid}, +[](gpointer p, GClosure *) { delete static_cast<ExtRowCtx *>(p); }, GConnectFlags(0));
        for (GtkWidget *w : {label, sw, opt, rm}) gtk_box_append(GTK_BOX(row), w);
        gtk_box_append(GTK_BOX(list), row);
    }
}
static void show_extension_manager(Browser *b) {
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->window));
    gtk_window_set_title(GTK_WINDOW(win), "Extensions"); gtk_window_set_default_size(GTK_WINDOW(win), 560, 420);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 16); gtk_widget_set_margin_end(root, 16); gtk_widget_set_margin_top(root, 16); gtk_widget_set_margin_bottom(root, 16);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    GtkWidget *hint = gtk_label_new("Find extensions at addons.mozilla.org. uBlock Origin Lite (MV3) and Proton Pass are the supported targets.");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE); gtk_widget_set_halign(hint, GTK_ALIGN_START); gtk_widget_add_css_class(hint, "dim-label");
    GtkWidget *browse = gtk_button_new_with_label("Browse Firefox Add-ons"); gtk_widget_set_halign(browse, GTK_ALIGN_START);
    g_signal_connect_data(browse, "clicked", G_CALLBACK(+[](GtkButton *, gpointer d) { create_tab(static_cast<Browser *>(d), false, "https://addons.mozilla.org/", false); }), b, nullptr, GConnectFlags(0));
    gtk_box_append(GTK_BOX(root), scroll); gtk_box_append(GTK_BOX(root), hint); gtk_box_append(GTK_BOX(root), browse);
    gtk_window_set_child(GTK_WINDOW(win), root);
    ext_manager_populate(b, list);
    gtk_window_present(GTK_WINDOW(win));
}

// ======================================================================= bookmarks bar, downloads window, reopen tab
static void reopen_closed_tab(Browser *b) {
    if (b->closed_uris.empty()) { set_status(b, "No recently closed tabs."); return; }
    std::string uri = b->closed_uris.back(); b->closed_uris.pop_back();
    create_tab(b, false, uri, false);
}

static void bar_open(GtkButton *bt, gpointer d) {
    Browser *b = static_cast<Browser *>(d);
    const char *uri = static_cast<const char *>(g_object_get_data(G_OBJECT(bt), "uri"));
    if (!uri) return;
    if (Tab *t = active_tab(b)) webkit_web_view_load_uri(t->view, uri);
}

static void rebuild_bookmark_bar(Browser *b) {
    if (!b->bookmark_bar) return;
    GtkWidget *row = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(b->bookmark_bar), "row"));
    while (GtkWidget *c = gtk_widget_get_first_child(row)) gtk_box_remove(GTK_BOX(row), c);
    auto all = b->store.bookmarks_full();
    auto in_bar = [](const std::string &f) {
        return f == "Bookmarks bar" || f == "Bookmarks Toolbar" || f.rfind("Bookmarks bar / ", 0) == 0 || f.rfind("Bookmarks Toolbar / ", 0) == 0;
    };
    std::vector<Bookmark> shown;
    for (auto &x : all) if (in_bar(x.folder)) shown.push_back(x);
    if (shown.empty()) for (auto &x : all) if (x.folder.empty()) shown.push_back(x);  // no toolbar folder: use unfiled ones
    int n = 0;
    for (auto &x : shown) {
        if (++n > 30) break;
        std::string label = x.title.empty() ? x.uri : x.title;
        if (label.size() > 22) label = label.substr(0, 20) + "…";
        GtkWidget *btn = gtk_button_new_with_label(label.c_str());
        gtk_widget_add_css_class(btn, "flat"); gtk_widget_set_tooltip_text(btn, x.uri.c_str());
        g_object_set_data_full(G_OBJECT(btn), "uri", g_strdup(x.uri.c_str()), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(bar_open), b);
        gtk_box_append(GTK_BOX(row), btn);
    }
    GtkWidget *more = gtk_button_new_with_label("All bookmarks »");
    gtk_widget_add_css_class(more, "flat");
    g_signal_connect_swapped(more, "clicked", G_CALLBACK(+[](gpointer d) { show_bookmark_manager(static_cast<Browser *>(d)); }), b);
    gtk_box_append(GTK_BOX(row), more);
    std::string pref = b->store.meta("bookmark_bar");
    gtk_widget_set_visible(b->bookmark_bar, pref == "1" || (pref.empty() && !all.empty()));
}

static void toggle_bookmark_bar(Browser *b) {
    bool now = gtk_widget_get_visible(b->bookmark_bar);
    b->store.set_meta("bookmark_bar", now ? "0" : "1");
    rebuild_bookmark_bar(b);
    set_status(b, now ? "Bookmarks bar hidden." : "Bookmarks bar shown.");
}

static void show_downloads(Browser *b) {
    GtkWidget *win = gtk_window_new(); gtk_window_set_application(GTK_WINDOW(win), b->app);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(b->window));
    gtk_window_set_title(GTK_WINDOW(win), "Downloads"); gtk_window_set_default_size(GTK_WINDOW(win), 620, 460);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 14); gtk_widget_set_margin_end(root, 14); gtk_widget_set_margin_top(root, 14); gtk_widget_set_margin_bottom(root, 14);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    for (auto &kv : b->downloads) {
        GtkWidget *l = gtk_label_new(("⬇ In progress: " + kv.second).c_str()); gtk_widget_set_halign(l, GTK_ALIGN_START); gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(list), l);
    }
    auto rows = b->store.recent_downloads(100);
    if (rows.empty() && b->downloads.empty()) { GtkWidget *l = gtk_label_new("No downloads yet."); gtk_widget_set_halign(l, GTK_ALIGN_START); gtk_box_append(GTK_BOX(list), l); }
    for (auto &r : rows) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1); gtk_widget_set_hexpand(col, TRUE);
        GtkWidget *n = gtk_label_new((r.name + (r.state == "failed" ? "  (failed)" : "")).c_str()); gtk_widget_set_halign(n, GTK_ALIGN_START); gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
        GtkWidget *p = gtk_label_new(r.path.c_str()); gtk_widget_set_halign(p, GTK_ALIGN_START); gtk_widget_add_css_class(p, "dim-label"); gtk_label_set_ellipsize(GTK_LABEL(p), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(col), n); gtk_box_append(GTK_BOX(col), p);
        GtkWidget *open = gtk_button_new_with_label("Open"); GtkWidget *folder = gtk_button_new_with_label("Show in folder");
        gtk_widget_set_sensitive(open, r.state != "failed" && g_file_test(r.path.c_str(), G_FILE_TEST_EXISTS));
        g_object_set_data_full(G_OBJECT(open), "path", g_strdup(r.path.c_str()), g_free);
        g_object_set_data_full(G_OBJECT(folder), "path", g_strdup(r.path.c_str()), g_free);
        g_signal_connect(open, "clicked", G_CALLBACK(+[](GtkButton *bt, gpointer) {
            char *u = g_filename_to_uri(static_cast<const char *>(g_object_get_data(G_OBJECT(bt), "path")), nullptr, nullptr);
            if (u) { g_app_info_launch_default_for_uri(u, nullptr, nullptr); g_free(u); }
        }), nullptr);
        g_signal_connect(folder, "clicked", G_CALLBACK(+[](GtkButton *bt, gpointer) {
            char *dir = g_path_get_dirname(static_cast<const char *>(g_object_get_data(G_OBJECT(bt), "path")));
            char *u = g_filename_to_uri(dir, nullptr, nullptr);
            if (u) { g_app_info_launch_default_for_uri(u, nullptr, nullptr); g_free(u); }
            g_free(dir);
        }), nullptr);
        gtk_box_append(GTK_BOX(row), col); gtk_box_append(GTK_BOX(row), open); gtk_box_append(GTK_BOX(row), folder);
        gtk_box_append(GTK_BOX(list), row);
    }
    GtkWidget *clear = gtk_button_new_with_label("Clear list"); gtk_widget_set_halign(clear, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(clear), "win", win);
    g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton *bt, gpointer d) { static_cast<Browser *>(d)->store.clear_downloads(); gtk_window_destroy(GTK_WINDOW(g_object_get_data(G_OBJECT(bt), "win"))); }), b);
    gtk_box_append(GTK_BOX(root), scroll); gtk_box_append(GTK_BOX(root), clear);
    gtk_window_set_child(GTK_WINDOW(win), root); gtk_window_present(GTK_WINDOW(win));
}

static void on_menu_action(GtkButton *button, gpointer data) {
    Browser *b = static_cast<Browser *>(data); int action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "action"));
    if (action == 0) create_tab(b, false, "", false);
    else if (action == 1) { create_tab(b, true, "", false); set_status(b, "Private tab: history and site data are not saved."); }
    else if (action == 2) show_settings(b);
    else if (action == 3) set_theme(b, "dark");
    else if (action == 5) show_bookmark_manager(b);
    else if (action == 6) show_import_wizard(b);
    else if (action == 7) show_extension_manager(b);
    else if (action == 8) show_downloads(b);
    else if (action == 9) reopen_closed_tab(b);
    else if (action == 10) toggle_bookmark_bar(b);
    else set_theme(b, "system");
    GtkWidget *p = gtk_widget_get_parent(GTK_WIDGET(button)); if (p) p = gtk_widget_get_parent(p); if (p && GTK_IS_POPOVER(p)) gtk_popover_popdown(GTK_POPOVER(p));
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

    load_theme(b);
    apply_css();
    apply_appearance(b);
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
    {
        b->bookmark_bar = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(b->bookmark_bar), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(b->bookmark_bar), TRUE);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_set_margin_start(row, 8); gtk_widget_set_margin_end(row, 8);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(b->bookmark_bar), row);
        g_object_set_data(G_OBJECT(b->bookmark_bar), "row", row);
        gtk_box_append(GTK_BOX(root), b->bookmark_bar);
        rebuild_bookmark_bar(b);
    }

    GtkWidget *back = icon_button("go-previous-symbolic", "Back", G_CALLBACK(on_back), b);
    GtkWidget *forward = icon_button("go-next-symbolic", "Forward", G_CALLBACK(on_forward), b);
    GtkWidget *reload = icon_button("view-refresh-symbolic", "Reload", G_CALLBACK(on_reload), b);
    GtkWidget *home = icon_button("go-home-symbolic", "Home", G_CALLBACK(on_home), b);
    GtkWidget *bookmark = icon_button("starred-symbolic", "Bookmark this page (Ctrl+D)", G_CALLBACK(on_bookmark), b);
    b->bookmark_btn = bookmark;
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
    b->ext_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(b->toolbar), b->ext_box);
    if (b->ext) {
        b->ext->set_ui_callbacks([b]() { refresh_extension_buttons(b); }, [b](const std::string &uuid) { open_extension_options(b, uuid); });
        refresh_extension_buttons(b);
    }
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu_button, "ReedBrowser menu");
    GtkWidget *popover = gtk_popover_new(); GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(menu_box, 8); gtk_widget_set_margin_end(menu_box, 8); gtk_widget_set_margin_top(menu_box, 8); gtk_widget_set_margin_bottom(menu_box, 8);
    const char *menu_labels[] = {"New tab", "New private tab", "Settings & extensions", "Dark mode", "System theme", "Bookmarks (Ctrl+Shift+O)", "Import bookmarks…", "Extensions", "Downloads (Ctrl+J)", "Reopen closed tab (Ctrl+Shift+T)", "Show/hide bookmarks bar (Ctrl+Shift+B)"};
    for (int i=0;i<11;i++) { GtkWidget *item=gtk_button_new_with_label(menu_labels[i]); gtk_widget_add_css_class(item,"flat"); g_object_set_data(G_OBJECT(item),"action",GINT_TO_POINTER(i)); g_signal_connect(item,"clicked",G_CALLBACK(on_menu_action),b); gtk_box_append(GTK_BOX(menu_box),item); }
    gtk_popover_set_child(GTK_POPOVER(popover),menu_box); gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button),popover); gtk_box_append(GTK_BOX(b->toolbar),menu_button);
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

    // First run: offer to import bookmarks from other browsers (once, and only if the user has none yet).
    if (b->store.meta("import_offered").empty()) {
        b->store.set_meta("import_offered", "1");
        if (b->store.count_bookmarks() == 0)
            g_idle_add(+[](gpointer d) -> gboolean { show_import_wizard(static_cast<Browser *>(d)); return G_SOURCE_REMOVE; }, b);
    }

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
    const char *css = R"CSS(.bookmarked{color:#e5b800;}.toolbar{background:alpha(@theme_bg_color,.94);border-bottom:1px solid alpha(@theme_fg_color,.10);}.address-entry{border-radius:10px;min-height:34px;padding-left:12px;padding-right:12px;}.notebook tab{padding:5px 8px;}.notebook tab label{font-size:0.92em;}.toolbar.compact-ui{padding-top:0;padding-bottom:0;}.toolbar.spacious-ui{padding-top:8px;padding-bottom:8px;})CSS";
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
                 "       reedbrowser --install-extension <AMO-URL|amo:slug|file.xpi|.zip|folder> [--yes]\n"
                 "       reedbrowser --list-extensions\n"
                 "       reedbrowser --remove-extension <id-or-name>\n"
                 "       reedbrowser --version");
            return 0;
        }
    }
    Browser browser;
    browser.data_dir = home_path(".local/share/reedbrowser");
    { gchar *tok = g_uuid_string_random(); browser.home_token = tok; g_free(tok); }
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
