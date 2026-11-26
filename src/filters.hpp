// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
//
// Native ad / tracker blocking: downloads EasyList-style filter lists (EasyList, EasyPrivacy, uBlock
// filters, ...), converts them (abp.hpp) and compiles them into WebKit content rule lists.
#pragma once
#include <webkit/webkit.h>

#include <functional>
#include <string>
#include <vector>

struct FilterList {
    std::string id, name, description, url;
    bool default_on;
};

class FilterManager {
public:
    using Getter = std::function<std::string(const std::string &)>;
    using Setter = std::function<void(const std::string &, const std::string &)>;
    FilterManager(std::string data_dir, Getter get, Setter set);
    ~FilterManager();

    static const std::vector<FilterList> &catalog();
    bool enabled(const std::string &id) const;
    void set_enabled(const std::string &id, bool on);
    std::string user_filters() const;
    void set_user_filters(const std::string &text);
    bool blocking_off(const std::string &host) const;
    void set_blocking_off(const std::string &host, bool off);

    void start();                  // load what is cached, refresh stale lists in the background
    void refresh(bool force);      // download lists (force = ignore the cache age) and rebuild
    const std::vector<WebKitUserContentFilter *> &filters() const { return filters_; }
    std::string summary() const { return summary_; }

    std::function<void()> on_changed;                    // filters() changed: re-attach to tabs
    std::function<void(const std::string &)> on_status;  // human-readable progress

private:
    void rebuild();
    struct Impl;
    Impl *d_;
    std::vector<WebKitUserContentFilter *> filters_;
    std::string summary_;
    friend struct Impl;
};
