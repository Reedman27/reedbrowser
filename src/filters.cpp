// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
#include "filters.hpp"

#include <curl/curl.h>
#include <glib/gstdio.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "abp.hpp"

namespace fs = std::filesystem;

static const char *kFormatVersion = "abp1";

const std::vector<FilterList> &FilterManager::catalog() {
    static const std::vector<FilterList> v = {
        {"easylist", "EasyList", "The standard ad-blocking list.", "https://easylist.to/easylist/easylist.txt", true},
        {"easyprivacy", "EasyPrivacy", "Trackers and analytics.", "https://easylist.to/easylist/easyprivacy.txt", true},
        {"ubo-filters", "uBlock filters", "uBlock Origin's own filters (ads, trackers, anti-adblock).", "https://ublockorigin.github.io/uAssets/filters/filters.min.txt", true},
        {"ubo-privacy", "uBlock privacy", "uBlock Origin's privacy filters.", "https://ublockorigin.github.io/uAssets/filters/privacy.min.txt", true},
        {"ubo-badware", "uBlock badware", "Malware, phishing and scam domains.", "https://ublockorigin.github.io/uAssets/filters/badware.min.txt", true},
        {"ubo-unbreak", "uBlock unbreak", "Fixes for sites broken by the lists above.", "https://ublockorigin.github.io/uAssets/filters/unbreak.min.txt", true},
        {"ubo-quick", "uBlock quick fixes", "Fast-moving fixes (e.g. for YouTube).", "https://ublockorigin.github.io/uAssets/filters/quick-fixes.min.txt", true},
        {"cookies", "EasyList Cookie Notices", "Hides cookie banners.", "https://secure.fanboy.co.nz/fanboy-cookiemonster.txt", false},
        {"annoyances", "Fanboy Annoyances", "Pop-ups, newsletter prompts, social widgets.", "https://secure.fanboy.co.nz/fanboy-annoyance.txt", false},
    };
    return v;
}

struct FilterManager::Impl {
    FilterManager *self;
    std::string dir, lists_dir;
    Getter get; Setter set;
    WebKitUserContentFilterStore *store = nullptr;
    unsigned generation = 0;
    guint timer = 0;
    bool downloading = false;

    std::string list_path(const std::string &id) const { return lists_dir + "/" + id + ".txt"; }
    void status(const std::string &s) { if (self->on_status) self->on_status(s); }
};

FilterManager::FilterManager(std::string data_dir, Getter get, Setter set) : d_(new Impl) {
    d_->self = this; d_->dir = data_dir; d_->get = std::move(get); d_->set = std::move(set);
    d_->lists_dir = data_dir + "/filter-lists";
    g_mkdir_with_parents(d_->lists_dir.c_str(), 0700);
    std::string store_dir = data_dir + "/content-filters";
    g_mkdir_with_parents(store_dir.c_str(), 0700);
    d_->store = webkit_user_content_filter_store_new(store_dir.c_str());
}

FilterManager::~FilterManager() {
    if (d_->timer) g_source_remove(d_->timer);
    ++d_->generation;
    for (auto *f : filters_) webkit_user_content_filter_unref(f);
    if (d_->store) g_object_unref(d_->store);
    delete d_;
}

bool FilterManager::enabled(const std::string &id) const {
    std::string v = d_->get("filters_" + id);
    if (!v.empty()) return v == "1";
    for (auto &l : catalog()) if (l.id == id) return l.default_on;
    return false;
}
void FilterManager::set_enabled(const std::string &id, bool on) {
    d_->set("filters_" + id, on ? "1" : "0");
    refresh(false);
}
std::string FilterManager::user_filters() const { return d_->get("filters_user"); }
void FilterManager::set_user_filters(const std::string &text) { d_->set("filters_user", text); refresh(false); }

// Sites where the user turned blocking off. The browser detaches the lists from a tab while it shows
// such a site, so toggling is instant (no recompile).
static bool host_matches(const std::string &listed, const std::string &host) {
    return host == listed || (host.size() > listed.size() && host.compare(host.size() - listed.size() - 1, std::string::npos, "." + listed) == 0);
}
bool FilterManager::blocking_off(const std::string &host) const {
    if (host.empty()) return false;
    std::istringstream in(d_->get("filters_off_sites")); std::string line;
    while (std::getline(in, line)) if (!line.empty() && host_matches(abp::trim(line), host)) return true;
    return false;
}
void FilterManager::set_blocking_off(const std::string &host, bool off) {
    if (host.empty()) return;
    std::istringstream in(d_->get("filters_off_sites")); std::string line, out;
    while (std::getline(in, line)) { std::string t = abp::trim(line); if (!t.empty() && t != host) out += t + "\n"; }
    if (off) out += host + "\n";
    d_->set("filters_off_sites", out);
}

// ------------------------------------------------------------------ download
static size_t write_cb(char *p, size_t s, size_t n, void *ud) {
    auto *o = static_cast<std::string *>(ud);
    if (o->size() + s * n > 60u * 1024 * 1024) return 0;  // refuse absurdly large lists
    o->append(p, s * n);
    return s * n;
}

struct DownloadJob { std::vector<std::pair<std::string, std::string>> items; /* id, url */ std::string dir; int ok = 0, failed = 0; };

static void download_thread(GTask *task, gpointer, gpointer data, GCancellable *) {
    auto *job = static_cast<DownloadJob *>(data);
    for (auto &[id, url] : job->items) {
        std::string body;
        CURL *c = curl_easy_init();
        if (!c) { job->failed++; continue; }
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "ReedBrowser filter updater");
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
        CURLcode rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        // sanity check: a filter list is text with many lines and (almost always) a header comment
        bool plausible = rc == CURLE_OK && body.size() > 200 && body.find('\n') != std::string::npos && body.compare(0, 6, "<html>") != 0 && body.compare(0, 9, "<!DOCTYPE") != 0;
        if (!plausible) { job->failed++; g_warning("filter list %s: download failed (%s)", url.c_str(), curl_easy_strerror(rc)); continue; }
        std::string tmp = job->dir + "/" + id + ".txt.part";
        { std::ofstream f(tmp, std::ios::binary); f << body; }
        std::error_code ec; fs::rename(tmp, job->dir + "/" + id + ".txt", ec);
        job->ok += ec ? 0 : 1;
    }
    g_task_return_boolean(task, TRUE);
}

void FilterManager::refresh(bool force) {
    auto *d = d_;
    auto *job = new DownloadJob; job->dir = d->lists_dir;
    for (auto &l : catalog()) {
        if (!enabled(l.id)) continue;
        std::string path = d->list_path(l.id);
        std::error_code ec;
        bool have = fs::exists(path, ec);
        bool stale = !have || force || (fs::file_time_type::clock::now() - fs::last_write_time(path, ec) > std::chrono::hours(96));
        if (stale) job->items.push_back({l.id, l.url});
    }
    auto finish = [](GObject *src, GAsyncResult *res, gpointer ud) {
        auto *self = static_cast<FilterManager *>(g_object_get_data(G_OBJECT(src), "fm"));
        auto *job = static_cast<DownloadJob *>(g_task_get_task_data(G_TASK(res)));
        (void)ud;
        self->d_->downloading = false;
        if (job->ok || job->failed) self->d_->status(job->failed ? "Some filter lists could not be downloaded; using cached copies." : "Filter lists updated.");
        self->rebuild();  // from what is on disk; never loops back into downloading
    };
    if (job->items.empty() || d->downloading) { delete job; rebuild(); return; }
    {
        d->downloading = true;
        d->status("Downloading filter lists…");
        GObject *anchor = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
        g_object_set_data(anchor, "fm", this);
        GTask *t = g_task_new(anchor, nullptr, finish, nullptr);
        g_object_unref(anchor);  // the task keeps it alive until `finish` has run
        g_task_set_task_data(t, job, +[](gpointer p) { delete static_cast<DownloadJob *>(p); });
        g_task_run_in_thread(t, download_thread);
        g_object_unref(t);
        return;
    }
}

// Parse + convert on a worker thread, then compile (or load WebKit's cached copy) on the main loop.
void FilterManager::rebuild() {
    auto *d = d_;
    ++d->generation;
    struct Build { FilterManager *self; unsigned gen; std::string hash; std::vector<std::string> chunks; int cached = 0; std::string summary; };
    auto *bld = new Build{this, d->generation, "", {}, 0, ""};
    std::vector<std::pair<std::string, std::string>> inputs;  // id, path
    for (auto &l : catalog()) if (enabled(l.id) && fs::exists(d->list_path(l.id))) inputs.push_back({l.id, d->list_path(l.id)});
    std::string user = user_filters();
    GTask *t = g_task_new(nullptr, nullptr, +[](GObject *, GAsyncResult *res, gpointer) {
        auto *b = static_cast<Build *>(g_task_get_task_data(G_TASK(res)));
        FilterManager *self = b->self;
        Impl *d = self->d_;
        if (b->gen != d->generation) { return; }  // a newer rebuild superseded this one
        self->summary_ = b->summary;
        // Compile (or load from WebKit's on-disk cache) chunk by chunk, then swap in the result.
        struct Step { FilterManager *self; unsigned gen; std::string hash; std::vector<std::string> chunks; size_t count; size_t i = 0; std::vector<WebKitUserContentFilter *> out; };
        auto *st = new Step{self, b->gen, b->hash, std::move(b->chunks), 0, 0, {}};
        std::string cnt = d->get("filters_count_" + st->hash);
        bool cached = !cnt.empty();
        st->count = cached ? static_cast<size_t>(atoi(cnt.c_str())) : st->chunks.size();
        struct Fns {
            static void done(Step *s) {
                FilterManager *self = s->self; Impl *d = self->d_;
                if (s->gen != d->generation) { for (auto *f : s->out) webkit_user_content_filter_unref(f); delete s; return; }
                std::vector<WebKitUserContentFilter *> old = std::move(self->filters_);
                self->filters_ = std::move(s->out);
                d->set("filters_count_" + s->hash, std::to_string(self->filters_.size()));
                d->status("Blocking active: " + self->summary_);
                if (self->on_changed) self->on_changed();  // tabs swap their old filters (still valid here) for the new ones
                for (auto *f : old) webkit_user_content_filter_unref(f);
                // drop compiled lists from earlier builds
                struct Ctx { FilterManager *self; std::string keep; };
                webkit_user_content_filter_store_fetch_identifiers(d->store, nullptr, +[](GObject *src, GAsyncResult *r, gpointer ud) {
                    auto *c = static_cast<Ctx *>(ud);
                    char **ids = webkit_user_content_filter_store_fetch_identifiers_finish(WEBKIT_USER_CONTENT_FILTER_STORE(src), r);
                    for (char **p = ids; p && *p; ++p)
                        if (strncmp(*p, "reed-abp-", 9) == 0 && std::string(*p).find(c->keep) == std::string::npos)
                            webkit_user_content_filter_store_remove(WEBKIT_USER_CONTENT_FILTER_STORE(src), *p, nullptr, nullptr, nullptr);
                    g_strfreev(ids); delete c;
                }, new Ctx{self, s->hash});
                delete s;
            }
            static void step(Step *s, bool) {
                FilterManager *self = s->self; Impl *d = self->d_;
                if (s->gen != d->generation) { for (auto *f : s->out) webkit_user_content_filter_unref(f); delete s; return; }
                if (s->i >= s->count) { done(s); return; }
                std::string id = "reed-abp-" + s->hash + "-" + std::to_string(s->i);
                if (!s->chunks.empty() && s->i < s->chunks.size()) {
                    GBytes *bytes = g_bytes_new(s->chunks[s->i].data(), s->chunks[s->i].size());
                    webkit_user_content_filter_store_save(d->store, id.c_str(), bytes, nullptr, +[](GObject *a, GAsyncResult *r, gpointer u) { cb_save(a, r, u); }, s);
                    g_bytes_unref(bytes);
                    s->chunks[s->i].clear(); s->chunks[s->i].shrink_to_fit();
                } else {
                    webkit_user_content_filter_store_load(d->store, id.c_str(), nullptr, +[](GObject *a, GAsyncResult *r, gpointer u) { cb_load(a, r, u); }, s);
                }
            }
            static void handle(GObject *src, GAsyncResult *r, gpointer ud, bool load) {
                auto *s = static_cast<Step *>(ud);
                GError *e = nullptr;
                WebKitUserContentFilter *f = load ? webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(src), r, &e)
                                                  : webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(src), r, &e);
                if (f) s->out.push_back(f);
                else g_warning("filter chunk %zu not usable: %s", s->i, e ? e->message : "?");
                if (e) g_error_free(e);
                s->i++;
                step(s, true);
            }
            static void cb_save(GObject *a, GAsyncResult *r, gpointer u) { handle(a, r, u, false); }
            static void cb_load(GObject *a, GAsyncResult *r, gpointer u) { handle(a, r, u, true); }
        };
        Fns::step(st, true);
    }, nullptr);
    g_task_set_task_data(t, bld, +[](gpointer p) { delete static_cast<Build *>(p); });
    // worker: read + hash, then parse/convert only if this exact input is not already compiled
    struct Work { std::vector<std::pair<std::string, std::string>> inputs; std::string user; Getter get; };
    auto *w = new Work{inputs, user, d->get};
    g_object_set_data_full(G_OBJECT(t), "work", w, +[](gpointer p) { delete static_cast<Work *>(p); });
    g_task_run_in_thread(t, +[](GTask *task, gpointer, gpointer data, GCancellable *) {
        auto *b = static_cast<Build *>(data);
        auto *w = static_cast<Work *>(g_object_get_data(G_OBJECT(task), "work"));
        GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA1);
        g_checksum_update(cs, reinterpret_cast<const guchar *>(kFormatVersion), strlen(kFormatVersion));
        std::vector<std::string> texts;
        for (auto &[id, path] : w->inputs) {
            std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf();
            texts.push_back(ss.str());
            g_checksum_update(cs, reinterpret_cast<const guchar *>(id.data()), id.size());
            g_checksum_update(cs, reinterpret_cast<const guchar *>(texts.back().data()), texts.back().size());
        }
        g_checksum_update(cs, reinterpret_cast<const guchar *>(w->user.data()), w->user.size());
        b->hash = g_checksum_get_string(cs);
        g_checksum_free(cs);
        bool cached = !w->get("filters_count_" + b->hash).empty();
        abp::Lists L;
        if (!cached || true) {  // parsing is fast (<1s); needed for the summary even when WebKit has the compiled rules
            for (auto &t : texts) abp::parse_list(t, L);
            abp::parse_list(w->user, L);
        }
        char buf[200];
        snprintf(buf, sizeof buf, "%d network filters, %d exceptions, %d element-hiding rules", L.st.network, L.st.exceptions, L.st.cosmetic);
        b->summary = buf;
        if (!cached) { dnr::Stats ds; b->chunks = abp::to_webkit_chunks(L, 30000, &ds); }
        g_task_return_boolean(task, TRUE);
    });
    g_object_unref(t);
}

void FilterManager::start() {
    // on_status is a public member; Impl::status forwards to it
    refresh(false);
    d_->timer = g_timeout_add_seconds(6 * 3600, +[](gpointer p) -> gboolean { static_cast<FilterManager *>(p)->refresh(false); return G_SOURCE_CONTINUE; }, this);
}
