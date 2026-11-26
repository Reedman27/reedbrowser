// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
//
// Adblock Plus / uBlock Origin filter lists (EasyList, EasyPrivacy, uBlock filters, ...) -> WebKit
// content rule lists. Network filters are turned into dnr::Rule and converted by dnr.hpp; cosmetic
// filters (##selector) become css-display-none rules.
//
// Supported: ||, |, ^, *, /regex/ (WebKit subset), @@ exceptions, $third-party/$1p/$3p, resource types
// (and ~negations), $domain=, $important, $match-case, $badfilter, $document/$all, ##generic and
// domain-specific element hiding, #@# exceptions, $elemhide/$generichide.
// Not expressible in WebKit and skipped (counted): $redirect, $removeparam, $csp, $replace, $header,
// $popup, scriptlets (##+js), procedural cosmetics (:has-text, :xpath, ...), HTML filters.
// Pure functions: tested without WebKit by tools/test-abp.cpp.
#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dnr.hpp"

namespace abp {

struct Stats {
    int lines = 0, network = 0, cosmetic = 0, exceptions = 0, skipped_option = 0, skipped_cosmetic = 0, skipped_other = 0;
};

struct Lists {
    std::vector<dnr::Rule> net;
    std::set<std::string> bad;                                   // raw filters disabled by $badfilter
    std::map<std::string, std::set<std::string>> hide_by_domain; // domain -> selectors
    std::set<std::string> hide_generic;
    std::map<std::string, std::set<std::string>> hide_exceptions; // selector -> domains where it is NOT applied ("" = everywhere)
    std::set<std::string> elemhide_off;                          // domains with $elemhide / $generichide exceptions
    Stats st;
};

inline std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == sep) { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur);
    return v;
}
inline bool starts(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

// A cosmetic selector WebKit will accept as a plain CSS selector list. Anything procedural or
// suspicious is rejected: one bad selector would otherwise make the whole list fail to compile.
inline bool selector_ok(const std::string &s) {
    if (s.empty() || s.size() > 600 || !dnr::ascii(s)) return false;
    static const char *bad[] = {":-abp-", ":has-text", ":contains(", ":xpath", ":matches-", ":min-text", ":others", ":upward", ":remove", ":style(",
                                ":watch-attr", ":nth-ancestor", ":-js", "+js(", ":not(:", "{", "}", "\\0", "/*", "<", ";", ":is(:", ":where(:"};
    for (auto *b : bad) if (s.find(b) != std::string::npos) return false;
    if (s[0] == '^' || s[0] == '+' || s[0] == '>' || s[0] == '~' || s[0] == ',') return false;
    int par = 0, br = 0; char q = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (q) { if (c == '\\') ++i; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(') ++par; else if (c == ')') { if (--par < 0) return false; }
        else if (c == '[') ++br; else if (c == ']') { if (--br < 0) return false; }
        else if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    return !q && par == 0 && br == 0;
}

inline const char *abp_type(const std::string &t) {
    if (t == "script") return "script";
    if (t == "image") return "image";
    if (t == "stylesheet" || t == "css") return "stylesheet";
    if (t == "font") return "font";
    if (t == "media") return "media";
    if (t == "xmlhttprequest" || t == "xhr") return "xmlhttprequest";
    if (t == "subdocument" || t == "frame") return "sub_frame";
    if (t == "document" || t == "doc") return "main_frame";
    if (t == "ping") return "ping";
    if (t == "websocket") return "websocket";
    if (t == "object" || t == "object-subrequest") return "object";
    if (t == "other") return "other";
    return nullptr;
}

inline std::string host_of_anchor(const std::string &f) {  // "||example.com^" -> example.com
    if (!starts(f, "||")) return "";
    std::string h;
    for (size_t i = 2; i < f.size(); ++i) {
        char c = f[i];
        if (c == '^' || c == '/' || c == '|') break;
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return "";
        h += c;
    }
    return h;
}

inline void parse_cosmetic(const std::string &line, size_t pos, size_t sep_len, bool exception, Lists &L) {
    std::string domains = line.substr(0, pos), sel = trim(line.substr(pos + sep_len));
    if (!selector_ok(sel)) { L.st.skipped_cosmetic++; return; }
    std::vector<std::string> inc, exc;
    if (!domains.empty())
        for (auto &d : split(domains, ',')) {
            d = dnr::lower(trim(d));
            if (d.empty() || d.find('*') != std::string::npos || d.find('/') != std::string::npos) continue;
            (d[0] == '~' ? exc : inc).push_back(d[0] == '~' ? d.substr(1) : d);
        }
    if (exception) {
        L.st.exceptions++;
        if (inc.empty()) L.hide_exceptions[sel].insert("");
        for (auto &d : inc) L.hide_exceptions[sel].insert(d);
        return;
    }
    L.st.cosmetic++;
    if (inc.empty()) {
        L.hide_generic.insert(sel);
        for (auto &d : exc) L.hide_exceptions[sel].insert(d);
    } else for (auto &d : inc) L.hide_by_domain[d].insert(sel);
}

inline void parse_line(std::string line, Lists &L) {
    line = trim(line);
    if (line.empty() || line[0] == '!' || line[0] == '[' || (line[0] == '#' && !(line.size() > 1 && (line[1] == '#' || line[1] == '@' || line[1] == '?' || line[1] == '$')))) return;
    ++L.st.lines;

    // ---- cosmetic / scriptlet / HTML filters ----
    size_t p;
    if ((p = line.find("#@#")) != std::string::npos && line.find("#@#+js") == std::string::npos) { parse_cosmetic(line, p, 3, true, L); return; }
    if (line.find("#@#") != std::string::npos || line.find("#?#") != std::string::npos || line.find("#$#") != std::string::npos ||
        line.find("#@$#") != std::string::npos || line.find("#@?#") != std::string::npos || line.find("##+js") != std::string::npos ||
        line.find("##^") != std::string::npos || line.find("$$") != std::string::npos) { L.st.skipped_cosmetic++; return; }
    if ((p = line.find("##")) != std::string::npos) {
        bool domain_part_ok = line.find('/') == std::string::npos || line.find('/') > p;  // "/regex/" before ## is a network filter
        if (domain_part_ok && !(line[0] == '/' && p > 0 && line[p - 1] == '/')) { parse_cosmetic(line, p, 2, false, L); return; }
    }

    // ---- network filters ----
    bool exception = false;
    if (starts(line, "@@")) { exception = true; line = line.substr(2); }
    std::string pattern = line, opts;
    bool regex = pattern.size() > 2 && pattern[0] == '/';
    size_t dollar = std::string::npos;
    for (size_t i = line.size(); i-- > 0;) if (line[i] == '$') { dollar = i; break; }
    if (dollar != std::string::npos) {
        std::string tail = line.substr(dollar + 1);
        bool looks_opts = !tail.empty() && tail.find('/') == std::string::npos ? true : tail.find('=') != std::string::npos;
        if (regex && tail.empty()) looks_opts = false;
        if (looks_opts) { pattern = line.substr(0, dollar); opts = tail; }
    }
    if (regex) {
        if (pattern.size() < 3 || pattern.back() != '/') { L.st.skipped_other++; return; }
        pattern = pattern.substr(1, pattern.size() - 2);
    }

    dnr::Rule r;
    r.type = exception ? "allow" : "block";
    r.priority = 1;
    if (regex) r.regex_filter = pattern; else r.url_filter = pattern;
    bool badfilter = false, document = false, elemhide = false, generichide = false;
    std::vector<std::string> inc_dom, exc_dom;
    for (auto &o : split(opts, ',')) {
        std::string opt = dnr::lower(trim(o));
        if (opt.empty()) continue;
        bool neg = opt[0] == '~';
        std::string name = neg ? opt.substr(1) : opt, val;
        if (size_t e = name.find('='); e != std::string::npos) { val = name.substr(e + 1); name = name.substr(0, e); }
        if (name == "third-party" || name == "3p") r.domain_type = neg ? "firstParty" : "thirdParty";
        else if (name == "first-party" || name == "1p") r.domain_type = neg ? "thirdParty" : "firstParty";
        else if (name == "important") r.priority = 2;
        else if (name == "match-case") r.case_sensitive = true;
        else if (name == "badfilter") badfilter = true;
        else if (name == "all") { for (auto *t : {"main_frame", "sub_frame", "script", "image", "stylesheet", "font", "media", "xmlhttprequest", "ping", "websocket", "object", "other"}) r.res_types.push_back(t); }
        else if (name == "elemhide" || name == "ehide") elemhide = true;
        else if (name == "generichide" || name == "ghide") generichide = true;
        else if (name == "domain" || name == "from") {
            for (auto &d : split(val, '|')) { if (d.empty()) continue; if (d[0] == '~') exc_dom.push_back(d.substr(1)); else inc_dom.push_back(d); }
        } else if (const char *t = abp_type(name)) {
            if (std::string(t) == "main_frame" && !neg) document = true;
            (neg ? r.excl_res_types : r.res_types).push_back(t);
        } else { L.st.skipped_option++; return; }  // redirect, removeparam, csp, popup, header, denyallow, ...
    }
    if (!inc_dom.empty() && !exc_dom.empty()) { L.st.skipped_option++; return; }  // WebKit cannot express both
    r.initiator = inc_dom; r.excl_initiator = exc_dom;

    if (badfilter) { L.bad.insert(dnr::lower(line.substr(0, line.find("$badfilter") == std::string::npos ? line.size() : line.find("$badfilter")))); return; }

    if (exception && (elemhide || generichide)) {  // @@||site.com^$elemhide : stop cosmetic filtering there
        std::string h = host_of_anchor(pattern);
        if (!h.empty()) L.elemhide_off.insert(dnr::lower(h));
        return;
    }
    if (exception && document) {  // @@||site.com^$document : disable blocking on that site
        std::string h = host_of_anchor(pattern);
        if (h.empty()) { L.st.skipped_option++; return; }
        r.type = "allowAllRequests"; r.url_filter.clear(); r.req_domains = {h}; r.res_types = {"main_frame"};
        L.st.exceptions++; L.net.push_back(std::move(r)); return;
    }
    if (!exception && document && r.res_types.size() == 1) r.res_types = {"main_frame"};
    if (r.url_filter.empty() && r.regex_filter.empty() && r.initiator.empty() && r.domain_type.empty() && r.res_types.empty()) { L.st.skipped_other++; return; }
    if (exception) L.st.exceptions++; else L.st.network++;
    L.net.push_back(std::move(r));
}

inline void parse_list(const std::string &text, Lists &L) {
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        parse_line(text.substr(i, e - i), L);
        i = e + 1;
    }
}

// JSON rule arrays for WebKit, each with at most `max_rules` entries (WebKit allows 150000 per list;
// smaller chunks compile faster and a failure only loses one chunk).
inline std::vector<std::string> to_webkit_chunks(Lists &L, size_t max_rules = 40000, dnr::Stats *dst = nullptr) {
    // drop $badfilter'ed rules
    if (!L.bad.empty()) {
        std::vector<dnr::Rule> keep;
        for (auto &r : L.net) {
            std::string raw = dnr::lower(r.regex_filter.empty() ? r.url_filter : "/" + r.regex_filter + "/");
            if (!L.bad.count(raw)) keep.push_back(std::move(r));
        }
        L.net.swap(keep);
    }
    // network rules via the DNR converter (it sorts blocks before allows)
    std::vector<std::string> rules;
    {
        dnr::Stats st;
        std::string j = dnr::to_webkit(L.net, &st);
        if (dst) *dst = st;
        // split the top-level array into rule objects
        int depth = 0; size_t start = 0; bool instr = false;
        for (size_t i = 0; i < j.size(); ++i) {
            char c = j[i];
            if (instr) { if (c == '\\') ++i; else if (c == '"') instr = false; continue; }
            if (c == '"') instr = true;
            else if (c == '{') { if (depth++ == 0) start = i; }
            else if (c == '}') { if (--depth == 0) rules.push_back(j.substr(start, i - start + 1)); }
        }
    }
    size_t net_count = rules.size();
    // cosmetic rules go last so that network exceptions order is unaffected
    auto exc_for = [&](const std::string &sel) {
        std::vector<std::string> d;
        auto it = L.hide_exceptions.find(sel);
        if (it != L.hide_exceptions.end()) for (auto &x : it->second) d.push_back(x);
        return d;
    };
    auto domain_list = [](const std::vector<std::string> &v) { std::vector<std::string> o; for (auto &x : v) o.push_back(dnr::domain_entry(x)); return dnr::json_str_array(o); };
    auto emit = [&](const std::string &sels, const std::string &domain_clause) {
        rules.push_back("{\"trigger\":{\"url-filter\":\".*\"" + domain_clause + "},\"action\":{\"type\":\"css-display-none\",\"selector\":" + dnr::esc(sels) + "}}");
    };
    // generic selectors: grouped by their exception set, 80 per rule
    std::map<std::string, std::vector<std::string>> groups;  // key = joined exception domains
    for (auto &sel : L.hide_generic) {
        auto ex = exc_for(sel);
        if (std::find(ex.begin(), ex.end(), "") != ex.end()) continue;  // exempt everywhere
        std::sort(ex.begin(), ex.end());
        std::string key; for (auto &x : ex) key += x + ",";
        groups[key].push_back(sel);
    }
    std::vector<std::string> off(L.elemhide_off.begin(), L.elemhide_off.end());
    for (auto &[key, sels] : groups) {
        std::vector<std::string> unless = split(key, ','); unless.pop_back();
        unless.insert(unless.end(), off.begin(), off.end());
        std::string clause = unless.empty() ? "" : ",\"unless-domain\":" + domain_list(unless);
        for (size_t i = 0; i < sels.size(); i += 80) {
            std::string joined;
            for (size_t k = i; k < std::min(sels.size(), i + 80); ++k) joined += (k > i ? "," : "") + sels[k];
            emit(joined, clause);
        }
    }
    for (auto &[dom, sels] : L.hide_by_domain) {
        if (L.elemhide_off.count(dom)) continue;
        std::vector<std::string> ok;
        for (auto &s : sels) {
            auto ex = exc_for(s);
            if (std::find(ex.begin(), ex.end(), "") != ex.end() || std::find(ex.begin(), ex.end(), dom) != ex.end()) continue;
            ok.push_back(s);
        }
        for (size_t i = 0; i < ok.size(); i += 80) {
            std::string joined;
            for (size_t k = i; k < std::min(ok.size(), i + 80); ++k) joined += (k > i ? "," : "") + ok[k];
            emit(joined, ",\"if-domain\":" + domain_list({dom}));
        }
    }
    // Network rules stay in ONE list (up to WebKit's 150000): "ignore-previous-rules" exceptions only
    // work inside the list that holds the rules they cancel. Cosmetic rules are chunked separately.
    std::vector<std::string> chunks;
    auto pack = [&](size_t from, size_t to, size_t step) {
        for (size_t i = from; i < to; i += step) {
            std::string c = "[";
            for (size_t k = i; k < std::min(to, i + step); ++k) c += (k > i ? "," : "") + rules[k];
            chunks.push_back(c + "]");
        }
    };
    pack(0, net_count, 140000);
    pack(net_count, rules.size(), max_rules);
    return chunks;
}

}  // namespace abp
