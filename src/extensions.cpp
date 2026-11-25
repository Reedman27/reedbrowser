// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
#include "extensions.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <json-glib/json-glib.h>
#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <cctype>

#include "dnr.hpp"
#include "ext/api_js.h"

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ JSON helpers

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool write_file(const std::string &path, const std::string &data) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

struct Json {  // owning JsonNode wrapper
    JsonNode *n = nullptr;
    Json() = default;
    explicit Json(JsonNode *node) : n(node) {}
    Json(const Json &) = delete;
    Json(Json &&o) noexcept : n(o.n) { o.n = nullptr; }
    Json &operator=(Json &&o) noexcept { if (this != &o) { if (n) json_node_unref(n); n = o.n; o.n = nullptr; } return *this; }
    ~Json() { if (n) json_node_unref(n); }
    JsonObject *obj() const { return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : nullptr; }
    JsonArray *arr() const { return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : nullptr; }
    explicit operator bool() const { return n != nullptr; }
};
Json parse(const std::string &s) {
    GError *err = nullptr;
    JsonNode *n = json_from_string(s.c_str(), &err);
    if (err) g_error_free(err);
    return Json(n);
}
std::string dump(JsonNode *n) {
    if (!n) return "null";
    char *s = json_to_string(n, FALSE);
    std::string r = s ? s : "null";
    g_free(s);
    return r;
}
std::string quote(const std::string &s) {  // JSON string literal
    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(n, s.c_str());
    std::string r = dump(n);
    json_node_unref(n);
    return r;
}
JsonNode *member(JsonObject *o, const char *k) { return (o && json_object_has_member(o, k)) ? json_object_get_member(o, k) : nullptr; }
std::string str_of(JsonObject *o, const char *k, const std::string &def = "") {
    JsonNode *m = member(o, k);
    return (m && JSON_NODE_HOLDS_VALUE(m) && json_node_get_value_type(m) == G_TYPE_STRING) ? json_node_get_string(m) : def;
}
bool bool_of(JsonObject *o, const char *k, bool def = false) {
    JsonNode *m = member(o, k);
    return (m && JSON_NODE_HOLDS_VALUE(m) && json_node_get_value_type(m) == G_TYPE_BOOLEAN) ? json_node_get_boolean(m) : def;
}
std::vector<std::string> strings_of(JsonObject *o, const char *k) {
    std::vector<std::string> out;
    JsonNode *m = member(o, k);
    if (!m) return out;
    if (JSON_NODE_HOLDS_ARRAY(m)) {
        JsonArray *a = json_node_get_array(m);
        for (guint i = 0; i < json_array_get_length(a); ++i) {
            JsonNode *e = json_array_get_element(a, i);
            if (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING) out.push_back(json_node_get_string(e));
        }
    } else if (JSON_NODE_HOLDS_VALUE(m) && json_node_get_value_type(m) == G_TYPE_STRING) out.push_back(json_node_get_string(m));
    return out;
}

std::string random_uuid() {
    char *u = g_uuid_string_random();
    std::string r = u;
    g_free(u);
    return r;
}
std::string sanitize_id(const std::string &id) {
    std::string r;
    for (char c : id) r.push_back((g_ascii_isalnum(c) || c == '.' || c == '-' || c == '_' || c == '@') ? c : '_');
    if (r.empty() || r[0] == '.') r.insert(r.begin(), '_');
    return r;
}

// Match pattern ("*://*.example.com/*", "<all_urls>") -> WebKit user-content patterns.
void convert_pattern(const std::string &p, std::vector<std::string> &out) {
    if (p == "<all_urls>") { out.push_back("http://*/*"); out.push_back("https://*/*"); out.push_back("file:///*"); return; }
    size_t sep = p.find("://");
    if (sep == std::string::npos) return;
    std::string scheme = p.substr(0, sep), rest = p.substr(sep + 3);
    if (scheme == "*") {
        out.push_back("http://" + rest);
        out.push_back("https://" + rest);
    } else if (scheme == "http" || scheme == "https" || scheme == "file" || scheme == "ftp") out.push_back(p);
}
std::vector<char *> to_cstrv(std::vector<std::string> &v) {
    std::vector<char *> r;
    for (auto &s : v) r.push_back(const_cast<char *>(s.c_str()));
    r.push_back(nullptr);
    return r;
}

}  // namespace

// ===================================================================== model

struct ContentScript {
    std::vector<std::string> matches, excludes, js, css;
    std::string run_at = "document_idle";
    bool all_frames = false;
};

struct Ext {
    std::string id, uuid, handler, dir, name, version;
    Json manifest;
    int mv = 2;
    std::vector<ContentScript> scripts;
    std::string bg_page;
    std::vector<std::string> bg_scripts;
    bool bg_module = false;
    bool enabled = true;
    std::string pending_event;  // "install" or "update", delivered once to onInstalled
    std::string messages_json = "null";
    std::map<std::string, Json> storage;  // area -> JSON object node
    std::map<std::string, std::string> action;
    WebKitWebView *bg = nullptr;
    WebKitUserContentManager *bg_ucm = nullptr;
    bool bg_started = false;
    bool bg_ready = false;
    struct MenuRec { std::string id, title, parent, type = "normal"; std::vector<std::string> contexts{"page"}; bool enabled = true, visible = true, checked = false; };
    std::vector<MenuRec> menus;
    std::vector<WebKitWebView *> page_views;  // open popup / options pages (targets for background -> page messages)
    std::vector<std::function<void()>> waiting;  // messages sent before the background page finished loading

    // toolbar action / extension pages
    bool webreq_listening = false;  // a webRequest listener is registered (observe-only)
    bool has_action = false;
    std::string popup, options_page, default_title, icon_rel;
    // declarativeNetRequest
    struct Ruleset { std::string id, path; bool enabled = false; };
    std::vector<Ruleset> rulesets;
    std::string dynamic_rules = "[]", session_rules = "[]";
    WebKitUserContentFilter *dnr_filter = nullptr;
    unsigned dnr_generation = 0;
    // scripts registered at runtime (scripting.registerContentScripts / contentScripts.register)
    struct Reg {
        std::string id, raw;  // raw = the JSON the extension passed, so update can merge into it
        std::vector<std::string> matches, excludes;
        std::string js, css, run_at = "document_idle", world = "ISOLATED";
        bool all_frames = false, persist = true;
        std::vector<std::pair<WebKitUserContentManager *, WebKitUserScript *>> scripts;
        std::vector<std::pair<WebKitUserContentManager *, WebKitUserStyleSheet *>> sheets;
    };
    std::vector<Reg> registered;
    bool has_permission(const std::string &perm) const;
};

struct BgCtx { ExtensionManager::Impl *mgr; Ext *ext; };

struct MsgCtx {
    ExtensionManager::Impl *mgr;
    Ext *ext;
    int tab_id;  // -1 for the background page
    WebKitWebView *view;
    bool page = false;  // popup / options page (privileged like the background, but can message it)
};

struct PortEnd { WebKitWebView *view = nullptr; std::string world; };
struct PortRec { PortEnd a, b; };

struct ReplyHandle {
    WebKitScriptMessageReply *reply;
    JSCContext *jctx;
};

struct ExtensionManager::Impl {
    std::string data_dir, ext_dir, store_dir, registry_path;
    WebKitNetworkSession *session;
    Host host;
    std::vector<std::unique_ptr<Ext>> exts;
    struct ViewInfo { WebKitUserContentManager *ucm; int tab_id; std::vector<Ext *> attached; };
    std::map<WebKitWebView *, ViewInfo> views;
    std::map<int, WebKitWebView *> tab_views;
    bool scheme_registered = false;
    std::map<std::string, PortRec> ports;
    WebKitUserContentFilterStore *filter_store = nullptr;
    std::function<void()> on_actions_changed;
    std::function<void(const std::string &)> on_open_options;

    // pages, ports, scripting, DNR (implemented in the "extension pages" section below)
    WebKitWebView *make_page_view(Ext &e, const char *ctx);
    PortEnd end_of(MsgCtx *c);
    void deliver_to(Ext &e, const PortEnd &to, const std::string &body, const std::string &param);
    void drop_ports_for(WebKitWebView *view);
    bool dispatch_cookies(MsgCtx *c, const std::string &op, JsonObject *args, ReplyHandle *r);
    bool dispatch_extra(MsgCtx *c, const std::string &op, JsonObject *args, const std::string &sender_url, bool top, ReplyHandle *r);
    WebKitWebView *tab_view_for(JsonNode *id_node);
    void add_registered_to(Ext &e, Ext::Reg &g, WebKitUserContentManager *ucm);
    void drop_registered(Ext &e, Ext::Reg &g);
    bool register_script(Ext &e, JsonObject *o, std::string *err);
    void save_registered(Ext &e);
    void rebuild_dnr(Ext &e);
    void load_dnr_state(Ext &e);
    bool views_has_ucm(WebKitUserContentManager *u) { for (auto &[v, i] : views) { (void)v; if (i.ucm == u) return true; } return false; }
    void notify_actions() { if (on_actions_changed) on_actions_changed(); }

    // registry
    void load_registry();
    void save_registry();
    bool load_manifest(Ext &e, std::string *err);
    std::string localize(const Ext &e, const std::string &s);
    Ext *by_uuid(const std::string &u) { for (auto &e : exts) if (e->uuid == u) return e.get(); return nullptr; }
    Ext *by_name(const std::string &n);

    // runtime
    void register_scheme();
    void start_background(Ext &e);
    std::string polyfill(const Ext &e, const char *ctx);
    void handle_scheme(WebKitURISchemeRequest *req);
    void on_message(MsgCtx *c, JSCValue *value, WebKitScriptMessageReply *reply);
    void dispatch(MsgCtx *c, const std::string &op, JsonObject *args, const std::string &sender_url, bool top, ReplyHandle *r);
    void respond(ReplyHandle *r, const std::string &envelope);
    void respond_ok(ReplyHandle *r, const std::string &value_json) { respond(r, "{\"ok\":true,\"value\":" + value_json + "}"); }
    void respond_err(ReplyHandle *r, const std::string &msg) { respond(r, "{\"ok\":false,\"error\":" + quote(msg) + "}"); }
    std::string tab_json(const TabInfo &t);
    std::string sender_json(MsgCtx *c, const std::string &url, bool top);
    void run_js(WebKitWebView *view, const std::string &world, const std::string &body, const std::string &param,
                std::function<void(bool ok, const std::string &json_or_error)> done);
    void fire(Ext &e, const std::string &name, const std::string &args_json, bool to_tabs);
    void fire_all(const std::string &name, const std::string &args_json);

    // storage
    JsonObject *store_obj(Ext &e, const std::string &area);
    void persist(Ext &e, const std::string &area);
    void storage_op(Ext &e, const std::string &op, JsonObject *args, ReplyHandle *r);
};

// ------------------------------------------------------------------- registry

void ExtensionManager::Impl::load_registry() {
    exts.clear();
    Json root = parse(read_file(registry_path));
    JsonArray *a = root.arr();
    if (!a) return;
    for (guint i = 0; i < json_array_get_length(a); ++i) {
        JsonObject *o = json_node_get_object(json_array_get_element(a, i));
        if (!o) continue;
        auto e = std::make_unique<Ext>();
        e->id = str_of(o, "id");
        e->uuid = str_of(o, "uuid");
        e->dir = ext_dir + "/" + str_of(o, "dir");
        e->enabled = bool_of(o, "enabled", true);
        e->pending_event = str_of(o, "pending_event");
        if (e->id.empty() || e->uuid.empty()) continue;
        e->handler = "reed_" + std::string(e->uuid);
        e->handler.erase(std::remove(e->handler.begin(), e->handler.end(), '-'), e->handler.end());
        std::string err;
        if (!load_manifest(*e, &err)) { g_warning("extension %s: %s", e->id.c_str(), err.c_str()); continue; }
        exts.push_back(std::move(e));
    }
}

void ExtensionManager::Impl::save_registry() {
    std::string out = "[";
    bool first = true;
    for (auto &e : exts) {
        if (!first) out += ",";
        first = false;
        out += "{\"id\":" + quote(e->id) + ",\"uuid\":" + quote(e->uuid) + ",\"dir\":" + quote(fs::path(e->dir).filename().string()) +
               ",\"enabled\":" + (e->enabled ? "true" : "false") + ",\"pending_event\":" + quote(e->pending_event) + "}";
    }
    out += "]";
    write_file(registry_path, out);
}

std::string ExtensionManager::Impl::localize(const Ext &e, const std::string &s) {
    if (s.rfind("__MSG_", 0) != 0 || s.size() < 8 || s.compare(s.size() - 2, 2, "__") != 0) return s;
    std::string key = s.substr(6, s.size() - 8);
    Json m = parse(e.messages_json);
    JsonNode *node = member(m.obj(), key.c_str());
    if (!node) {
        // keys are case-insensitive in the spec
        JsonObject *o = m.obj();
        if (o) for (GList *l = json_object_get_members(o); l; l = l->next)
            if (g_ascii_strcasecmp(static_cast<char *>(l->data), key.c_str()) == 0) { node = json_object_get_member(o, static_cast<char *>(l->data)); break; }
    }
    return node ? str_of(json_node_get_object(node), "message", s) : s;
}

bool ExtensionManager::Impl::load_manifest(Ext &e, std::string *err) {
    e.manifest = parse(read_file(e.dir + "/manifest.json"));
    JsonObject *m = e.manifest.obj();
    if (!m) { *err = "manifest.json missing or invalid"; return false; }
    JsonNode *mvn = member(m, "manifest_version");
    e.mv = mvn ? static_cast<int>(json_node_get_int(mvn)) : 0;
    if (e.mv != 2 && e.mv != 3) { *err = "unsupported manifest_version"; return false; }

    std::string loc = str_of(m, "default_locale", "en");
    for (const std::string &l : {loc, std::string("en"), std::string("en_US")}) {
        std::string p = e.dir + "/_locales/" + l + "/messages.json";
        if (fs::exists(p)) { e.messages_json = read_file(p); break; }
    }
    e.name = localize(e, str_of(m, "name", e.id));
    e.version = str_of(m, "version", "0");

    // background
    if (JsonNode *bg = member(m, "background"); bg && JSON_NODE_HOLDS_OBJECT(bg)) {
        JsonObject *b = json_node_get_object(bg);
        e.bg_page = str_of(b, "page");
        e.bg_scripts = strings_of(b, "scripts");
        if (e.bg_scripts.empty()) e.bg_scripts = strings_of(b, "service_worker");  // MV3: run as a page script
        e.bg_module = str_of(b, "type") == "module";
    }
    // toolbar action, options page, DNR rulesets
    e.has_action = false; e.popup.clear(); e.default_title.clear(); e.icon_rel.clear(); e.options_page.clear(); e.rulesets.clear();
    for (const char *k : {"action", "browser_action", "page_action"}) {
        JsonNode *an = member(m, k);
        if (!an || !JSON_NODE_HOLDS_OBJECT(an)) continue;
        JsonObject *a = json_node_get_object(an);
        e.has_action = true;
        if (e.popup.empty()) e.popup = str_of(a, "default_popup");
        if (e.default_title.empty()) e.default_title = localize(e, str_of(a, "default_title"));
        if (e.icon_rel.empty()) {
            JsonNode *ic = member(a, "default_icon");
            if (ic && JSON_NODE_HOLDS_VALUE(ic) && json_node_get_value_type(ic) == G_TYPE_STRING) e.icon_rel = json_node_get_string(ic);
            else if (ic && JSON_NODE_HOLDS_OBJECT(ic)) {
                int best = -1;
                JsonObject *io = json_node_get_object(ic);
                GList *mem = json_object_get_members(io);
                for (GList *l = mem; l; l = l->next) {
                    int sz = std::atoi(static_cast<const char *>(l->data));
                    std::string f = str_of(io, static_cast<const char *>(l->data));
                    bool svg = f.size() > 4 && f.compare(f.size() - 4, 4, ".svg") == 0;
                    int score = svg ? 1000 : sz;
                    if (!f.empty() && sz >= 0 && score > best && (svg || sz <= 64 || best < 0)) { best = score; e.icon_rel = f; }
                }
                g_list_free(mem);
            }
        }
    }
    if (JsonNode *oi = member(m, "options_ui"); oi && JSON_NODE_HOLDS_OBJECT(oi)) e.options_page = str_of(json_node_get_object(oi), "page");
    if (e.options_page.empty()) e.options_page = str_of(m, "options_page");
    if (JsonNode *dn = member(m, "declarative_net_request"); dn && JSON_NODE_HOLDS_OBJECT(dn)) {
        JsonNode *rr = member(json_node_get_object(dn), "rule_resources");
        if (rr && JSON_NODE_HOLDS_ARRAY(rr)) {
            JsonArray *ra = json_node_get_array(rr);
            for (guint i = 0; i < json_array_get_length(ra); ++i) {
                JsonObject *o = json_node_get_object(json_array_get_element(ra, i));
                if (!o) continue;
                Ext::Ruleset rs; rs.id = str_of(o, "id"); rs.path = str_of(o, "path"); rs.enabled = bool_of(o, "enabled", false);
                if (!rs.id.empty() && !rs.path.empty()) e.rulesets.push_back(rs);
            }
        }
    }

    // content scripts
    e.scripts.clear();
    if (JsonNode *cs = member(m, "content_scripts"); cs && JSON_NODE_HOLDS_ARRAY(cs)) {
        JsonArray *a = json_node_get_array(cs);
        for (guint i = 0; i < json_array_get_length(a); ++i) {
            JsonObject *o = json_node_get_object(json_array_get_element(a, i));
            if (!o) continue;
            ContentScript s;
            s.matches = strings_of(o, "matches");
            s.excludes = strings_of(o, "exclude_matches");
            s.js = strings_of(o, "js");
            s.css = strings_of(o, "css");
            s.run_at = str_of(o, "run_at", "document_idle");
            s.all_frames = bool_of(o, "all_frames", false);
            e.scripts.push_back(std::move(s));
        }
    }
    return true;
}

bool Ext::has_permission(const std::string &perm) const {
    JsonObject *m = manifest.obj();
    for (const char *k : {"permissions", "optional_permissions"})
        for (const auto &p : strings_of(m, k)) if (p == perm) return true;
    return false;
}

ExtensionManager::ExtensionManager(std::string data_dir, WebKitNetworkSession *session, Host host) : d_(new Impl) {
    d_->data_dir = std::move(data_dir);
    d_->ext_dir = d_->data_dir + "/extensions";
    d_->store_dir = d_->data_dir + "/extension-data";
    d_->registry_path = d_->ext_dir + "/registry.json";
    d_->session = session;
    d_->host = std::move(host);
    std::error_code ec;
    fs::create_directories(d_->ext_dir, ec);
    d_->load_registry();
}
ExtensionManager::~ExtensionManager() {
    for (auto &e : d_->exts) if (e->bg) g_object_unref(e->bg);
}
int ExtensionManager::extension_count() const { return static_cast<int>(d_->exts.size()); }


Ext *ExtensionManager::Impl::by_name(const std::string &n) {
    for (auto &e : exts) if (e->id == n || e->name == n || e->uuid == n) return e.get();
    return nullptr;
}

std::string ExtensionManager::describe() const {
    std::string r;
    for (auto &e : d_->exts)
        r += e->name + " " + e->version + "\n  id:   " + e->id + "\n  uuid: " + e->uuid + "\n  state: " + (e->enabled ? "enabled" : "disabled") +
             "\n  scripts: " + std::to_string(e->scripts.size()) + " content-script group(s), background " +
             ((e->bg_page.empty() && e->bg_scripts.empty()) ? "none" : "yes") + "\n";
    if (r.empty()) r = "No extensions installed.\n";
    return r;
}

// -------------------------------------------------------------------- install

// -------------------------------------------------------------------- AMO download support

static size_t curl_to_string(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}
static size_t curl_to_file(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE *>(userdata));
}

static bool is_https_url(const std::string &url) {
    return url.rfind("https://", 0) == 0;
}

static bool download_https_file(const std::string &url, const std::string &destination, std::string *err) {
    if (!is_https_url(url)) { *err = "Only HTTPS downloads are allowed."; return false; }
    static const CURLcode curl_init_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_init_status != CURLE_OK) { *err = "Could not initialize HTTPS support."; return false; }
    FILE *out = std::fopen(destination.c_str(), "wb");
    if (!out) { *err = "Could not create temporary XPI file."; return false; }
    CURL *curl = curl_easy_init();
    if (!curl) { std::fclose(out); *err = "Could not initialize XPI downloader."; return false; }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReedBrowser/0.5.2 (Firefox Add-ons installer)");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    bool file_ok = std::fclose(out) == 0;
    if (rc != CURLE_OK || status < 200 || status >= 300 || !file_ok || fs::file_size(destination) < 128) {
        std::error_code ec; fs::remove(destination, ec);
        *err = "XPI download failed (" + std::string(curl_easy_strerror(rc)) + ", HTTP " + std::to_string(status) + ").";
        return false;
    }
    return true;
}

static bool download_amo_xpi(const std::string &input, const std::string &destination, std::string *err) {
    // Accept an AMO add-on page or a direct AMO API add-on slug. Do not accept
    // arbitrary remote URLs here: the API response is constrained to HTTPS.
    std::string slug;
    const std::string marker = "/firefox/addon/";
    auto pos = input.find(marker);
    if (pos != std::string::npos) {
        std::string tail = input.substr(pos + marker.size());
        auto end = tail.find_first_of("/?#");
        slug = tail.substr(0, end);
    } else if (input.rfind("amo:", 0) == 0) {
        slug = input.substr(4);
    }
    while (!slug.empty() && slug.back() == '/') slug.pop_back();
    if (slug.empty() || slug.size() > 200 || !std::all_of(slug.begin(), slug.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        })) {
        *err = "Expected a Firefox Add-ons page URL, e.g. https://addons.mozilla.org/firefox/addon/ublock-origin/, or amo:<slug>.";
        return false;
    }

    static const CURLcode curl_init_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_init_status != CURLE_OK) { *err = "Could not initialize HTTPS support."; return false; }
    CURL *curl = curl_easy_init();
    if (!curl) { *err = "Could not initialize HTTPS downloader."; return false; }
    std::string api = "https://addons.mozilla.org/api/v5/addons/addon/" + slug + "/";
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, api.c_str());
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ReedBrowser/0.5.2 (Firefox Add-ons installer)");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status < 200 || status >= 300) {
        *err = "Could not retrieve add-on from Firefox Add-ons (" + std::string(curl_easy_strerror(rc)) + ", HTTP " + std::to_string(status) + ").";
        return false;
    }

    Json api_json = parse(body);
    JsonObject *root = api_json.obj();
    JsonNode *version_node = member(root, "current_version");
    JsonObject *version = version_node && JSON_NODE_HOLDS_OBJECT(version_node) ? json_node_get_object(version_node) : nullptr;

    // AMO's v5 detail response exposes the downloadable package as
    // current_version.file.url. Older/alternate responses may expose a
    // files[] collection, so accept both shapes without trusting arbitrary
    // URLs from the add-on page itself.
    std::string xpi_url;
    JsonNode *file_node = member(version, "file");
    JsonObject *file_obj = file_node && JSON_NODE_HOLDS_OBJECT(file_node) ? json_node_get_object(file_node) : nullptr;
    std::string current_file_url = str_of(file_obj, "url");
    if (is_https_url(current_file_url)) xpi_url = current_file_url;

    if (xpi_url.empty()) {
        JsonNode *files_node = member(version, "files");
        JsonArray *files = files_node && JSON_NODE_HOLDS_ARRAY(files_node) ? json_node_get_array(files_node) : nullptr;
        if (files) {
            for (guint i = 0; i < json_array_get_length(files); ++i) {
                JsonNode *candidate = json_array_get_element(files, i);
                JsonObject *candidate_obj = candidate && JSON_NODE_HOLDS_OBJECT(candidate) ? json_node_get_object(candidate) : nullptr;
                std::string candidate_url = str_of(candidate_obj, "url");
                if (is_https_url(candidate_url)) { xpi_url = candidate_url; break; }
            }
        }
    }
    if (!is_https_url(xpi_url)) {
        *err = "Firefox Add-ons did not return a valid HTTPS XPI download for '" + slug + "'.";
        return false;
    }

    return download_https_file(xpi_url, destination, err);
}

static bool extract_archive(const std::string &file, const std::string &dest, std::string *err) {
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, file.c_str(), 65536) != ARCHIVE_OK) { *err = archive_error_string(a); archive_read_free(a); return false; }
    struct archive_entry *entry;
    bool ok = true;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string rel = archive_entry_pathname(entry) ? archive_entry_pathname(entry) : "";
        fs::path p = fs::path(rel).lexically_normal();
        bool bad = rel.empty() || p.is_absolute() || *p.begin() == "..";
        for (auto &part : p) if (part == "..") bad = true;
        if (bad) { *err = "archive contains an unsafe path: " + rel; ok = false; break; }
        fs::path target = fs::path(dest) / p;
        if (archive_entry_filetype(entry) == AE_IFDIR) { std::error_code ec; fs::create_directories(target, ec); continue; }
        if (archive_entry_filetype(entry) != AE_IFREG) continue;  // no symlinks/devices
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        const void *buf; size_t size; la_int64_t off;
        while (archive_read_data_block(a, &buf, &size, &off) == ARCHIVE_OK) out.write(static_cast<const char *>(buf), static_cast<std::streamsize>(size));
    }
    archive_read_free(a);
    return ok;
}

bool ExtensionManager::fetch_package(const std::string &source, const std::string &destination, std::string *err) {
    if (source.rfind("https://", 0) == 0 && source.find("/addon/") == std::string::npos) return download_https_file(source, destination, err);
    return download_amo_xpi(source, destination, err);
}

bool ExtensionManager::install(const std::string &path, bool assume_yes, std::string *message) {
    std::error_code ec;
    if (path.rfind("https://addons.mozilla.org/", 0) == 0 || path.rfind("amo:", 0) == 0) {
        std::string temp = d_->data_dir + "/amo-download-" + std::to_string(getpid()) + ".xpi";
        std::string download_error;
        if (!download_amo_xpi(path, temp, &download_error)) { *message = download_error; return false; }
        struct RemoveTemp { std::string p; ~RemoveTemp() { std::error_code e; fs::remove(p, e); } } cleanup_download{temp};
        return install(temp, assume_yes, message);
    }
    if (!fs::exists(path)) { *message = "No such file or directory: " + path; return false; }
    std::string staging = d_->ext_dir + "/.staging-" + std::to_string(getpid());
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    struct Cleanup { std::string p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{staging};

    if (fs::is_directory(path)) fs::copy(path, staging, fs::copy_options::recursive, ec);
    else { std::string err; if (!extract_archive(path, staging, &err)) { *message = "Could not unpack extension: " + err; return false; } }
    if (ec) { *message = "Could not copy extension: " + ec.message(); return false; }

    Ext probe;
    probe.dir = staging;
    std::string err;
    if (!d_->load_manifest(probe, &err)) { *message = "Not a valid extension: " + err; return false; }
    JsonObject *m = probe.manifest.obj();
    std::string id;
    for (const char *k : {"browser_specific_settings", "applications"}) {
        if (JsonNode *n = member(m, k); n && JSON_NODE_HOLDS_OBJECT(n))
            if (JsonNode *g = member(json_node_get_object(n), "gecko"); g && JSON_NODE_HOLDS_OBJECT(g)) id = str_of(json_node_get_object(g), "id");
        if (!id.empty()) break;
    }
    if (id.empty()) {
        gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, (probe.name + probe.version).c_str(), -1);
        id = "ext-" + std::string(sum).substr(0, 12);
        g_free(sum);
    }

    std::string perms;
    for (const auto &p : strings_of(m, "permissions")) perms += "    " + p + "\n";
    for (const auto &p : strings_of(m, "host_permissions")) perms += "    " + p + "\n";
    std::string report = "Extension: " + probe.name + " " + probe.version + "\n  id: " + id +
                         "\n  manifest version: " + std::to_string(probe.mv) + "\n  requests:\n" + (perms.empty() ? "    (none)\n" : perms);
    if (!probe.scripts.empty()) {
        report += "  runs scripts on:\n";
        std::set<std::string> seen;
        for (auto &s : probe.scripts) for (auto &p : s.matches) if (seen.insert(p).second) report += "    " + p + "\n";
    }
    report += "  Note: permissions are granted as listed; per-site permission prompts are not implemented yet.\n";
    if (!assume_yes) {
        std::cerr << report << "Install this extension? [y/N] " << std::flush;
        std::string ans;
        if (!isatty(0) || !std::getline(std::cin, ans) || (ans != "y" && ans != "Y" && ans != "yes")) { *message = "Cancelled (nothing installed)."; return false; }
    }

    std::string dirname = sanitize_id(id);
    std::string final_dir = d_->ext_dir + "/" + dirname;
    Ext *existing = d_->by_name(id);
    std::string event = existing ? "update" : "install";
    fs::remove_all(final_dir, ec);
    fs::rename(staging, final_dir, ec);
    if (ec) { fs::copy(staging, final_dir, fs::copy_options::recursive, ec); if (ec) { *message = "Could not install: " + ec.message(); return false; } }

    if (existing) { existing->dir = final_dir; existing->pending_event = event; std::string e2; d_->load_manifest(*existing, &e2); }
    else {
        auto e = std::make_unique<Ext>();
        e->id = id; e->uuid = random_uuid(); e->dir = final_dir; e->pending_event = event;
        e->handler = "reed_" + e->uuid;
        e->handler.erase(std::remove(e->handler.begin(), e->handler.end(), '-'), e->handler.end());
        std::string e2;
        d_->load_manifest(*e, &e2);
        d_->exts.push_back(std::move(e));
    }
    d_->save_registry();
    *message = "Installed " + probe.name + " (" + id + "). Restart ReedBrowser to load it.";
    return true;
}

bool ExtensionManager::remove(const std::string &which, std::string *message) {
    Ext *e = d_->by_name(which);
    if (!e) { *message = "No extension matches '" + which + "'."; return false; }
    std::error_code ec;
    fs::remove_all(e->dir, ec);
    fs::remove_all(d_->store_dir + "/" + e->uuid, ec);
    std::string name = e->name;
    d_->exts.erase(std::remove_if(d_->exts.begin(), d_->exts.end(), [&](auto &p) { return p.get() == e; }), d_->exts.end());
    d_->save_registry();
    *message = "Removed " + name + ".";
    return true;
}

// ---------------------------------------------------------------- scheme / files

static std::string mime_for(const std::string &path, const std::string &data) {
    static const std::map<std::string, std::string> known = {
        {".js", "text/javascript"}, {".mjs", "text/javascript"}, {".json", "application/json"}, {".html", "text/html"},
        {".htm", "text/html"}, {".css", "text/css"}, {".svg", "image/svg+xml"}, {".wasm", "application/wasm"},
        {".png", "image/png"}, {".txt", "text/plain"}, {".woff2", "font/woff2"}, {".woff", "font/woff"}};
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (auto it = known.find(ext); it != known.end()) return it->second;
    gboolean uncertain = FALSE;
    char *ct = g_content_type_guess(path.c_str(), reinterpret_cast<const guchar *>(data.data()), std::min<size_t>(data.size(), 512), &uncertain);
    char *mime = g_content_type_get_mime_type(ct);
    std::string r = mime ? mime : "application/octet-stream";
    g_free(ct); g_free(mime);
    return r;
}

void ExtensionManager::Impl::handle_scheme(WebKitURISchemeRequest *req) {
    const char *uri = webkit_uri_scheme_request_get_uri(req);
    GError *gerr = nullptr;
    GUri *g = g_uri_parse(uri, G_URI_FLAGS_NONE, &gerr);
    auto fail = [&](int code, const char *msg) {
        GError *e = g_error_new_literal(G_IO_ERROR, code, msg);
        webkit_uri_scheme_request_finish_error(req, e);
        g_error_free(e);
    };
    if (!g) { if (gerr) g_error_free(gerr); fail(G_IO_ERROR_INVALID_ARGUMENT, "bad extension URL"); return; }
    std::string host = g_uri_get_host(g) ? g_uri_get_host(g) : "";
    char *decoded = g_uri_unescape_string(g_uri_get_path(g), nullptr);
    std::string path = decoded ? decoded : "";
    g_free(decoded);
    g_uri_unref(g);
    Ext *e = by_uuid(host);
    if (!e) { fail(G_IO_ERROR_NOT_FOUND, "unknown extension"); return; }

    std::string data;
    if (path == "/_generated_background_page.html") {
        data = "<!doctype html><meta charset=\"utf-8\"><title>Background</title><body>";
        for (auto &s : e->bg_scripts) data += std::string("<script") + (e->bg_module ? " type=\"module\"" : "") + " src=\"" + s + "\"></script>";
    } else {
        fs::path rel = fs::path(path.empty() ? "/" : path).relative_path().lexically_normal();
        for (auto &part : rel) if (part == "..") { fail(G_IO_ERROR_PERMISSION_DENIED, "path escapes extension"); return; }
        std::error_code ec;
        fs::path full = fs::weakly_canonical(fs::path(e->dir) / rel, ec);
        fs::path root = fs::weakly_canonical(e->dir, ec);
        if (full.string().rfind(root.string(), 0) != 0 || !fs::is_regular_file(full)) { fail(G_IO_ERROR_NOT_FOUND, "not found"); return; }
        data = read_file(full.string());
        path = full.string();
    }
    std::string mime = mime_for(path, data);
    gsize len = data.size();
    gchar *copy = static_cast<gchar *>(g_memdup2(data.data(), len ? len : 1));
    GInputStream *stream = g_memory_input_stream_new_from_data(copy, static_cast<gssize>(len), g_free);
    webkit_uri_scheme_request_finish(req, stream, static_cast<gint64>(len), mime.c_str());
    g_object_unref(stream);
}

void ExtensionManager::Impl::register_scheme() {
    if (scheme_registered) return;
    scheme_registered = true;
    WebKitWebContext *wc = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(wc, "moz-extension",
        [](WebKitURISchemeRequest *req, gpointer ud) { static_cast<Impl *>(ud)->handle_scheme(req); }, this, nullptr);
    WebKitSecurityManager *sm = webkit_web_context_get_security_manager(wc);
    webkit_security_manager_register_uri_scheme_as_secure(sm, "moz-extension");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, "moz-extension");
}

// --------------------------------------------------------------- JS plumbing

std::string ExtensionManager::Impl::polyfill(const Ext &e, const char *ctx) {
    bool bg = std::string(ctx) != "content";
    std::string cfg = "{\"ctx\":" + quote(ctx) + ",\"id\":" + quote(e.id) + ",\"uuid\":" + quote(e.uuid) + ",\"handler\":" + quote(e.handler) +
                      ",\"baseUrl\":" + quote("moz-extension://" + e.uuid + "/") + ",\"version\":" + quote(REEDBROWSER_EXT_VERSION) +
                      ",\"manifest\":" + dump(e.manifest.n) + ",\"messages\":" + (bg ? e.messages_json : std::string("null")) + "}";
    return "(function(__cfg){\n" + std::string(kReedApiJs) + "\n})(" + cfg + ");";
}

void ExtensionManager::Impl::run_js(WebKitWebView *view, const std::string &world, const std::string &body, const std::string &param,
                                    std::function<void(bool, const std::string &)> done) {
    struct Cb { std::function<void(bool, const std::string &)> fn; };
    auto *cb = new Cb{std::move(done)};
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "p", g_variant_new_string(param.c_str()));
    webkit_web_view_call_async_javascript_function(
        view, body.c_str(), -1, g_variant_builder_end(&b), world.empty() ? nullptr : world.c_str(), nullptr, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer ud) {
            std::unique_ptr<Cb> cb(static_cast<Cb *>(ud));
            GError *err = nullptr;
            JSCValue *v = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(src), res, &err);
            if (err) { std::string m = err->message; g_error_free(err); if (cb->fn) cb->fn(false, m); return; }
            std::string json = "null";
            if (v) {
                if (!jsc_value_is_undefined(v) && !jsc_value_is_null(v)) { char *s = jsc_value_to_json(v, 0); if (s) json = s; g_free(s); }
                g_object_unref(v);
            }
            if (cb->fn) cb->fn(true, json);
        }, cb);
}

void ExtensionManager::Impl::fire(Ext &e, const std::string &name, const std::string &args_json, bool to_tabs) {
    std::string body = "window.__reedFire(" + quote(name) + ", JSON.parse(p)); return null;";
    if (e.bg) run_js(e.bg, "", body, args_json, nullptr);
    if (!to_tabs) return;
    for (auto &[view, info] : views)
        if (std::find(info.attached.begin(), info.attached.end(), &e) != info.attached.end())
            run_js(view, "reed-ext-" + e.uuid, body, args_json, nullptr);
}
void ExtensionManager::Impl::fire_all(const std::string &name, const std::string &args_json) {
    for (auto &e : exts) if (e->enabled && e->bg) fire(*e, name, args_json, false);
}

void ExtensionManager::Impl::respond(ReplyHandle *r, const std::string &envelope) {
    JSCValue *v = jsc_value_new_from_json(r->jctx, envelope.c_str());
    if (v) { webkit_script_message_reply_return_value(r->reply, v); g_object_unref(v); }
    else webkit_script_message_reply_return_error_message(r->reply, "bad reply");
    webkit_script_message_reply_unref(r->reply);
    g_object_unref(r->jctx);
    delete r;
}

std::string ExtensionManager::Impl::tab_json(const TabInfo &t) {
    return "{\"id\":" + std::to_string(t.id) + ",\"windowId\":1,\"index\":0,\"url\":" + quote(t.url) + ",\"title\":" + quote(t.title) +
           ",\"active\":" + (t.active ? "true" : "false") + ",\"highlighted\":" + (t.active ? "true" : "false") +
           ",\"incognito\":false,\"pinned\":false,\"status\":\"complete\"}";
}

std::string ExtensionManager::Impl::sender_json(MsgCtx *c, const std::string &url, bool top) {
    std::string s = "{\"id\":" + quote(c->ext->id) + ",\"url\":" + quote(url) + ",\"frameId\":" + (top ? "0" : "1");
    if (c->tab_id >= 0 && host.list_tabs)
        for (auto &t : host.list_tabs()) if (t.id == c->tab_id) s += ",\"tab\":" + tab_json(t);
    return s + "}";
}

// ---------------------------------------------------------------------- storage

JsonObject *ExtensionManager::Impl::store_obj(Ext &e, const std::string &area) {
    auto it = e.storage.find(area);
    if (it == e.storage.end()) {
        Json node;
        if (area != "session") node = parse(read_file(store_dir + "/" + e.uuid + "/" + area + ".json"));
        if (!node.obj()) node = Json(json_node_init_object(json_node_alloc(), json_object_new()));
        it = e.storage.emplace(area, std::move(node)).first;
    }
    return it->second.obj();
}
void ExtensionManager::Impl::persist(Ext &e, const std::string &area) {
    if (area == "session") return;
    write_file(store_dir + "/" + e.uuid + "/" + area + ".json", dump(e.storage[area].n));
}

void ExtensionManager::Impl::storage_op(Ext &e, const std::string &op, JsonObject *args, ReplyHandle *r) {
    std::string area = str_of(args, "area", "local");
    if (area != "local" && area != "sync" && area != "session") { respond_err(r, "unknown storage area"); return; }
    JsonObject *store = store_obj(e, area);
    auto copy_into = [](JsonObject *dst, const char *k, JsonNode *n) { json_object_set_member(dst, k, json_node_copy(n)); };

    if (op == "storage.get") {
        JsonNode *keys = member(args, "keys");
        JsonObject *res = json_object_new();
        if (!keys || JSON_NODE_HOLDS_NULL(keys)) {
            for (GList *l = json_object_get_members(store); l; l = l->next) copy_into(res, static_cast<char *>(l->data), json_object_get_member(store, static_cast<char *>(l->data)));
        } else if (JSON_NODE_HOLDS_OBJECT(keys)) {  // defaults
            JsonObject *def = json_node_get_object(keys);
            for (GList *l = json_object_get_members(def); l; l = l->next) {
                const char *k = static_cast<char *>(l->data);
                copy_into(res, k, json_object_has_member(store, k) ? json_object_get_member(store, k) : json_object_get_member(def, k));
            }
        } else {
            std::vector<std::string> ks;
            if (JSON_NODE_HOLDS_ARRAY(keys)) { JsonArray *a = json_node_get_array(keys); for (guint i = 0; i < json_array_get_length(a); ++i) ks.push_back(json_node_get_string(json_array_get_element(a, i))); }
            else ks.push_back(json_node_get_string(keys));
            for (auto &k : ks) if (json_object_has_member(store, k.c_str())) copy_into(res, k.c_str(), json_object_get_member(store, k.c_str()));
        }
        JsonNode *n = json_node_alloc(); json_node_init_object(n, res); json_object_unref(res);
        respond_ok(r, dump(n)); json_node_unref(n);
        return;
    }

    std::string changes = "{";
    bool first = true;
    auto note = [&](const char *k, JsonNode *old_v, JsonNode *new_v) {
        if (!first) changes += ",";
        first = false;
        changes += quote(k) + ":{";
        if (old_v) changes += "\"oldValue\":" + dump(old_v);
        if (new_v) changes += std::string(old_v ? "," : "") + "\"newValue\":" + dump(new_v);
        changes += "}";
    };
    if (op == "storage.set") {
        JsonObject *items = json_node_get_object(member(args, "items") ? member(args, "items") : nullptr);
        if (items) for (GList *l = json_object_get_members(items); l; l = l->next) {
            const char *k = static_cast<char *>(l->data);
            JsonNode *old_v = json_object_has_member(store, k) ? json_node_copy(json_object_get_member(store, k)) : nullptr;
            JsonNode *new_v = json_object_get_member(items, k);
            if (old_v && dump(old_v) == dump(new_v)) { json_node_unref(old_v); continue; }
            note(k, old_v, new_v);
            copy_into(store, k, new_v);
            if (old_v) json_node_unref(old_v);
        }
    } else if (op == "storage.remove") {
        JsonNode *keys = member(args, "keys");
        std::vector<std::string> ks;
        if (keys && JSON_NODE_HOLDS_ARRAY(keys)) { JsonArray *a = json_node_get_array(keys); for (guint i = 0; i < json_array_get_length(a); ++i) ks.push_back(json_node_get_string(json_array_get_element(a, i))); }
        else if (keys && JSON_NODE_HOLDS_VALUE(keys)) ks.push_back(json_node_get_string(keys));
        for (auto &k : ks) if (json_object_has_member(store, k.c_str())) {
            JsonNode *old_v = json_node_copy(json_object_get_member(store, k.c_str()));
            note(k.c_str(), old_v, nullptr);
            json_node_unref(old_v);
            json_object_remove_member(store, k.c_str());
        }
    } else if (op == "storage.clear") {
        for (GList *l = json_object_get_members(store); l; l = g_list_next(l)) {
            const char *k = static_cast<char *>(l->data);
            note(k, json_object_get_member(store, k), nullptr);
        }
        std::vector<std::string> all_keys;
        for (GList *l = json_object_get_members(store); l; l = l->next) all_keys.push_back(static_cast<char *>(l->data));
        for (auto &k : all_keys) json_object_remove_member(store, k.c_str());
    }
    changes += "}";
    persist(e, area);
    respond_ok(r, "null");
    if (changes != "{}") fire(e, "storage.onChanged", "[" + changes + "," + quote(area) + "]", true);
}

// --------------------------------------------------------------------- dispatch

static bool matches_filter(ExtensionManager::TabInfo &t, JsonObject *q) {
    if (member(q, "active") && bool_of(q, "active") != t.active) return false;
    if (member(q, "title")) { auto ts = strings_of(q, "title"); if (!ts.empty() && !g_pattern_match_simple(ts[0].c_str(), t.title.c_str())) return false; }
    if (JsonNode *u = member(q, "url")) {
        std::vector<std::string> pats = strings_of(q, "url");
        (void)u;
        bool any = pats.empty();
        for (auto &p : pats) {
            GError *err = nullptr;
            WebKitWebExtensionMatchPattern *mp = webkit_web_extension_match_pattern_new_with_string(p.c_str(), &err);
            if (err) { g_error_free(err); continue; }
            if (mp) { if (webkit_web_extension_match_pattern_matches_url(mp, t.url.c_str(), WEBKIT_WEB_EXTENSION_MATCH_PATTERN_OPTIONS_NONE)) any = true; webkit_web_extension_match_pattern_unref(mp); }
        }
        if (!any) return false;
    }
    return true;
}

void ExtensionManager::Impl::dispatch(MsgCtx *c, const std::string &op, JsonObject *args, const std::string &sender_url, bool top, ReplyHandle *r) {
    Ext &e = *c->ext;
    bool is_bg = c->tab_id < 0;
    if (op == "log") { g_message("[extension %s] %s", e.name.c_str(), str_of(args, "msg").c_str()); respond_ok(r, "null"); return; }
    if (op.rfind("storage.", 0) == 0) { storage_op(e, op, args, r); return; }

    if (op == "runtime.sendMessage") {
        if (is_bg && !c->page) {
            // background -> popup/options pages: the first page that answers wins
            std::string payload = "[" + dump(member(args, "message")) + "," + sender_json(c, sender_url, top) + "]";
            auto views = std::make_shared<std::vector<WebKitWebView *>>(e.page_views);
            auto step = std::make_shared<std::function<void(size_t)>>();
            *step = [this, views, step, payload, r](size_t i) {
                if (i >= views->size()) { respond_ok(r, "{\"noReceiver\":true}"); return; }
                run_js((*views)[i], "", "const a = JSON.parse(p); return await window.__reedDeliver(a[0], a[1]);", payload,
                       [this, step, r, i](bool ok, const std::string &j) {
                           if (ok && j.find("\"responded\":true") != std::string::npos) { respond_ok(r, j); return; }
                           (*step)(i + 1);
                       });
            };
            (*step)(0);
            return;
        }
        if (!e.bg) { respond_ok(r, "{\"noReceiver\":true}"); return; }
        if (!e.bg_ready) {
            std::string args_copy = "{}";
            if (args) { Json tmp(json_node_alloc()); json_node_init_object(tmp.n, args); args_copy = dump(tmp.n); }
            e.waiting.push_back([this, c, op, args_copy, sender_url, top, r]() {
                Json a = parse(args_copy);
                dispatch(c, op, a.obj(), sender_url, top, r);
            });
            return;
        }
        std::string payload = "[" + dump(member(args, "message")) + "," + sender_json(c, sender_url, top) + "]";
        run_js(e.bg, "", "const a = JSON.parse(p); return await window.__reedDeliver(a[0], a[1]);", payload,
               [this, r](bool ok, const std::string &j) { if (ok) respond_ok(r, j); else respond_ok(r, "{\"noReceiver\":true}"); });
        return;
    }
    if (op == "action.set") {
        e.action[str_of(args, "name")] = dump(member(args, "details"));
        respond_ok(r, "null");
        notify_actions();
        return;
    }
    if (op == "action.get") {
        std::string n = str_of(args, "name");
        std::string key = n == "getTitle" ? "setTitle" : n == "getBadgeText" ? "setBadgeText" : n == "getPopup" ? "setPopup" : "setBadgeBackgroundColor";
        std::string v = e.action.count(key) ? e.action[key] : "null";
        respond_ok(r, v == "null" ? "\"\"" : v);
        return;
    }
    if (!is_bg) {
        // content scripts may use ports and nothing else privileged
        if (op.rfind("port.", 0) == 0 && dispatch_extra(c, op, args, sender_url, top, r)) return;
        respond_err(r, op + " is only available to the background page"); return;
    }
    if (dispatch_extra(c, op, args, sender_url, top, r)) return;

    if (op == "tabs.query") {
        JsonObject *q = json_node_get_object(member(args, "q"));
        std::string out = "[";
        bool first = true;
        if (host.list_tabs) for (auto &t : host.list_tabs()) {
            if (q && !matches_filter(t, q)) continue;
            if (!first) out += ",";
            first = false;
            out += tab_json(t);
        }
        respond_ok(r, out + "]");
        return;
    }
    if (op == "tabs.get") {
        int id = static_cast<int>(json_node_get_int(member(args, "id")));
        if (host.list_tabs) for (auto &t : host.list_tabs()) if (t.id == id) { respond_ok(r, tab_json(t)); return; }
        respond_err(r, "Invalid tab ID: " + std::to_string(id));
        return;
    }
    if (op == "tabs.create") {
        JsonObject *p = json_node_get_object(member(args, "p"));
        std::string url = str_of(p, "url");
        bool active = p && member(p, "active") ? bool_of(p, "active", true) : true;
        int id = host.open_tab ? host.open_tab(url, active) : -1;
        if (host.list_tabs) for (auto &t : host.list_tabs()) if (t.id == id) { respond_ok(r, tab_json(t)); return; }
        respond_ok(r, "{\"id\":" + std::to_string(id) + "}");
        return;
    }
    if (op == "tabs.update" || op == "tabs.reload" || op == "tabs.remove") {
        if (op == "tabs.remove") {
            JsonArray *ids = json_node_get_array(member(args, "ids"));
            if (ids && host.close_tab) for (guint i = 0; i < json_array_get_length(ids); ++i) host.close_tab(static_cast<int>(json_node_get_int(json_array_get_element(ids, i))));
            respond_ok(r, "null");
            return;
        }
        int id = -1;
        if (JsonNode *n = member(args, "id"); n && JSON_NODE_HOLDS_VALUE(n)) id = static_cast<int>(json_node_get_int(n));
        if (id < 0 && host.list_tabs) for (auto &t : host.list_tabs()) if (t.active) id = t.id;
        auto it = tab_views.find(id);
        if (it == tab_views.end()) { respond_err(r, "Invalid tab ID: " + std::to_string(id)); return; }
        if (op == "tabs.reload") webkit_web_view_reload(it->second);
        else {
            JsonObject *p = json_node_get_object(member(args, "p"));
            std::string url = str_of(p, "url");
            if (!url.empty()) webkit_web_view_load_uri(it->second, url.c_str());
            if (p && bool_of(p, "active") && host.activate_tab) host.activate_tab(id);
        }
        if (host.list_tabs) for (auto &t : host.list_tabs()) if (t.id == id) { respond_ok(r, tab_json(t)); return; }
        respond_ok(r, "null");
        return;
    }
    if (op == "tabs.sendMessage") {
        int id = static_cast<int>(json_node_get_int(member(args, "id")));
        auto it = tab_views.find(id);
        if (it == tab_views.end()) { respond_err(r, "Invalid tab ID: " + std::to_string(id)); return; }
        std::string payload = "[" + dump(member(args, "message")) + ",{\"id\":" + quote(e.id) + "}]";
        run_js(it->second, "reed-ext-" + e.uuid, "const a = JSON.parse(p); return await window.__reedDeliver(a[0], a[1]);", payload,
               [this, r](bool ok, const std::string &j) { if (ok) respond_ok(r, j); else respond_ok(r, "{\"noReceiver\":true}"); });
        return;
    }
    respond_err(r, "unsupported operation " + op);
}

void ExtensionManager::Impl::on_message(MsgCtx *c, JSCValue *value, WebKitScriptMessageReply *reply) {
    auto *r = new ReplyHandle{webkit_script_message_reply_ref(reply), static_cast<JSCContext *>(g_object_ref(jsc_value_get_context(value)))};
    char *js = jsc_value_to_json(value, 0);
    Json msg = parse(js ? js : "");
    g_free(js);
    JsonObject *o = msg.obj();
    if (!o) { respond_err(r, "malformed message"); return; }
    JsonObject *args = member(o, "args") && JSON_NODE_HOLDS_OBJECT(member(o, "args")) ? json_node_get_object(member(o, "args")) : nullptr;
    dispatch(c, str_of(o, "op"), args, str_of(o, "url"), bool_of(o, "top", true), r);
}

// ---------------------------------------------------------------- background

void ExtensionManager::Impl::start_background(Ext &e) {
    if (e.bg_started || (e.bg_page.empty() && e.bg_scripts.empty())) return;
    e.bg_started = true;
    e.bg_ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler_with_reply(e.bg_ucm, e.handler.c_str(), nullptr);
    auto *ctx = new MsgCtx{this, &e, -1, nullptr};
    g_signal_connect_data(e.bg_ucm, ("script-message-with-reply-received::" + e.handler).c_str(),
        G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *v, WebKitScriptMessageReply *reply, gpointer ud) -> gboolean {
            auto *c = static_cast<MsgCtx *>(ud);
            c->mgr->on_message(c, v, reply);
            return TRUE;
        }), ctx, +[](gpointer d, GClosure *) { delete static_cast<MsgCtx *>(d); }, GConnectFlags(0));

    std::string src = polyfill(e, "background");
    WebKitUserScript *us = webkit_user_script_new(src.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(e.bg_ucm, us);
    webkit_user_script_unref(us);

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    e.bg = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, "settings", settings, "user-content-manager", e.bg_ucm, nullptr));
    g_object_unref(settings);
    g_object_ref_sink(e.bg);
    const char *allow[] = {"http://*/*", "https://*/*", nullptr};
    webkit_web_view_set_cors_allowlist(e.bg, allow);
    BgCtx *bgctx = new BgCtx{this, &e};
    g_signal_connect(e.bg, "load-changed", G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent ev, gpointer ud) {
        if (ev != WEBKIT_LOAD_FINISHED) return;
        auto *self = static_cast<BgCtx *>(ud);
        Ext &ex = *self->ext;
        ex.bg_ready = true;
        if (!ex.pending_event.empty()) {
            self->mgr->fire(ex, "runtime.onInstalled", "[{\"reason\":" + quote(ex.pending_event) + "}]", false);
            ex.pending_event.clear();
            self->mgr->save_registry();
        }
        self->mgr->fire(ex, "runtime.onStartup", "[]", false);
        auto pending = std::move(ex.waiting);
        ex.waiting.clear();
        for (auto &fn : pending) fn();
    }), bgctx);
    g_signal_connect(e.bg, "load-failed", G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, const char *uri, GError *err, gpointer) -> gboolean {
        g_warning("extension background page failed to load (%s): %s", uri, err ? err->message : "?");
        return FALSE;
    }), nullptr);
    std::string page = e.bg_page.empty() ? "_generated_background_page.html" : e.bg_page;
    std::string url = "moz-extension://" + e.uuid + "/" + page;
    webkit_web_view_load_uri(e.bg, url.c_str());
}

// ===================================================================== extension pages, ports, scripting, DNR

static const char *kNoReceiver = "Could not establish connection. Receiving end does not exist.";

static std::string sha_key(const std::string &s) {
    gchar *h = g_compute_checksum_for_string(G_CHECKSUM_SHA1, s.c_str(), -1);
    std::string r = h; g_free(h); return r.substr(0, 16);
}

static std::string read_ext_file(const Ext &e, const std::string &rel) {
    fs::path r = fs::path(rel).relative_path().lexically_normal();
    for (auto &part : r) if (part == "..") return "";
    return read_file(e.dir + "/" + r.string());
}

WebKitWebView *ExtensionManager::Impl::make_page_view(Ext &e, const char *ctx) {
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler_with_reply(ucm, e.handler.c_str(), nullptr);
    auto *mc = new MsgCtx{this, &e, -1, nullptr, true};
    g_signal_connect_data(ucm, ("script-message-with-reply-received::" + e.handler).c_str(),
        G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *v, WebKitScriptMessageReply *reply, gpointer ud) -> gboolean {
            auto *c = static_cast<MsgCtx *>(ud);
            c->mgr->on_message(c, v, reply);
            return TRUE;
        }), mc, +[](gpointer d, GClosure *) { delete static_cast<MsgCtx *>(d); }, GConnectFlags(0));
    std::string src = polyfill(e, ctx);
    WebKitUserScript *us = webkit_user_script_new(src.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, us);
    webkit_user_script_unref(us);
    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    WebKitWebView *view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, "settings", settings, "user-content-manager", ucm, nullptr));
    g_object_unref(settings);
    g_object_unref(ucm);
    mc->view = view;
    e.page_views.push_back(view);
    {
        struct Gone { Ext *e; };
        g_object_weak_ref(G_OBJECT(view), +[](gpointer d, GObject *obj) {
            auto *g = static_cast<Gone *>(d);
            auto &v = g->e->page_views;
            v.erase(std::remove(v.begin(), v.end(), reinterpret_cast<WebKitWebView *>(obj)), v.end());
            delete g;
        }, new Gone{&e});
    }
    const char *allow[] = {"http://*/*", "https://*/*", nullptr};
    webkit_web_view_set_cors_allowlist(view, allow);

    // Extension pages stay on moz-extension://<uuid>/; any other link opens in a normal tab.
    struct Nav { Impl *mgr; std::string prefix; };
    auto *nav = new Nav{this, "moz-extension://" + e.uuid + "/"};
    g_signal_connect_data(view, "decide-policy", G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *d, WebKitPolicyDecisionType t, gpointer ud) -> gboolean {
        auto *n = static_cast<Nav *>(ud);
        if (t != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && t != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;
        auto *na = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
        const char *u = webkit_uri_request_get_uri(webkit_navigation_action_get_request(na));
        std::string uri = u ? u : "";
        bool local = uri.rfind(n->prefix, 0) == 0 || uri == "about:blank" || uri.rfind("blob:", 0) == 0 || uri.rfind("data:", 0) == 0;
        if (local && t == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
        webkit_policy_decision_ignore(d);
        if (!local && n->mgr->host.open_tab && (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0)) n->mgr->host.open_tab(uri, true);
        return TRUE;
    }), nav, +[](gpointer p, GClosure *) { delete static_cast<Nav *>(p); }, GConnectFlags(0));
    g_signal_connect(view, "create", G_CALLBACK(+[](WebKitWebView *, WebKitNavigationAction *na, gpointer ud) -> GtkWidget * {
        auto *self = static_cast<Impl *>(ud);
        const char *u = webkit_uri_request_get_uri(webkit_navigation_action_get_request(na));
        std::string uri = u ? u : "";
        if (self->host.open_tab && (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0)) self->host.open_tab(uri, true);
        return nullptr;
    }), this);
    g_object_weak_ref(G_OBJECT(view), [](gpointer d, GObject *obj) { static_cast<Impl *>(d)->drop_ports_for(reinterpret_cast<WebKitWebView *>(obj)); }, this);
    return view;
}

PortEnd ExtensionManager::Impl::end_of(MsgCtx *c) {
    return PortEnd{c->view ? c->view : c->ext->bg, c->tab_id >= 0 ? "reed-ext-" + c->ext->uuid : std::string()};
}

void ExtensionManager::Impl::deliver_to(Ext &e, const PortEnd &to, const std::string &body, const std::string &param) {
    if (!to.view) return;
    WebKitWebView *view = to.view; std::string world = to.world;
    auto go = [this, view, world, body, param]() { run_js(view, world, body, param, nullptr); };
    if (view == e.bg && !e.bg_ready) e.waiting.push_back(go); else go();
}

void ExtensionManager::Impl::drop_ports_for(WebKitWebView *view) {
    std::vector<std::string> dead;
    for (auto &[id, rec] : ports) if (rec.a.view == view || rec.b.view == view) dead.push_back(id);
    for (auto &id : dead) {
        PortRec rec = ports[id];
        ports.erase(id);
        const PortEnd &other = rec.a.view == view ? rec.b : rec.a;
        if (other.view && other.view != view) run_js(other.view, other.world, "window.__reedPortDisc(JSON.parse(p)); return null;", quote(id), nullptr);
    }
}

WebKitWebView *ExtensionManager::Impl::tab_view_for(JsonNode *n) {
    int id = -1;
    if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) != G_TYPE_STRING) id = static_cast<int>(json_node_get_int(n));
    if (id < 0 && host.list_tabs) for (auto &t : host.list_tabs()) if (t.active) id = t.id;
    auto it = tab_views.find(id);
    return it == tab_views.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------- registered content scripts
void ExtensionManager::Impl::add_registered_to(Ext &e, Ext::Reg &g, WebKitUserContentManager *ucm) {
    std::vector<std::string> allow, block;
    for (auto &m : g.matches) convert_pattern(m, allow);
    for (auto &m : g.excludes) convert_pattern(m, block);
    if (allow.empty()) return;
    auto av = to_cstrv(allow), bv = to_cstrv(block);
    std::string world = "reed-ext-" + e.uuid;
    bool main_world = g.world == "MAIN";
    auto frames = g.all_frames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
    if (!g.css.empty()) {
        WebKitUserStyleSheet *ss = main_world
            ? webkit_user_style_sheet_new(g.css.c_str(), frames, WEBKIT_USER_STYLE_LEVEL_USER, av.data(), block.empty() ? nullptr : bv.data())
            : webkit_user_style_sheet_new_for_world(g.css.c_str(), frames, WEBKIT_USER_STYLE_LEVEL_USER, world.c_str(), av.data(), block.empty() ? nullptr : bv.data());
        webkit_user_content_manager_add_style_sheet(ucm, ss);
        g.sheets.push_back({ucm, ss});  // our reference is kept so the sheet can be removed later
    }
    if (!g.js.empty()) {
        auto when = g.run_at == "document_start" ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;
        WebKitUserScript *us = main_world
            ? webkit_user_script_new(g.js.c_str(), frames, when, av.data(), block.empty() ? nullptr : bv.data())
            : webkit_user_script_new_for_world(g.js.c_str(), frames, when, world.c_str(), av.data(), block.empty() ? nullptr : bv.data());
        webkit_user_content_manager_add_script(ucm, us);
        g.scripts.push_back({ucm, us});
    }
}
void ExtensionManager::Impl::drop_registered(Ext &, Ext::Reg &g) {
    for (auto &[ucm, us] : g.scripts) { if (views_has_ucm(ucm)) webkit_user_content_manager_remove_script(ucm, us); webkit_user_script_unref(us); }
    for (auto &[ucm, ss] : g.sheets) { if (views_has_ucm(ucm)) webkit_user_content_manager_remove_style_sheet(ucm, ss); webkit_user_style_sheet_unref(ss); }
    g.scripts.clear(); g.sheets.clear();
}
void ExtensionManager::Impl::save_registered(Ext &e) {
    std::string out = "[";
    bool first = true;
    for (auto &g : e.registered) { if (!g.persist) continue; if (!first) out += ","; first = false; out += g.raw; }
    write_file(store_dir + "/" + e.uuid + "/registered.json", out + "]");
}
bool ExtensionManager::Impl::register_script(Ext &e, JsonObject *o, std::string *err) {
    Ext::Reg g;
    JsonNode *tmp = json_node_alloc();
    json_node_init_object(tmp, o);
    g.raw = dump(tmp);
    json_node_unref(tmp);  // does not free `o` (init_object takes its own reference)
    g.id = str_of(o, "id");
    if (g.id.empty()) { g.id = "reed-" + sha_key(g.raw + std::to_string(g_get_monotonic_time())); }
    for (auto &x : e.registered) if (x.id == g.id) { *err = "Duplicate script ID '" + g.id + "'"; return false; }
    g.matches = strings_of(o, "matches");
    g.excludes = strings_of(o, "excludeMatches");
    g.run_at = str_of(o, "runAt", "document_idle");
    g.world = str_of(o, "world", "ISOLATED");
    g.all_frames = bool_of(o, "allFrames", false);
    g.persist = bool_of(o, "persistAcrossSessions", true);
    for (auto &f : strings_of(o, "js")) g.js += "\n// ---- " + f + "\n" + read_ext_file(e, f) + "\n;\n";
    for (auto &f : strings_of(o, "css")) g.css += read_ext_file(e, f) + "\n";
    for (auto &c : strings_of(o, "jsCode")) g.js += "\n" + c + "\n;\n";
    for (auto &c : strings_of(o, "cssCode")) g.css += c + "\n";
    if (g.matches.empty()) { *err = "Script must specify matches"; return false; }
    e.registered.push_back(std::move(g));
    Ext::Reg &stored = e.registered.back();
    for (auto &[view, info] : views) {
        (void)view;
        if (std::find(info.attached.begin(), info.attached.end(), &e) != info.attached.end()) add_registered_to(e, stored, info.ucm);
    }
    return true;
}

// ---------------------------------------------------------------- declarativeNetRequest
void ExtensionManager::Impl::load_dnr_state(Ext &e) {
    std::string dir = store_dir + "/" + e.uuid;
    Json en = parse(read_file(dir + "/dnr-enabled.json"));
    if (JsonArray *a = en.arr()) {
        std::set<std::string> on;
        for (guint i = 0; i < json_array_get_length(a); ++i) on.insert(json_node_get_string(json_array_get_element(a, i)));
        for (auto &rs : e.rulesets) rs.enabled = on.count(rs.id) > 0;
    }
    std::string dyn = read_file(dir + "/dnr-dynamic.json");
    if (!dyn.empty() && parse(dyn).arr()) e.dynamic_rules = dyn;
    Json reg = parse(read_file(dir + "/registered.json"));
    if (JsonArray *a = reg.arr()) {
        for (guint i = 0; i < json_array_get_length(a); ++i) {
            JsonObject *o = json_node_get_object(json_array_get_element(a, i));
            std::string err;
            if (o) register_script(e, o, &err);
        }
    }
    if (!e.rulesets.empty() || e.dynamic_rules != "[]") rebuild_dnr(e);
}

void ExtensionManager::Impl::rebuild_dnr(Ext &e) {
    std::vector<dnr::Rule> rules;
    for (auto &rs : e.rulesets) {
        if (!rs.enabled) continue;
        Json j = parse(read_ext_file(e, rs.path));
        auto part = dnr::parse_rules(j.n);
        for (auto &r : part) rules.push_back(std::move(r));
    }
    for (const std::string *list : {&e.dynamic_rules, &e.session_rules}) {
        Json j = parse(*list);
        auto part = dnr::parse_rules(j.n);
        for (auto &r : part) rules.push_back(std::move(r));
    }
    dnr::Stats st;
    std::string json = dnr::to_webkit(rules, &st);
    if (st.input) g_message("[extension %s] declarativeNetRequest: %d rules -> %d content rules (skipped: %d redirect/modifyHeaders, %d regex, %d other)",
                            e.name.c_str(), st.input, st.emitted, st.skipped_unsupported_action, st.skipped_regex, st.skipped_other);
    unsigned gen = ++e.dnr_generation;
    if (st.emitted == 0) {
        if (e.dnr_filter) {
            for (auto &[v, info] : views) { (void)v; webkit_user_content_manager_remove_filter(info.ucm, e.dnr_filter); }
            webkit_user_content_filter_unref(e.dnr_filter);
            e.dnr_filter = nullptr;
        }
        return;
    }
    if (!filter_store) filter_store = webkit_user_content_filter_store_new((data_dir + "/ext-filters").c_str());
    struct Job { Impl *mgr; Ext *ext; unsigned gen; };
    GBytes *bytes = g_bytes_new(json.data(), json.size());
    webkit_user_content_filter_store_save(filter_store, ("dnr-" + e.uuid).c_str(), bytes, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer ud) {
            std::unique_ptr<Job> j(static_cast<Job *>(ud));
            GError *err = nullptr;
            WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(src), res, &err);
            Ext &ex = *j->ext;
            if (err) { g_warning("[extension %s] content blocking rules rejected by WebKit: %s", ex.name.c_str(), err->message); g_error_free(err); return; }
            if (j->gen != ex.dnr_generation) { webkit_user_content_filter_unref(f); return; }  // a newer update superseded this one
            if (ex.dnr_filter) {
                for (auto &[v, info] : j->mgr->views) { (void)v; webkit_user_content_manager_remove_filter(info.ucm, ex.dnr_filter); }
                webkit_user_content_filter_unref(ex.dnr_filter);
            }
            ex.dnr_filter = f;
            for (auto &[v, info] : j->mgr->views) { (void)v; webkit_user_content_manager_add_filter(info.ucm, f); }
            g_message("[extension %s] content blocking rules active", ex.name.c_str());
        }, new Job{this, &e, gen});
    g_bytes_unref(bytes);
}


// ---------------------------------------------------------------- cookies
static std::string cookie_json(SoupCookie *c) {
    const char *dom = soup_cookie_get_domain(c);
    std::string d = dom ? dom : "";
    GDateTime *exp = soup_cookie_get_expires(c);
    const char *ss = "no_restriction";
    switch (soup_cookie_get_same_site_policy(c)) { case SOUP_SAME_SITE_POLICY_LAX: ss = "lax"; break; case SOUP_SAME_SITE_POLICY_STRICT: ss = "strict"; break; default: break; }
    std::string o = "{\"name\":" + quote(soup_cookie_get_name(c)) + ",\"value\":" + quote(soup_cookie_get_value(c)) + ",\"domain\":" + quote(d) +
        ",\"hostOnly\":" + (!d.empty() && d[0] == '.' ? "false" : "true") + ",\"path\":" + quote(soup_cookie_get_path(c)) +
        ",\"secure\":" + (soup_cookie_get_secure(c) ? "true" : "false") + ",\"httpOnly\":" + (soup_cookie_get_http_only(c) ? "true" : "false") +
        ",\"sameSite\":\"" + ss + "\",\"session\":" + (exp ? "false" : "true") + ",\"storeId\":\"0\",\"firstPartyDomain\":\"\"";
    if (exp) o += ",\"expirationDate\":" + std::to_string(static_cast<double>(g_date_time_to_unix(exp)));
    return o + "}";
}
static bool cookie_domain_matches(const std::string &cookie_dom, const std::string &want) {
    auto strip = [](std::string x) { if (!x.empty() && x[0] == '.') x.erase(0, 1); return x; };
    std::string c = strip(cookie_dom), w = strip(want);
    return c == w || (c.size() > w.size() && c.compare(c.size() - w.size() - 1, w.size() + 1, "." + w) == 0);
}
static bool cookie_matches(SoupCookie *c, JsonObject *f) {
    if (!f) return true;
    std::string name = str_of(f, "name"), path = str_of(f, "path"), domain = str_of(f, "domain");
    if (!name.empty() && name != soup_cookie_get_name(c)) return false;
    if (!path.empty() && path != soup_cookie_get_path(c)) return false;
    if (!domain.empty() && !cookie_domain_matches(soup_cookie_get_domain(c), domain)) return false;
    if (member(f, "secure") && bool_of(f, "secure") != (soup_cookie_get_secure(c) != FALSE)) return false;
    if (member(f, "session") && bool_of(f, "session") != (soup_cookie_get_expires(c) == nullptr)) return false;
    return true;
}
static WebKitCookieManager *cookie_manager_for(WebKitNetworkSession *s) { return webkit_network_session_get_cookie_manager(s); }

bool ExtensionManager::Impl::dispatch_cookies(MsgCtx *c, const std::string &op, JsonObject *args, ReplyHandle *r) {
    Ext &e = *c->ext;
    if (!e.has_permission("cookies")) { respond_err(r, "The \"cookies\" permission is required."); return true; }
    WebKitCookieManager *cm = cookie_manager_for(session);
    JsonNode *dn = member(args, "d");
    JsonObject *d = dn && JSON_NODE_HOLDS_OBJECT(dn) ? json_node_get_object(dn) : nullptr;
    struct Job { Impl *mgr; ReplyHandle *r; Json filter; std::string op, url; };
    if (op == "cookies.getAllCookieStores") { respond_ok(r, "[{\"id\":\"0\",\"incognito\":false,\"tabIds\":[]}]"); return true; }
    std::string url = str_of(d, "url");
    if (op == "cookies.set") {
        std::string name = str_of(d, "name"), value = str_of(d, "value"), domain = str_of(d, "domain"), path = str_of(d, "path", "/");
        if (url.empty() && domain.empty()) { respond_err(r, "cookies.set requires a url"); return true; }
        if (domain.empty()) { GUri *u = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr); if (u) { domain = g_uri_get_host(u) ? g_uri_get_host(u) : ""; g_uri_unref(u); } }
        SoupCookie *ck = soup_cookie_new(name.c_str(), value.c_str(), domain.c_str(), path.c_str(), -1);
        if (bool_of(d, "secure")) soup_cookie_set_secure(ck, TRUE);
        if (bool_of(d, "httpOnly")) soup_cookie_set_http_only(ck, TRUE);
        std::string ss = str_of(d, "sameSite");
        if (ss == "strict") soup_cookie_set_same_site_policy(ck, SOUP_SAME_SITE_POLICY_STRICT);
        else if (ss == "lax") soup_cookie_set_same_site_policy(ck, SOUP_SAME_SITE_POLICY_LAX);
        else if (ss == "no_restriction" && bool_of(d, "secure")) soup_cookie_set_same_site_policy(ck, SOUP_SAME_SITE_POLICY_NONE);  // SameSite=None needs Secure
        if (JsonNode *ex = member(d, "expirationDate"); ex && JSON_NODE_HOLDS_VALUE(ex)) {
            GDateTime *dt = g_date_time_new_from_unix_utc(static_cast<gint64>(json_node_get_double(ex)));
            if (dt) { soup_cookie_set_expires(ck, dt); g_date_time_unref(dt); }
        }
        struct SetJob { Impl *mgr; ReplyHandle *r; std::string out; SoupCookie *ck; };
        auto *sj = new SetJob{this, r, cookie_json(ck), ck};  // the cookie must outlive the async call
        webkit_cookie_manager_add_cookie(cm, ck, nullptr, [](GObject *src, GAsyncResult *res, gpointer ud) {
            std::unique_ptr<SetJob> j(static_cast<SetJob *>(ud));
            GError *err = nullptr;
            gboolean ok = webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(src), res, &err);
            if (err || !ok) { j->mgr->respond_err(j->r, err ? err->message : "could not set cookie"); if (err) g_error_free(err); }
            else j->mgr->respond_ok(j->r, j->out);
            soup_cookie_free(j->ck);
        }, sj);
        return true;
    }
    // get / getAll / remove: fetch candidate cookies, then filter
    auto *job = new Job{this, r, Json(), op, url};
    if (d) { JsonNode *cp = json_node_init_object(json_node_alloc(), d); job->filter = Json(cp); }
    auto finish = +[](GObject *src, GAsyncResult *res, gpointer ud) {
        std::unique_ptr<Job> j(static_cast<Job *>(ud));
        GError *err = nullptr;
        GList *list = j->url.empty() ? webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(src), res, &err)
                                     : webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(src), res, &err);
        if (err) { j->mgr->respond_err(j->r, err->message); g_error_free(err); return; }
        JsonObject *f = j->filter.obj();
        std::vector<SoupCookie *> hits;
        for (GList *l = list; l; l = l->next) if (cookie_matches(static_cast<SoupCookie *>(l->data), f)) hits.push_back(static_cast<SoupCookie *>(l->data));
        if (j->op == "cookies.getAll") {
            std::string out = "[";
            for (size_t i = 0; i < hits.size(); ++i) { if (i) out += ","; out += cookie_json(hits[i]); }
            j->mgr->respond_ok(j->r, out + "]");
        } else if (j->op == "cookies.get") {
            // most specific path first
            std::sort(hits.begin(), hits.end(), [](SoupCookie *a, SoupCookie *b) { return strlen(soup_cookie_get_path(a)) > strlen(soup_cookie_get_path(b)); });
            j->mgr->respond_ok(j->r, hits.empty() ? "null" : cookie_json(hits[0]));
        } else {  // remove
            if (hits.empty()) { j->mgr->respond_ok(j->r, "null"); }
            else {
                std::string reply = "{\"url\":" + quote(j->url) + ",\"name\":" + quote(soup_cookie_get_name(hits[0])) + ",\"storeId\":\"0\"}";
                webkit_cookie_manager_delete_cookie(WEBKIT_COOKIE_MANAGER(src), hits[0], nullptr, [](GObject *s2, GAsyncResult *r2, gpointer u2) {
                    auto *p = static_cast<std::pair<Impl *, std::pair<ReplyHandle *, std::string>> *>(u2);
                    webkit_cookie_manager_delete_cookie_finish(WEBKIT_COOKIE_MANAGER(s2), r2, nullptr);
                    p->first->respond_ok(p->second.first, p->second.second);
                    delete p;
                }, new std::pair<Impl *, std::pair<ReplyHandle *, std::string>>(j->mgr, {j->r, reply}));
            }
        }
        g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
    };
    if (url.empty()) webkit_cookie_manager_get_all_cookies(cm, nullptr, finish, job);
    else webkit_cookie_manager_get_cookies(cm, url.c_str(), nullptr, finish, job);
    return true;
}

// ---------------------------------------------------------------- webRequest (observe-only)
static const char *guess_type(const std::string &url, bool main) {
    if (main) return "main_frame";
    std::string u = url.substr(0, url.find_first_of("?#"));
    auto ends = [&](const char *x) { size_t n = strlen(x); return u.size() >= n && g_ascii_strcasecmp(u.c_str() + u.size() - n, x) == 0; };
    if (ends(".js") || ends(".mjs")) return "script";
    if (ends(".css")) return "stylesheet";
    if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".gif") || ends(".webp") || ends(".svg") || ends(".ico") || ends(".avif")) return "image";
    if (ends(".woff") || ends(".woff2") || ends(".ttf") || ends(".otf")) return "font";
    if (ends(".mp4") || ends(".webm") || ends(".mp3") || ends(".ogg")) return "media";
    if (ends(".html") || ends(".htm")) return "sub_frame";
    return "xmlhttprequest";
}

static void webreq_emit(ExtensionManager::Impl *d, const char *event, WebKitWebView *view, const std::string &id, const std::string &url,
                        const std::string &method, const std::string &type, int status) {
    int tab = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "reed-tab-id"));
    const char *doc = webkit_web_view_get_uri(view);
    std::string det = "{\"requestId\":" + quote(id) + ",\"url\":" + quote(url) + ",\"method\":" + quote(method) + ",\"type\":" + quote(type) +
                      ",\"tabId\":" + std::to_string(tab) + ",\"frameId\":0,\"parentFrameId\":-1,\"timeStamp\":" + std::to_string(g_get_real_time() / 1000.0) +
                      ",\"originUrl\":" + quote(doc ? doc : "") + ",\"documentUrl\":" + quote(doc ? doc : "");
    if (status) det += ",\"statusCode\":" + std::to_string(status);
    det += "}";
    for (auto &e : d->exts) if (e->enabled && e->bg && e->bg_ready && e->webreq_listening && e->has_permission("webRequest")) d->fire(*e, event, "[" + det + "]", false);
}

static void on_resource_started(WebKitWebView *view, WebKitWebResource *res, WebKitURIRequest *req, gpointer ud) {
    auto *d = static_cast<ExtensionManager::Impl *>(ud);
    static unsigned counter = 0;
    std::string id = std::to_string(++counter);
    const char *u = webkit_uri_request_get_uri(req);
    std::string url = u ? u : "";
    if (url.rfind("http", 0) != 0) return;
    const char *m = webkit_uri_request_get_http_method(req);
    std::string method = m ? m : "GET";
    bool main = res == webkit_web_view_get_main_resource(view);
    std::string type = guess_type(url, main);
    webreq_emit(d, "webRequest.onBeforeRequest", view, id, url, method, type, 0);
    struct Ctx { ExtensionManager::Impl *d; WebKitWebView *view; std::string id, url, method, type; };
    auto *c = new Ctx{d, view, id, url, method, type};
    g_object_ref(view);
    g_signal_connect_data(res, "finished", G_CALLBACK(+[](WebKitWebResource *r, gpointer p) {
        auto *x = static_cast<Ctx *>(p);
        WebKitURIResponse *resp = webkit_web_resource_get_response(r);
        webreq_emit(x->d, "webRequest.onCompleted", x->view, x->id, x->url, x->method, x->type, resp ? static_cast<int>(webkit_uri_response_get_status_code(resp)) : 0);
    }), c, +[](gpointer p, GClosure *) { auto *x = static_cast<Ctx *>(p); g_object_unref(x->view); delete x; }, GConnectFlags(0));
    g_signal_connect(res, "failed", G_CALLBACK(+[](WebKitWebResource *, GError *, gpointer p) {
        auto *x = static_cast<Ctx *>(p);
        webreq_emit(x->d, "webRequest.onErrorOccurred", x->view, x->id, x->url, x->method, x->type, 0);
    }), c);
}

// ---------------------------------------------------------------- the extra API operations
static JsonArray *arr_of(JsonObject *o, const char *k) {
    JsonNode *n = member(o, k);
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : nullptr;
}
static std::string enabled_ids_json(const Ext &e) {
    std::string out = "[";
    bool first = true;
    for (auto &rs : e.rulesets) if (rs.enabled) { if (!first) out += ","; first = false; out += quote(rs.id); }
    return out + "]";
}

bool ExtensionManager::Impl::dispatch_extra(MsgCtx *c, const std::string &op, JsonObject *args, const std::string &sender_url, bool top, ReplyHandle *r) {
    Ext &e = *c->ext;
    const std::string world = "reed-ext-" + e.uuid;

    // ---- ports (runtime.connect / tabs.connect)
    if (op.rfind("menus.", 0) == 0) {
        auto apply = [](Ext::MenuRec &m, JsonObject *p) {
            if (member(p, "title")) m.title = str_of(p, "title");
            if (member(p, "type")) m.type = str_of(p, "type", "normal");
            if (member(p, "parentId")) m.parent = str_of(p, "parentId");
            if (member(p, "enabled")) m.enabled = bool_of(p, "enabled", true);
            if (member(p, "visible")) m.visible = bool_of(p, "visible", true);
            if (member(p, "checked")) m.checked = bool_of(p, "checked", false);
            if (JsonArray *a = arr_of(p, "contexts")) {
                m.contexts.clear();
                for (guint i = 0; i < json_array_get_length(a); i++) m.contexts.push_back(json_array_get_string_element(a, i));
            }
        };
        if (op == "menus.create") {
            JsonObject *p = json_node_get_object(member(args, "props"));
            std::string id = str_of(p, "id");
            for (auto &m : e.menus) if (m.id == id) { respond_err(r, "ID already exists: " + id); return true; }
            Ext::MenuRec m; m.id = id; apply(m, p);
            e.menus.push_back(m);
        } else if (op == "menus.update") {
            std::string id = str_of(args, "id");
            JsonObject *p = json_node_get_object(member(args, "props"));
            bool found = false;
            for (auto &m : e.menus) if (m.id == id) { apply(m, p); found = true; }
            if (!found) { respond_err(r, "Cannot find menu item with id " + id); return true; }
        } else if (op == "menus.remove") {
            std::string id = str_of(args, "id");
            e.menus.erase(std::remove_if(e.menus.begin(), e.menus.end(), [&](const Ext::MenuRec &m) { return m.id == id || m.parent == id; }), e.menus.end());
        } else if (op == "menus.removeAll") {
            e.menus.clear();
        } else { respond_err(r, "unknown " + op); return true; }
        respond_ok(r, "null");
        return true;
    }
    if (op == "webreq.listen") { e.webreq_listening = true; respond_ok(r, "null"); return true; }
    if (op == "port.connect") {
        std::string id = str_of(args, "id"), name = str_of(args, "name");
        PortEnd from = end_of(c), to;
        JsonNode *tn = member(args, "tabId");
        if (tn && JSON_NODE_HOLDS_VALUE(tn) && json_node_get_value_type(tn) != G_TYPE_STRING) {
            auto it = tab_views.find(static_cast<int>(json_node_get_int(tn)));
            if (it == tab_views.end()) { respond_err(r, kNoReceiver); return true; }
            to = PortEnd{it->second, world};
        } else {
            if (!e.bg || (c->tab_id < 0 && !c->page)) { respond_err(r, kNoReceiver); return true; }
            to = PortEnd{e.bg, ""};
        }
        if (id.empty() || !from.view) { respond_err(r, "bad port"); return true; }
        ports[id] = PortRec{from, to};
        respond_ok(r, "null");
        deliver_to(e, to, "window.__reedPortConnect(JSON.parse(p)); return null;",
                   "{\"id\":" + quote(id) + ",\"name\":" + quote(name) + ",\"sender\":" + sender_json(c, sender_url, top) + "}");
        return true;
    }
    if (op == "port.post" || op == "port.disconnect") {
        std::string id = str_of(args, "id");
        auto it = ports.find(id);
        respond_ok(r, "null");
        if (it == ports.end()) return true;
        PortEnd me = end_of(c);
        const PortEnd other = (it->second.a.view == me.view && it->second.a.world == me.world) ? it->second.b : it->second.a;
        if (op == "port.post") deliver_to(e, other, "window.__reedPortMsg(JSON.parse(p)); return null;", "{\"id\":" + quote(id) + ",\"data\":" + dump(member(args, "data")) + "}");
        else { deliver_to(e, other, "window.__reedPortDisc(JSON.parse(p)); return null;", quote(id)); ports.erase(it); }
        return true;
    }
    if (c->tab_id >= 0) return false;  // everything below is for the background page and extension pages

    // ---- options page
    if (op == "runtime.openOptionsPage") {
        if (e.options_page.empty() || !on_open_options) { respond_err(r, "This extension has no options page"); return true; }
        on_open_options(e.uuid);
        respond_ok(r, "null");
        return true;
    }

    // ---- script injection
    if (op == "tabs.executeScript" || op == "scripting.executeScript") {
        WebKitWebView *view = tab_view_for(member(args, "tabId"));
        if (!view) { respond_err(r, "No tab with that id, or the tab cannot be scripted"); return true; }
        bool main_world = str_of(args, "world") == "MAIN";
        std::string body, param;
        std::string func = str_of(args, "func");
        if (!func.empty()) { body = "return await (" + func + ").apply(null, JSON.parse(p));"; param = member(args, "args") ? dump(member(args, "args")) : "[]"; }
        else {
            std::string src = str_of(args, "code");
            for (auto &f : strings_of(args, "files")) src += "\n" + read_ext_file(e, f) + "\n;\n";
            if (src.empty()) { std::string f = str_of(args, "file"); if (!f.empty()) src = read_ext_file(e, f); }
            if (src.empty()) { respond_err(r, "No code or files to run"); return true; }
            body = "return (0, eval)(p);"; param = src;
        }
        bool scripting = op == "scripting.executeScript";
        run_js(view, main_world ? "" : world, body, param, [this, r, scripting](bool ok, const std::string &j) {
            if (!ok) { respond_err(r, j); return; }
            respond_ok(r, scripting ? "[{\"frameId\":0,\"result\":" + j + "}]" : "[" + j + "]");
        });
        return true;
    }
    if (op == "tabs.insertCSS" || op == "scripting.insertCSS" || op == "tabs.removeCSS" || op == "scripting.removeCSS") {
        WebKitWebView *view = tab_view_for(member(args, "tabId"));
        if (!view) { respond_err(r, "No tab with that id, or the tab cannot be scripted"); return true; }
        std::string css = str_of(args, "css");
        if (css.empty()) css = str_of(args, "code");
        for (auto &f : strings_of(args, "files")) css += read_ext_file(e, f) + "\n";
        if (css.empty()) { std::string f = str_of(args, "file"); if (!f.empty()) css = read_ext_file(e, f); }
        std::string key = sha_key(css);
        bool insert = op.find("insert") != std::string::npos;
        std::string body = insert
            ? "const a = JSON.parse(p); const s = document.createElement('style'); s.setAttribute('data-reed-css', a[0]); s.textContent = a[1]; (document.head || document.documentElement).appendChild(s); return null;"
            : "const a = JSON.parse(p); document.querySelectorAll('style[data-reed-css=\"' + a[0] + '\"]').forEach(function (x) { x.remove(); }); return null;";
        run_js(view, world, body, "[" + quote(key) + "," + quote(css) + "]", [this, r](bool ok, const std::string &j) { if (ok) respond_ok(r, "null"); else respond_err(r, j); });
        return true;
    }

    // ---- registered content scripts
    if (op == "scripting.registerContentScripts") {
        JsonArray *a = arr_of(args, "scripts");
        for (guint i = 0; a && i < json_array_get_length(a); ++i) {
            JsonObject *o = json_node_get_object(json_array_get_element(a, i));
            std::string err;
            if (!o || !register_script(e, o, &err)) { respond_err(r, err.empty() ? "invalid script" : err); return true; }
        }
        save_registered(e);
        respond_ok(r, "null");
        return true;
    }
    if (op == "contentScripts.register") {
        JsonNode *on = member(args, "opts"); JsonObject *o = on && JSON_NODE_HOLDS_OBJECT(on) ? json_node_get_object(on) : nullptr;
        std::string err;
        if (!o || !register_script(e, o, &err)) { respond_err(r, err.empty() ? "invalid script" : err); return true; }
        e.registered.back().persist = false;  // MV2: lasts until unregistered
        respond_ok(r, quote(e.registered.back().id));
        return true;
    }
    if (op == "scripting.unregisterContentScripts") {
        std::vector<std::string> ids = strings_of(args, "ids");
        bool all = ids.empty();
        for (auto it = e.registered.begin(); it != e.registered.end();) {
            if (all || std::find(ids.begin(), ids.end(), it->id) != ids.end()) { drop_registered(e, *it); it = e.registered.erase(it); }
            else ++it;
        }
        save_registered(e);
        respond_ok(r, "null");
        return true;
    }
    if (op == "scripting.updateContentScripts") {
        JsonArray *a = arr_of(args, "scripts");
        for (guint i = 0; a && i < json_array_get_length(a); ++i) {
            JsonObject *upd = json_node_get_object(json_array_get_element(a, i));
            if (!upd) continue;
            std::string id = str_of(upd, "id");
            auto it = std::find_if(e.registered.begin(), e.registered.end(), [&](Ext::Reg &g) { return g.id == id; });
            if (it == e.registered.end()) { respond_err(r, "No registered script with ID '" + id + "'"); return true; }
            Json merged = parse(it->raw);
            if (!merged.obj()) continue;
            GList *mem = json_object_get_members(upd);
            for (GList *l = mem; l; l = l->next) {
                const char *k = static_cast<const char *>(l->data);
                json_object_set_member(merged.obj(), k, json_node_copy(json_object_get_member(upd, k)));
            }
            g_list_free(mem);
            drop_registered(e, *it); e.registered.erase(it);
            std::string err;
            if (!register_script(e, merged.obj(), &err)) { respond_err(r, err); return true; }
        }
        save_registered(e);
        respond_ok(r, "null");
        return true;
    }
    if (op == "scripting.getRegisteredContentScripts") {
        std::vector<std::string> ids = strings_of(args, "ids");
        std::string out = "[";
        bool first = true;
        for (auto &g : e.registered) {
            if (!ids.empty() && std::find(ids.begin(), ids.end(), g.id) == ids.end()) continue;
            if (!first) out += ",";
            first = false; out += g.raw;
        }
        respond_ok(r, out + "]");
        return true;
    }

    if (op.rfind("cookies.", 0) == 0) return dispatch_cookies(c, op, args, r);

    // ---- declarativeNetRequest
    if (op.rfind("dnr.", 0) == 0) {
        std::string dir = store_dir + "/" + e.uuid;
        if (op == "dnr.updateDynamicRules" || op == "dnr.updateSessionRules") {
            bool dynamic = op == "dnr.updateDynamicRules";
            std::string &store = dynamic ? e.dynamic_rules : e.session_rules;
            Json cur = parse(store);
            std::vector<std::pair<long long, std::string>> items;
            if (JsonArray *a = cur.arr()) for (guint i = 0; i < json_array_get_length(a); ++i) {
                JsonNode *n = json_array_get_element(a, i);
                items.push_back({json_object_get_int_member(json_node_get_object(n), "id"), dump(n)});
            }
            if (JsonArray *rm = arr_of(args, "removeRuleIds"))
                for (guint i = 0; i < json_array_get_length(rm); ++i) {
                    long long id = json_node_get_int(json_array_get_element(rm, i));
                    items.erase(std::remove_if(items.begin(), items.end(), [&](auto &p) { return p.first == id; }), items.end());
                }
            if (JsonArray *ad = arr_of(args, "addRules"))
                for (guint i = 0; i < json_array_get_length(ad); ++i) {
                    JsonNode *n = json_array_get_element(ad, i);
                    JsonObject *o = json_node_get_object(n);
                    long long id = o && json_object_has_member(o, "id") ? json_object_get_int_member(o, "id") : 0;
                    if (!o || id <= 0) { respond_err(r, "Rules must have a positive integer id"); return true; }
                    for (auto &p : items) if (p.first == id) { respond_err(r, "Rule with id " + std::to_string(id) + " does not have a unique ID"); return true; }
                    items.push_back({id, dump(n)});
                }
            std::string out = "[";
            for (size_t i = 0; i < items.size(); ++i) { if (i) out += ","; out += items[i].second; }
            store = out + "]";
            if (dynamic) write_file(dir + "/dnr-dynamic.json", store);
            rebuild_dnr(e);
            respond_ok(r, "null");
            return true;
        }
        if (op == "dnr.getDynamicRules") { respond_ok(r, e.dynamic_rules); return true; }
        if (op == "dnr.getSessionRules") { respond_ok(r, e.session_rules); return true; }
        if (op == "dnr.getEnabledRulesets") { respond_ok(r, enabled_ids_json(e)); return true; }
        if (op == "dnr.getAvailableStaticRules" || op == "dnr.getDisabledRuleIds") { respond_ok(r, "[]"); return true; }
        if (op == "dnr.updateEnabledRulesets") {
            std::vector<std::string> on = strings_of(args, "enableRulesetIds"), off = strings_of(args, "disableRulesetIds");
            for (auto &id : on) if (std::none_of(e.rulesets.begin(), e.rulesets.end(), [&](auto &x) { return x.id == id; })) { respond_err(r, "Invalid ruleset id: " + id); return true; }
            for (auto &rs : e.rulesets) {
                if (std::find(on.begin(), on.end(), rs.id) != on.end()) rs.enabled = true;
                if (std::find(off.begin(), off.end(), rs.id) != off.end()) rs.enabled = false;
            }
            write_file(dir + "/dnr-enabled.json", enabled_ids_json(e));
            rebuild_dnr(e);
            respond_ok(r, "null");
            return true;
        }
        if (op == "dnr.getAvailableStaticRuleCount") { respond_ok(r, "300000"); return true; }
        if (op == "dnr.isRegexSupported") { respond_ok(r, dnr::webkit_regex_ok(str_of(args, "regex")) ? "{\"isSupported\":true}" : "{\"isSupported\":false,\"reason\":\"syntaxError\"}"); return true; }
        if (op == "dnr.getMatchedRules") { respond_ok(r, "{\"rulesMatchedInfo\":[]}"); return true; }
        if (op == "dnr.noop") { respond_ok(r, "null"); return true; }
    }
    return false;
}


// ---------------------------------------------------------------- public: toolbar actions and pages
static std::string details_string(const Ext &e, const char *key, const char *field) {
    auto it = e.action.find(key);
    if (it == e.action.end()) return "";
    Json j = parse(it->second);
    if (JsonObject *o = j.obj()) return str_of(o, field);
    return "";
}

std::vector<ExtensionManager::MenuItem> ExtensionManager::context_menu_items(const std::vector<std::string> &contexts) const {
    std::vector<MenuItem> out;
    auto hit = [&](const Ext::MenuRec &m) {
        if (!m.visible) return false;
        for (auto &c : m.contexts) {
            if (c == "all") return true;
            if (std::find(contexts.begin(), contexts.end(), c) != contexts.end()) return true;
        }
        return false;
    };
    for (auto &e : d_->exts) {
        if (!e->enabled) continue;
        for (auto &m : e->menus) {
            bool show = hit(m);
            if (!show && m.type != "separator")
                for (auto &k : e->menus) if (k.parent == m.id && hit(k)) { show = true; break; }  // container with a matching child
            if (m.parent.size() && !hit(m)) show = false;
            if (!show) continue;
            out.push_back(MenuItem{e->uuid, e->name, m.id, m.title, m.parent, m.type, m.enabled, m.checked});
        }
    }
    return out;
}

void ExtensionManager::context_menu_clicked(const std::string &uuid, const MenuClick &c, int tab_id) {
    Ext *e = d_->by_uuid(uuid);
    if (!e || !e->bg) return;
    std::string info = "{\"menuItemId\":" + quote(c.menu_id) + ",\"pageUrl\":" + quote(c.page_url) + ",\"editable\":" + (c.editable ? "true" : "false");
    if (!c.link_url.empty()) info += ",\"linkUrl\":" + quote(c.link_url);
    if (!c.src_url.empty()) info += ",\"srcUrl\":" + quote(c.src_url);
    if (!c.media_type.empty()) info += ",\"mediaType\":" + quote(c.media_type);
    if (!c.selection.empty()) info += ",\"selectionText\":" + quote(c.selection);
    for (auto &m : e->menus) if (m.id == c.menu_id && (m.type == "checkbox" || m.type == "radio")) info += std::string(",\"wasChecked\":") + (m.checked ? "true" : "false") + ",\"checked\":" + (c.checked ? "true" : "false");
    info += "}";
    std::string tab = "{\"id\":" + std::to_string(tab_id) + "}";
    if (d_->host.list_tabs) for (auto &t : d_->host.list_tabs()) if (t.id == tab_id) tab = d_->tab_json(t);
    d_->fire(*e, "menus.onClicked", "[" + info + "," + tab + "]", false);
    for (auto &m : e->menus) if (m.id == c.menu_id && (m.type == "checkbox" || m.type == "radio")) m.checked = c.checked;
}

std::vector<ExtensionManager::ActionInfo> ExtensionManager::actions() const {
    std::vector<ActionInfo> out;
    for (auto &up : d_->exts) {
        const Ext &e = *up;
        if (!e.enabled || !e.has_action) continue;
        ActionInfo a;
        a.uuid = e.uuid; a.name = e.name;
        a.title = details_string(e, "setTitle", "title");
        if (a.title.empty()) a.title = e.default_title.empty() ? e.name : e.default_title;
        a.badge = details_string(e, "setBadgeText", "text");
        a.badge_color = details_string(e, "setBadgeBackgroundColor", "color");
        a.popup = e.action.count("setPopup") ? details_string(e, "setPopup", "popup") : e.popup;
        if (!e.icon_rel.empty()) {
            fs::path r = fs::path(e.icon_rel).relative_path().lexically_normal();
            a.icon_path = (fs::path(e.dir) / r).string();
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<ExtensionManager::Summary> ExtensionManager::list() const {
    std::vector<Summary> out;
    for (auto &e : d_->exts) out.push_back({e->uuid, e->id, e->name, e->version, e->enabled, !e->options_page.empty()});
    return out;
}

WebKitWebView *ExtensionManager::create_popup(const std::string &uuid) {
    Ext *e = d_->by_uuid(uuid);
    if (!e || !e->enabled) return nullptr;
    std::string popup = e->action.count("setPopup") ? details_string(*e, "setPopup", "popup") : e->popup;
    if (popup.empty()) return nullptr;
    WebKitWebView *v = d_->make_page_view(*e, "popup");
    std::string url = "moz-extension://" + e->uuid + "/" + popup;
    webkit_web_view_load_uri(v, url.c_str());
    return v;
}

WebKitWebView *ExtensionManager::create_options(const std::string &uuid) {
    Ext *e = d_->by_uuid(uuid);
    if (!e || !e->enabled || e->options_page.empty()) return nullptr;
    WebKitWebView *v = d_->make_page_view(*e, "options");
    std::string url = "moz-extension://" + e->uuid + "/" + e->options_page;
    webkit_web_view_load_uri(v, url.c_str());
    return v;
}

void ExtensionManager::action_clicked(const std::string &uuid, int tab_id) {
    Ext *e = d_->by_uuid(uuid);
    if (!e || !e->enabled || !e->bg) return;
    std::string tab = "{\"id\":" + std::to_string(tab_id) + "}";
    if (d_->host.list_tabs) for (auto &t : d_->host.list_tabs()) if (t.id == tab_id) tab = d_->tab_json(t);
    d_->fire(*e, "action.onClicked", "[" + tab + ",{\"modifiers\":[],\"button\":0}]", false);
}

void ExtensionManager::set_ui_callbacks(std::function<void()> actions_changed, std::function<void(const std::string &)> open_options) {
    d_->on_actions_changed = std::move(actions_changed);
    d_->on_open_options = std::move(open_options);
}

void ExtensionManager::start_new() {
    for (auto &e : d_->exts) if (e->enabled && !e->bg_started) { d_->load_dnr_state(*e); d_->start_background(*e); }
    d_->notify_actions();
}

void ExtensionManager::set_enabled(const std::string &uuid, bool on) {
    Ext *e = d_->by_uuid(uuid);
    if (!e || e->enabled == on) return;
    e->enabled = on;
    d_->save_registry();
    d_->notify_actions();
}

void ExtensionManager::start() {
    d_->register_scheme();
    for (auto &e : d_->exts) if (e->enabled) d_->load_dnr_state(*e);
    for (auto &e : d_->exts) if (e->enabled) d_->start_background(*e);
}

// -------------------------------------------------------------- tab attachment

void ExtensionManager::attach_view(WebKitWebView *view, WebKitUserContentManager *ucm, int tab_id, bool is_private) {
    if (is_private) return;  // like Firefox, extensions do not run in private windows by default
    Impl::ViewInfo info{ucm, tab_id, {}};
    for (auto &up : d_->exts) {
        Ext &e = *up;
        bool wants_api = !e.scripts.empty() || !e.registered.empty() || e.has_permission("scripting") || e.has_permission("contentScripts");
        if (!e.enabled || !wants_api) continue;
        std::string world = "reed-ext-" + e.uuid;
        webkit_user_content_manager_register_script_message_handler_with_reply(ucm, e.handler.c_str(), world.c_str());
        auto *ctx = new MsgCtx{d_.get(), &e, tab_id, view};
        g_signal_connect_data(ucm, ("script-message-with-reply-received::" + e.handler).c_str(),
            G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *v, WebKitScriptMessageReply *reply, gpointer ud) -> gboolean {
                auto *c = static_cast<MsgCtx *>(ud);
                c->mgr->on_message(c, v, reply);
                return TRUE;
            }), ctx, +[](gpointer d, GClosure *) { delete static_cast<MsgCtx *>(d); }, GConnectFlags(0));

        std::vector<std::string> all;
        for (auto &s : e.scripts) for (auto &m : s.matches) convert_pattern(m, all);
        for (auto &g : e.registered) for (auto &m : g.matches) convert_pattern(m, all);
        if (e.has_permission("scripting") || e.has_permission("contentScripts")) convert_pattern("<all_urls>", all);
        if (all.empty()) continue;
        auto allv = to_cstrv(all);
        std::string api = d_->polyfill(e, "content");
        WebKitUserScript *pf = webkit_user_script_new_for_world(api.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, world.c_str(), allv.data(), nullptr);
        webkit_user_content_manager_add_script(ucm, pf);
        webkit_user_script_unref(pf);

        for (auto &s : e.scripts) {
            std::vector<std::string> allow, block;
            for (auto &m : s.matches) convert_pattern(m, allow);
            for (auto &m : s.excludes) convert_pattern(m, block);
            if (allow.empty()) continue;
            auto av = to_cstrv(allow), bv = to_cstrv(block);
            auto frames = s.all_frames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
            for (auto &css : s.css) {
                std::string text = read_file(e.dir + "/" + css);
                if (text.empty()) continue;
                WebKitUserStyleSheet *ss = webkit_user_style_sheet_new_for_world(text.c_str(), frames, WEBKIT_USER_STYLE_LEVEL_USER, world.c_str(), av.data(), block.empty() ? nullptr : bv.data());
                webkit_user_content_manager_add_style_sheet(ucm, ss);
                webkit_user_style_sheet_unref(ss);
            }
            std::string code;
            for (auto &js : s.js) code += "\n// ---- " + js + "\n" + read_file(e.dir + "/" + js) + "\n;\n";
            if (code.empty()) continue;
            auto when = s.run_at == "document_start" ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;
            WebKitUserScript *us = webkit_user_script_new_for_world(code.c_str(), frames, when, world.c_str(), av.data(), block.empty() ? nullptr : bv.data());
            webkit_user_content_manager_add_script(ucm, us);
            webkit_user_script_unref(us);
        }
        for (auto &g : e.registered) d_->add_registered_to(e, g, ucm);
        info.attached.push_back(&e);
    }
    for (auto &up : d_->exts) if (up->enabled && up->dnr_filter) webkit_user_content_manager_add_filter(ucm, up->dnr_filter);
    g_object_set_data(G_OBJECT(view), "reed-tab-id", GINT_TO_POINTER(tab_id));
    g_signal_connect(view, "resource-load-started", G_CALLBACK(on_resource_started), d_.get());
    d_->views[view] = info;
    d_->tab_views[tab_id] = view;
}

void ExtensionManager::detach_view(WebKitWebView *view) {
    auto it = d_->views.find(view);
    if (it == d_->views.end()) return;
    d_->tab_views.erase(it->second.tab_id);
    d_->views.erase(it);
    d_->drop_ports_for(view);
}

static std::string tab_args(ExtensionManager::Impl *d, int id) {
    if (d->host.list_tabs) for (auto &t : d->host.list_tabs()) if (t.id == id) return d->tab_json(t);
    return "{\"id\":" + std::to_string(id) + "}";
}
void ExtensionManager::tab_created(int id) { d_->fire_all("tabs.onCreated", "[" + tab_args(d_.get(), id) + "]"); }
void ExtensionManager::tab_removed(int id) { d_->fire_all("tabs.onRemoved", "[" + std::to_string(id) + ",{\"windowId\":1,\"isWindowClosing\":false}]"); }
void ExtensionManager::tab_activated(int id) { d_->fire_all("tabs.onActivated", "[{\"tabId\":" + std::to_string(id) + ",\"windowId\":1}]"); }
void ExtensionManager::tab_committed(int id, const std::string &url) {
    std::string d = "[{\"tabId\":" + std::to_string(id) + ",\"url\":" + quote(url) + ",\"frameId\":0,\"parentFrameId\":-1,\"processId\":-1,\"timeStamp\":" + std::to_string(g_get_real_time() / 1000) + ",\"transitionType\":\"link\",\"transitionQualifiers\":[]}]";
    d_->fire_all("webNavigation.onBeforeNavigate", d);
    d_->fire_all("webNavigation.onCommitted", d);
}
void ExtensionManager::tab_updated(int id, const std::string &status, const std::string &url, const std::string &title) {
    if (status == "complete" && !url.empty()) {
        std::string d = "[{\"tabId\":" + std::to_string(id) + ",\"url\":" + quote(url) + ",\"frameId\":0,\"parentFrameId\":-1,\"processId\":-1,\"timeStamp\":" + std::to_string(g_get_real_time() / 1000) + "}]";
        d_->fire_all("webNavigation.onDOMContentLoaded", d);
        d_->fire_all("webNavigation.onCompleted", d);
    }
    if (status == "loading") { auto tv = d_->tab_views.find(id); if (tv != d_->tab_views.end()) d_->drop_ports_for(tv->second); }  // new document: old content-script ports are gone
    std::string change = "{\"status\":" + quote(status);
    if (!url.empty()) change += ",\"url\":" + quote(url);
    if (!title.empty()) change += ",\"title\":" + quote(title);
    d_->fire_all("tabs.onUpdated", "[" + std::to_string(id) + "," + change + "}," + tab_args(d_.get(), id) + "]");
}
