// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ReedBrowser contributors
//
// declarativeNetRequest (Chrome/Firefox MV3 content blocking) -> WebKit content rule lists.
// Pure functions on json-glib nodes so they can be tested without WebKit (tools/test-dnr.cpp).
//
// Supported: block, allow, allowAllRequests (as "ignore everything on these sites"), upgradeScheme.
// urlFilter (||, |, *, ^), regexFilter (WebKit regex subset only), requestDomains, initiatorDomains,
// excluded*Domains, resourceTypes / excludedResourceTypes, domainType, case sensitivity, priorities.
// Not expressible in WebKit and skipped (counted in Stats): redirect, modifyHeaders, regex with
// alternation / counted repeats, requestMethods, tabIds.
#pragma once

#include <json-glib/json-glib.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace dnr {

struct Stats {
    int input = 0, emitted = 0, skipped_unsupported_action = 0, skipped_regex = 0, skipped_other = 0;
};

struct Rule {
    long long id = 0;
    int priority = 1;
    std::string type;  // block | allow | allowAllRequests | upgradeScheme | redirect | modifyHeaders
    std::string url_filter, regex_filter;
    bool case_sensitive = false;
    std::vector<std::string> initiator, excl_initiator, req_domains, excl_req_domains, res_types, excl_res_types;
    std::string domain_type;  // firstParty | thirdParty | ""
    bool has_methods = false, has_tabs = false;
};

inline std::string esc(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

inline std::vector<std::string> str_list(JsonObject *o, const char *k) {
    std::vector<std::string> r;
    if (!o || !json_object_has_member(o, k)) return r;
    JsonNode *n = json_object_get_member(o, k);
    if (!n || !JSON_NODE_HOLDS_ARRAY(n)) return r;
    JsonArray *a = json_node_get_array(n);
    for (guint i = 0; i < json_array_get_length(a); ++i) {
        JsonNode *e = json_array_get_element(a, i);
        if (e && JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING) r.push_back(json_node_get_string(e));
    }
    return r;
}
inline std::string str_of(JsonObject *o, const char *k) {
    if (!o || !json_object_has_member(o, k)) return "";
    JsonNode *n = json_object_get_member(o, k);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return "";
    return json_node_get_string(n);
}

inline bool parse_rule(JsonNode *node, Rule &r) {
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return false;
    JsonObject *o = json_node_get_object(node);
    if (json_object_has_member(o, "id")) r.id = json_object_get_int_member(o, "id");
    if (json_object_has_member(o, "priority")) r.priority = static_cast<int>(json_object_get_int_member(o, "priority"));
    JsonObject *a = json_object_has_member(o, "action") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "action")) ? json_object_get_object_member(o, "action") : nullptr;
    JsonObject *c = json_object_has_member(o, "condition") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "condition")) ? json_object_get_object_member(o, "condition") : nullptr;
    if (!a || !c) return false;
    r.type = str_of(a, "type");
    r.url_filter = str_of(c, "urlFilter");
    r.regex_filter = str_of(c, "regexFilter");
    if (json_object_has_member(c, "isUrlFilterCaseSensitive")) r.case_sensitive = json_object_get_boolean_member(c, "isUrlFilterCaseSensitive");
    // "domains"/"excludedDomains" are the deprecated names of the initiator lists.
    r.initiator = str_list(c, "initiatorDomains"); if (r.initiator.empty()) r.initiator = str_list(c, "domains");
    r.excl_initiator = str_list(c, "excludedInitiatorDomains"); if (r.excl_initiator.empty()) r.excl_initiator = str_list(c, "excludedDomains");
    r.req_domains = str_list(c, "requestDomains");
    r.excl_req_domains = str_list(c, "excludedRequestDomains");
    r.res_types = str_list(c, "resourceTypes");
    r.excl_res_types = str_list(c, "excludedResourceTypes");
    r.domain_type = str_of(c, "domainType");
    r.has_methods = json_object_has_member(c, "requestMethods") || json_object_has_member(c, "excludedRequestMethods");
    r.has_tabs = json_object_has_member(c, "tabIds") || json_object_has_member(c, "excludedTabIds");
    return true;
}

inline bool ascii(const std::string &s) { return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x80; }); }
inline std::string lower(std::string s) { for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
inline std::string regex_escape_char(char c) {
    static const std::string special = ".+?()[]{}\\|$^*";
    return special.find(c) != std::string::npos ? std::string("\\") + c : std::string(1, c);
}

// Chrome urlFilter syntax -> WebKit regex subset. Returns false when it cannot be expressed.
inline bool url_filter_to_regex(const std::string &f, std::string &out) {
    if (!ascii(f)) return false;
    size_t i = 0, n = f.size();
    std::string re;
    if (f.compare(0, 2, "||") == 0) { re = "^[a-zA-Z][a-zA-Z0-9+.-]*://([^/]+\\.)?"; i = 2; }
    else if (n && f[0] == '|') { re = "^"; i = 1; }
    bool end_anchor = false;
    if (n > i && f[n - 1] == '|') { end_anchor = true; --n; }
    for (; i < n; ++i) {
        char c = f[i];
        if (c == '*') { re += ".*"; }
        else if (c == '^') {
            bool last = (i + 1 == n);
            // A separator at the end of a host-anchored filter nearly always follows a hostname.
            re += last ? "[/:?#&=]" : "[^a-zA-Z0-9_.%-]";
        } else re += regex_escape_char(c);
    }
    if (end_anchor) re += "$";
    if (re.empty()) re = ".*";
    out = re;
    return true;
}

// WebKit's regex subset: no alternation, no {n,m}, no lookaround, no \b etc.
inline bool webkit_regex_ok(const std::string &r) {
    if (!ascii(r) || r.empty()) return false;
    for (size_t i = 0; i < r.size(); ++i) {
        char c = r[i];
        if (c == '\\') {
            if (i + 1 >= r.size()) return false;
            char d = r[++i];
            if (std::isalpha(static_cast<unsigned char>(d)) && d != 'd' && d != 'w' && d != 's') return false;  // \b \B \p \1 ...
            if (d == 'd' || d == 'w' || d == 's') return false;  // be conservative: expanded classes are not guaranteed
        } else if (c == '|' || c == '{' || c == '}') return false;
        else if (c == '(' && i + 1 < r.size() && r[i + 1] == '?') return false;
    }
    return true;
}

inline const char *map_type(const std::string &t) {
    if (t == "main_frame" || t == "sub_frame") return "document";
    if (t == "stylesheet") return "style-sheet";
    if (t == "script") return "script";
    if (t == "image") return "image";
    if (t == "font") return "font";
    if (t == "xmlhttprequest") return "raw";
    if (t == "ping") return "ping";
    if (t == "media") return "media";
    if (t == "websocket") return "raw";
    if (t == "object" || t == "other" || t == "csp_report") return "raw";
    return nullptr;
}

inline std::string domain_entry(const std::string &d) {
    std::string x = lower(d);
    if (!x.empty() && x.compare(0, 2, "*.") == 0) x = x.substr(1);
    if (!x.empty() && x[0] == '.') x = x.substr(1);
    return "*" + x;  // WebKit: leading * also matches subdomains
}

inline std::string json_str_array(const std::vector<std::string> &v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += esc(v[i]); }
    return o + "]";
}

// Convert one rule. Appends zero or more WebKit rule objects (as JSON text) to `out`.
inline void convert_rule(const Rule &r, std::vector<std::string> &out, Stats &st) {
    std::string action;
    if (r.type == "block") action = "{\"type\":\"block\"}";
    else if (r.type == "allow" || r.type == "allowAllRequests") action = "{\"type\":\"ignore-previous-rules\"}";
    else if (r.type == "upgradeScheme") action = "{\"type\":\"make-https\"}";
    else { st.skipped_unsupported_action++; return; }
    if (r.has_methods || r.has_tabs) { st.skipped_other++; return; }

    // resource types
    std::vector<std::string> types;
    std::string load_ctx;  // WebKit "document" covers both top frames and iframes; load-context separates them
    bool restrict_types = false;
    std::set<std::string> seen;
    if (r.type == "allowAllRequests") { /* applies to whole pages on the given sites */ }
    else if (!r.res_types.empty()) {
        restrict_types = true;
        for (auto &t : r.res_types) if (const char *m = map_type(t)) if (seen.insert(m).second) types.push_back(m);
        if (types.empty()) { st.skipped_other++; return; }
        bool has_main = std::find(r.res_types.begin(), r.res_types.end(), "main_frame") != r.res_types.end();
        bool has_sub = std::find(r.res_types.begin(), r.res_types.end(), "sub_frame") != r.res_types.end();
        if (has_main && !has_sub) load_ctx = ",\"load-context\":[\"top-frame\"]";
        else if (has_sub && !has_main) load_ctx = ",\"load-context\":[\"child-frame\"]";
    } else {
        // DNR default: everything except main_frame. Exclusions narrow it further.
        std::set<std::string> excl;
        for (auto &t : r.excl_res_types) if (const char *m = map_type(t)) excl.insert(m);
        restrict_types = true;
        for (const char *m : {"image", "style-sheet", "script", "font", "raw", "media", "ping"})
            if (!excl.count(m)) types.push_back(m);
        // DNR's default includes sub_frame (iframes) but not main_frame.
        bool sub_ex = std::find(r.excl_res_types.begin(), r.excl_res_types.end(), "sub_frame") != r.excl_res_types.end();
        if (!sub_ex) { types.push_back("document"); load_ctx = ",\"load-context\":[\"child-frame\"]"; }
    }

    std::string load_type;
    if (r.domain_type == "thirdParty") load_type = ",\"load-type\":[\"third-party\"]";
    else if (r.domain_type == "firstParty") load_type = ",\"load-type\":[\"first-party\"]";

    std::string domains;
    if (!r.initiator.empty()) {
        std::vector<std::string> d; for (auto &x : r.initiator) d.push_back(domain_entry(x));
        domains = ",\"if-domain\":" + json_str_array(d);
    } else if (!r.excl_initiator.empty()) {
        std::vector<std::string> d; for (auto &x : r.excl_initiator) d.push_back(domain_entry(x));
        domains = ",\"unless-domain\":" + json_str_array(d);
    }

    // url-filter candidates
    std::vector<std::string> filters;
    if (!r.regex_filter.empty()) {
        if (!webkit_regex_ok(r.regex_filter)) { st.skipped_regex++; return; }
        filters.push_back(r.regex_filter);
    } else if (!r.url_filter.empty()) {
        std::string re;
        if (!url_filter_to_regex(r.url_filter, re)) { st.skipped_other++; return; }
        filters.push_back(re);
    } else if (!r.req_domains.empty()) {
        for (auto &d : r.req_domains) {
            if (!ascii(d) || d.empty()) continue;
            std::string re = "^[a-zA-Z][a-zA-Z0-9+.-]*://([^/]+\\.)?";
            for (char ch : lower(d)) re += regex_escape_char(ch);
            re += "[/:?#]";
            filters.push_back(re);
        }
        if (r.type == "allowAllRequests" && r.initiator.empty()) {
            // "allow everything on these sites": use the page domain instead of the request domain
            std::vector<std::string> d; for (auto &x : r.req_domains) d.push_back(domain_entry(x));
            filters.assign(1, ".*");
            domains = ",\"if-domain\":" + json_str_array(d);
        }
    } else filters.push_back(".*");
    if (filters.empty()) { st.skipped_other++; return; }
    // excluded request domains: cannot be expressed together with a positive filter; ignore the exclusion
    // for allow rules (safe: allows less) and skip block rules (safe: blocks less).
    if (!r.excl_req_domains.empty() && r.type == "block") { st.skipped_other++; return; }

    std::string common = std::string(",\"url-filter-is-case-sensitive\":") + (r.case_sensitive ? "true" : "false");
    if (restrict_types) common += ",\"resource-type\":" + json_str_array(types);
    common += load_type + domains;
    if (std::find(types.begin(), types.end(), "document") != types.end()) common += load_ctx;
    for (auto &f : filters) {
        out.push_back("{\"trigger\":{\"url-filter\":" + esc(f) + common + "},\"action\":" + action + "}");
        st.emitted++;
    }
}

// Rules from several lists (static rulesets, dynamic, session). Output: JSON array for WebKit,
// ordered so DNR precedence holds: higher priority later; at equal priority allow comes after block.
inline std::string to_webkit(const std::vector<Rule> &rules_in, Stats *stats_out = nullptr) {
    Stats st;
    st.input = static_cast<int>(rules_in.size());
    std::vector<const Rule *> order;
    for (auto &r : rules_in) order.push_back(&r);
    auto rank = [](const Rule *r) { return r->type == "allow" || r->type == "allowAllRequests" ? 2 : r->type == "upgradeScheme" ? 1 : 0; };
    std::stable_sort(order.begin(), order.end(), [&](const Rule *a, const Rule *b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        return rank(a) < rank(b);
    });
    std::vector<std::string> out;
    for (auto *r : order) convert_rule(*r, out, st);
    std::string json = "[";
    for (size_t i = 0; i < out.size(); ++i) { if (i) json += ","; json += out[i]; }
    json += "]";
    if (stats_out) *stats_out = st;
    return json;
}

inline std::vector<Rule> parse_rules(JsonNode *array_node) {
    std::vector<Rule> v;
    if (!array_node || !JSON_NODE_HOLDS_ARRAY(array_node)) return v;
    JsonArray *a = json_node_get_array(array_node);
    for (guint i = 0; i < json_array_get_length(a); ++i) {
        Rule r;
        if (parse_rule(json_array_get_element(a, i), r)) v.push_back(std::move(r));
    }
    return v;
}

}  // namespace dnr
