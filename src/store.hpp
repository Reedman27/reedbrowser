// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
// Persistent browser data: history, bookmarks, per-site zoom, session, downloads.
#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

struct Row {
    long long id = 0;
    std::string title, uri;
};

struct DownloadRow {
    std::string name, path, state;
};

class Store {
public:
    bool open(const std::string &path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }
        sqlite3_busy_timeout(db_, 2000);
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("CREATE TABLE IF NOT EXISTS history(id INTEGER PRIMARY KEY, title TEXT NOT NULL, uri TEXT NOT NULL, visited INTEGER NOT NULL);");
        exec("CREATE INDEX IF NOT EXISTS history_visited ON history(visited DESC);");
        exec("CREATE TABLE IF NOT EXISTS bookmarks(id INTEGER PRIMARY KEY, title TEXT NOT NULL, uri TEXT NOT NULL UNIQUE, created INTEGER NOT NULL);");
        exec("CREATE TABLE IF NOT EXISTS zoom(host TEXT PRIMARY KEY, level REAL NOT NULL);");
        exec("CREATE TABLE IF NOT EXISTS session(pos INTEGER PRIMARY KEY, uri TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 0);");
        exec("CREATE TABLE IF NOT EXISTS downloads(id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL, state TEXT NOT NULL, finished INTEGER NOT NULL);");
        exec("DELETE FROM history WHERE id NOT IN (SELECT id FROM history ORDER BY visited DESC LIMIT 20000);");
        return true;
    }
    ~Store() { if (db_) sqlite3_close(db_); }

    // ---- history
    void record_visit(const std::string &title, const std::string &uri) {
        // Same page as the latest entry (reload, redirect settle): refresh it, don't duplicate.
        sqlite3_stmt *st = prep("SELECT id, uri FROM history ORDER BY visited DESC, id DESC LIMIT 1");
        long long last = 0; std::string last_uri;
        if (st && sqlite3_step(st) == SQLITE_ROW) { last = sqlite3_column_int64(st, 0); last_uri = text(st, 1); }
        sqlite3_finalize(st);
        if (last && last_uri == uri) {
            st = prep("UPDATE history SET title=?, visited=strftime('%s','now') WHERE id=?");
            if (st) { bind(st, 1, title); sqlite3_bind_int64(st, 2, last); sqlite3_step(st); }
            sqlite3_finalize(st);
            return;
        }
        st = prep("INSERT INTO history(title,uri,visited) VALUES(?,?,strftime('%s','now'))");
        if (st) { bind(st, 1, title.empty() ? uri : title); bind(st, 2, uri); sqlite3_step(st); }
        sqlite3_finalize(st);
    }
    std::vector<Row> history(const std::string &filter, int limit = 500) {
        std::vector<Row> out;
        sqlite3_stmt *st = prep("SELECT id,title,uri FROM history WHERE title LIKE ?1 ESCAPE '\\' OR uri LIKE ?1 ESCAPE '\\' ORDER BY visited DESC, id DESC LIMIT ?2");
        if (!st) return out;
        bind(st, 1, like(filter));
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back({sqlite3_column_int64(st, 0), text(st, 1), text(st, 2)});
        sqlite3_finalize(st);
        return out;
    }
    std::vector<Row> recent_unique(int limit) {
        std::vector<Row> out;
        sqlite3_stmt *st = prep("SELECT MAX(id),title,uri FROM history GROUP BY uri ORDER BY MAX(visited) DESC, MAX(id) DESC LIMIT ?1");
        if (!st) return out;
        sqlite3_bind_int(st, 1, limit);
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back({sqlite3_column_int64(st, 0), text(st, 1), text(st, 2)});
        sqlite3_finalize(st);
        return out;
    }
    void delete_history(long long id) { run1("DELETE FROM history WHERE id=?", id); }
    void clear_history() { exec("DELETE FROM history;"); }

    // ---- bookmarks
    bool is_bookmarked(const std::string &uri) {
        sqlite3_stmt *st = prep("SELECT 1 FROM bookmarks WHERE uri=?");
        bool yes = false;
        if (st) { bind(st, 1, uri); yes = sqlite3_step(st) == SQLITE_ROW; }
        sqlite3_finalize(st);
        return yes;
    }
    void add_bookmark(const std::string &title, const std::string &uri) {
        sqlite3_stmt *st = prep("INSERT OR IGNORE INTO bookmarks(title,uri,created) VALUES(?,?,strftime('%s','now'))");
        if (st) { bind(st, 1, title.empty() ? uri : title); bind(st, 2, uri); sqlite3_step(st); }
        sqlite3_finalize(st);
    }
    void remove_bookmark_uri(const std::string &uri) {
        sqlite3_stmt *st = prep("DELETE FROM bookmarks WHERE uri=?");
        if (st) { bind(st, 1, uri); sqlite3_step(st); }
        sqlite3_finalize(st);
    }
    void delete_bookmark(long long id) { run1("DELETE FROM bookmarks WHERE id=?", id); }
    std::vector<Row> bookmarks(const std::string &filter = "") {
        std::vector<Row> out;
        sqlite3_stmt *st = prep("SELECT id,title,uri FROM bookmarks WHERE title LIKE ?1 ESCAPE '\\' OR uri LIKE ?1 ESCAPE '\\' ORDER BY title COLLATE NOCASE");
        if (!st) return out;
        bind(st, 1, like(filter));
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back({sqlite3_column_int64(st, 0), text(st, 1), text(st, 2)});
        sqlite3_finalize(st);
        return out;
    }

    // ---- zoom
    double zoom_for(const std::string &host) {
        double level = 1.0;
        sqlite3_stmt *st = prep("SELECT level FROM zoom WHERE host=?");
        if (st) { bind(st, 1, host); if (sqlite3_step(st) == SQLITE_ROW) level = sqlite3_column_double(st, 0); }
        sqlite3_finalize(st);
        return level;
    }
    void set_zoom(const std::string &host, double level) {
        sqlite3_stmt *st = (level > 0.99 && level < 1.01) ? prep("DELETE FROM zoom WHERE host=?1")
                                                         : prep("INSERT OR REPLACE INTO zoom(host,level) VALUES(?1,?2)");
        if (!st) return;
        bind(st, 1, host);
        if (sqlite3_bind_parameter_count(st) > 1) sqlite3_bind_double(st, 2, level);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    // ---- session
    void save_session(const std::vector<std::string> &uris, int active) {
        exec("DELETE FROM session;");
        for (size_t i = 0; i < uris.size(); ++i) {
            sqlite3_stmt *st = prep("INSERT INTO session(pos,uri,active) VALUES(?,?,?)");
            if (!st) continue;
            sqlite3_bind_int(st, 1, static_cast<int>(i));
            bind(st, 2, uris[i]);
            sqlite3_bind_int(st, 3, static_cast<int>(i) == active ? 1 : 0);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    std::vector<std::string> load_session(int *active) {
        std::vector<std::string> out;
        *active = 0;
        sqlite3_stmt *st = prep("SELECT uri,active FROM session ORDER BY pos");
        if (!st) return out;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (sqlite3_column_int(st, 1)) *active = static_cast<int>(out.size());
            out.push_back(text(st, 0));
        }
        sqlite3_finalize(st);
        return out;
    }

    // ---- downloads
    void add_download(const std::string &name, const std::string &path, const std::string &state) {
        sqlite3_stmt *st = prep("INSERT INTO downloads(name,path,state,finished) VALUES(?,?,?,strftime('%s','now'))");
        if (st) { bind(st, 1, name); bind(st, 2, path); bind(st, 3, state); sqlite3_step(st); }
        sqlite3_finalize(st);
        exec("DELETE FROM downloads WHERE id NOT IN (SELECT id FROM downloads ORDER BY id DESC LIMIT 100);");
    }
    std::vector<DownloadRow> recent_downloads(int limit) {
        std::vector<DownloadRow> out;
        sqlite3_stmt *st = prep("SELECT name,path,state FROM downloads ORDER BY id DESC LIMIT ?");
        if (!st) return out;
        sqlite3_bind_int(st, 1, limit);
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back({text(st, 0), text(st, 1), text(st, 2)});
        sqlite3_finalize(st);
        return out;
    }
    void clear_downloads() { exec("DELETE FROM downloads;"); }

private:
    sqlite3 *db_ = nullptr;

    void exec(const char *sql) { if (db_) sqlite3_exec(db_, sql, nullptr, nullptr, nullptr); }
    sqlite3_stmt *prep(const char *sql) {
        sqlite3_stmt *st = nullptr;
        if (!db_ || sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return nullptr;
        return st;
    }
    static void bind(sqlite3_stmt *st, int i, const std::string &s) { sqlite3_bind_text(st, i, s.c_str(), -1, SQLITE_TRANSIENT); }
    static std::string text(sqlite3_stmt *st, int col) {
        const unsigned char *t = sqlite3_column_text(st, col);
        return t ? reinterpret_cast<const char *>(t) : "";
    }
    void run1(const char *sql, long long id) {
        sqlite3_stmt *st = prep(sql);
        if (st) { sqlite3_bind_int64(st, 1, id); sqlite3_step(st); }
        sqlite3_finalize(st);
    }
    static std::string like(const std::string &f) {
        std::string r = "%";
        for (char c : f) { if (c == '%' || c == '_' || c == '\\') r.push_back('\\'); r.push_back(c); }
        return r + "%";
    }
};
