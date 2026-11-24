// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
// Bookmark import: Firefox (places.sqlite), Chromium family (Bookmarks JSON), Netscape HTML.
// Pure functions, no GTK, so they can be tested on their own (see tools/test-importer.cpp).
#pragma once

#include <json-glib/json-glib.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "store.hpp"

namespace importer {

namespace fs = std::filesystem;

struct Source {
    std::string label;
    std::string path;
    std::string kind;  // "firefox" | "chromium" | "html"
};

inline bool importable_uri(const std::string &u) {
    return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0 || u.rfind("ftp://", 0) == 0;
}

// ---------------------------------------------------------------- Chromium
inline void walk_chromium(JsonNode *node, const std::string &folder, std::vector<Bookmark> &out) {
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return;
    JsonObject *o = json_node_get_object(node);
    auto str = [&](const char *k) -> std::string {
        if (!json_object_has_member(o, k)) return "";
        const char *v = json_object_get_string_member_with_default(o, k, "");
        return v ? v : "";
    };
    std::string type = str("type"), name = str("name");
    if (type == "url") {
        std::string url = str("url");
        if (importable_uri(url)) out.push_back({0, name, url, folder});
    } else if (type == "folder") {
        std::string sub = folder.empty() ? name : folder + " / " + name;
        if (json_object_has_member(o, "children")) {
            JsonArray *kids = json_object_get_array_member(o, "children");
            for (guint i = 0; kids && i < json_array_get_length(kids); ++i) walk_chromium(json_array_get_element(kids, i), sub, out);
        }
    }
}

inline std::vector<Bookmark> parse_chromium(const std::string &path, std::string *err) {
    std::vector<Bookmark> out;
    GError *e = nullptr;
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_file(p, path.c_str(), &e)) {
        if (err) *err = e ? e->message : "could not read file";
        if (e) g_error_free(e);
        g_object_unref(p);
        return out;
    }
    JsonNode *root = json_parser_get_root(p);
    if (root && JSON_NODE_HOLDS_OBJECT(root) && json_object_has_member(json_node_get_object(root), "roots")) {
        JsonObject *roots = json_object_get_object_member(json_node_get_object(root), "roots");
        // bookmark_bar -> "Bookmarks bar", other -> "Other bookmarks", synced -> "Mobile bookmarks"
        for (const char *key : {"bookmark_bar", "other", "synced"}) {
            if (!json_object_has_member(roots, key)) continue;
            JsonNode *n = json_object_get_member(roots, key);
            if (!n || !JSON_NODE_HOLDS_OBJECT(n)) continue;
            JsonObject *ro = json_node_get_object(n);
            if (!json_object_has_member(ro, "children")) continue;
            JsonArray *kids = json_object_get_array_member(ro, "children");
            const char *name = json_object_get_string_member_with_default(ro, "name", key);
            for (guint i = 0; kids && i < json_array_get_length(kids); ++i)
                walk_chromium(json_array_get_element(kids, i), name ? name : key, out);
        }
    } else if (err) *err = "not a Chromium bookmarks file";
    g_object_unref(p);
    return out;
}

// ---------------------------------------------------------------- Firefox
inline std::vector<Bookmark> parse_firefox(const std::string &places, std::string *err) {
    std::vector<Bookmark> out;
    // Firefox keeps the DB locked (WAL) while running: work on a private copy.
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec) / ("reed-import-" + std::to_string(g_get_monotonic_time()));
    fs::create_directories(tmp, ec);
    fs::path copy = tmp / "places.sqlite";
    fs::copy_file(places, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) { if (err) *err = "could not read " + places + ": " + ec.message(); fs::remove_all(tmp, ec); return out; }
    for (const char *suffix : {"-wal", "-shm"}) {
        std::error_code e2;
        if (fs::exists(places + suffix, e2)) fs::copy_file(places + suffix, tmp / (std::string("places.sqlite") + suffix), fs::copy_options::overwrite_existing, e2);
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(copy.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (err) *err = "could not open Firefox database";
        if (db) sqlite3_close(db);
        fs::remove_all(tmp, ec);
        return out;
    }
    struct Node { long long parent = 0; int type = 0; std::string title, guid, url; };
    std::map<long long, Node> nodes;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT b.id,b.parent,b.type,COALESCE(b.title,''),COALESCE(b.guid,''),COALESCE(p.url,'') "
                               "FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id=b.fk ORDER BY b.parent,b.position", -1, &st, nullptr) != SQLITE_OK) {
        if (err) *err = "unexpected Firefox database layout";
        sqlite3_close(db); fs::remove_all(tmp, ec);
        return out;
    }
    auto col = [&](int i) { const unsigned char *t = sqlite3_column_text(st, i); return std::string(t ? reinterpret_cast<const char *>(t) : ""); };
    std::vector<long long> order;
    while (sqlite3_step(st) == SQLITE_ROW) {
        Node n; long long id = sqlite3_column_int64(st, 0);
        n.parent = sqlite3_column_int64(st, 1); n.type = sqlite3_column_int(st, 2);
        n.title = col(3); n.guid = col(4); n.url = col(5);
        nodes[id] = n; order.push_back(id);
    }
    sqlite3_finalize(st); sqlite3_close(db); fs::remove_all(tmp, ec);

    auto root_name = [](const std::string &guid) -> std::string {
        if (guid == "toolbar_____") return "Bookmarks Toolbar";
        if (guid == "menu________") return "Bookmarks Menu";
        if (guid == "unfiled_____") return "Other Bookmarks";
        if (guid == "mobile______") return "Mobile Bookmarks";
        return "";
    };
    auto folder_path = [&](long long id) {
        std::vector<std::string> parts;
        for (int guard = 0; id && guard < 64; ++guard) {
            auto it = nodes.find(id);
            if (it == nodes.end()) break;
            std::string rn = root_name(it->second.guid);
            if (it->second.guid == "root________") break;
            parts.push_back(!rn.empty() ? rn : it->second.title);
            id = it->second.parent;
        }
        std::reverse(parts.begin(), parts.end());
        std::string r;
        for (auto &p : parts) { if (!r.empty()) r += " / "; r += p; }
        return r;
    };
    for (long long id : order) {
        const Node &n = nodes[id];
        if (n.type != 1 || !importable_uri(n.url)) continue;  // 1 = bookmark (2 = folder, 3 = separator)
        out.push_back({0, n.title, n.url, folder_path(n.parent)});
    }
    return out;
}

// ---------------------------------------------------------------- HTML (Netscape bookmark file)
inline std::string decode_entities(std::string s) {
    static const std::pair<const char *, const char *> map[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"}, {"&apos;", "'"}};
    for (auto &m : map) {
        size_t pos = 0; std::string from = m.first, to = m.second;
        while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    }
    return s;
}
inline std::string lower(std::string s) { for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

inline std::string attr_value(const std::string &tag, const std::string &name) {
    std::string lt = lower(tag);
    size_t p = 0;
    while ((p = lt.find(name, p)) != std::string::npos) {
        bool boundary = p == 0 || std::isspace(static_cast<unsigned char>(lt[p - 1]));
        size_t q = p + name.size();
        while (q < lt.size() && std::isspace(static_cast<unsigned char>(lt[q]))) ++q;
        if (boundary && q < lt.size() && lt[q] == '=') {
            ++q;
            while (q < lt.size() && std::isspace(static_cast<unsigned char>(lt[q]))) ++q;
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                char quote = tag[q]; size_t end = tag.find(quote, q + 1);
                return end == std::string::npos ? "" : tag.substr(q + 1, end - q - 1);
            }
            size_t end = tag.find_first_of(" \t\r\n>", q);
            return tag.substr(q, end == std::string::npos ? std::string::npos : end - q);
        }
        p = q;
    }
    return "";
}

inline std::vector<Bookmark> parse_html(const std::string &html) {
    std::vector<Bookmark> out;
    std::string lh = lower(html);
    std::vector<std::string> stack;     // folder names for open <DL> levels
    std::string pending_folder;         // name from the last <H3>, applied to the next <DL>
    size_t i = 0;
    while (i < html.size()) {
        size_t lt = html.find('<', i);
        if (lt == std::string::npos) break;
        size_t gt = html.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = html.substr(lt, gt - lt + 1);
        std::string ltag = lh.substr(lt, gt - lt + 1);
        if (ltag.rfind("<h3", 0) == 0) {
            size_t end = lh.find("</h3", gt);
            if (end == std::string::npos) end = html.size();
            pending_folder = decode_entities(html.substr(gt + 1, end - gt - 1));
            i = end;
            continue;
        }
        if (ltag.rfind("<dl", 0) == 0) { stack.push_back(pending_folder); pending_folder.clear(); }
        else if (ltag.rfind("</dl", 0) == 0) { if (!stack.empty()) stack.pop_back(); }
        else if (ltag.size() > 2 && ltag.rfind("<a", 0) == 0 && std::isspace(static_cast<unsigned char>(ltag[2]))) {
            std::string href = decode_entities(attr_value(tag, "href"));
            size_t end = lh.find("</a", gt);
            if (end == std::string::npos) end = html.size();
            std::string title = decode_entities(html.substr(gt + 1, end - gt - 1));
            std::string folder;
            for (auto &f : stack) if (!f.empty()) { if (!folder.empty()) folder += " / "; folder += f; }
            if (importable_uri(href)) out.push_back({0, title, href, folder});
            i = end;
            continue;
        }
        i = gt + 1;
    }
    return out;
}

inline std::vector<Bookmark> parse_html_file(const std::string &path, std::string *err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "could not open " + path; return {}; }
    std::stringstream ss; ss << f.rdbuf();
    return parse_html(ss.str());
}

// ---------------------------------------------------------------- discovery
inline std::vector<Source> detect_sources() {
    std::vector<Source> found;
    std::error_code ec;
    const char *h = g_get_home_dir();
    fs::path home = h ? h : ".";

    // Firefox profiles (native, Flatpak, Snap), skipping profiles without bookmarks.
    for (const char *root : {".mozilla/firefox", ".var/app/org.mozilla.firefox/.mozilla/firefox", "snap/firefox/common/.mozilla/firefox",
                             ".librewolf", ".var/app/io.gitlab.librewolf-community/.librewolf", ".zen"}) {
        fs::path dir = home / root;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto &entry : fs::directory_iterator(dir, ec)) {
            fs::path pl = entry.path() / "places.sqlite";
            if (entry.is_directory(ec) && fs::exists(pl, ec))
                found.push_back({std::string(root).find("librewolf") != std::string::npos ? "LibreWolf — " + entry.path().filename().string()
                                 : std::string(root).find("zen") != std::string::npos ? "Zen — " + entry.path().filename().string()
                                 : "Firefox — " + entry.path().filename().string(), pl.string(), "firefox"});
        }
    }
    struct Chrome { const char *label, *dir; };
    for (const Chrome &c : {Chrome{"Google Chrome", ".config/google-chrome"}, Chrome{"Chromium", ".config/chromium"},
                            Chrome{"Brave", ".config/BraveSoftware/Brave-Browser"}, Chrome{"Vivaldi", ".config/vivaldi"},
                            Chrome{"Microsoft Edge", ".config/microsoft-edge"}, Chrome{"Opera", ".config/opera"},
                            Chrome{"Chromium (Flatpak)", ".var/app/org.chromium.Chromium/config/chromium"},
                            Chrome{"Chrome (Flatpak)", ".var/app/com.google.Chrome/config/google-chrome"},
                            Chrome{"Brave (Flatpak)", ".var/app/com.brave.Browser/config/BraveSoftware/Brave-Browser"}}) {
        fs::path dir = home / c.dir;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto &entry : fs::directory_iterator(dir, ec)) {
            std::string n = entry.path().filename().string();
            if (!entry.is_directory(ec) || (n != "Default" && n.rfind("Profile ", 0) != 0)) continue;
            fs::path bm = entry.path() / "Bookmarks";
            if (fs::exists(bm, ec)) found.push_back({std::string(c.label) + " — " + n, bm.string(), "chromium"});
        }
        fs::path opera = dir / "Bookmarks";  // Opera keeps it at the top level
        if (fs::exists(opera, ec)) found.push_back({std::string(c.label), opera.string(), "chromium"});
    }
    return found;
}

inline std::vector<Bookmark> load(const Source &s, std::string *err) {
    if (s.kind == "firefox") return parse_firefox(s.path, err);
    if (s.kind == "chromium") return parse_chromium(s.path, err);
    return parse_html_file(s.path, err);
}

// Guess the kind from a user-picked file.
inline Source classify(const std::string &path) {
    std::string name = lower(fs::path(path).filename().string());
    if (name == "places.sqlite" || (name.size() > 7 && name.substr(name.size() - 7) == ".sqlite")) return {path, path, "firefox"};
    if (name == "bookmarks" || (name.size() > 5 && name.substr(name.size() - 5) == ".json")) return {path, path, "chromium"};
    return {path, path, "html"};
}

}  // namespace importer
