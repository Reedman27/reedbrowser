// Build: g++ -std=c++17 -Isrc tools/test-dnr.cpp $(pkg-config --cflags --libs json-glib-1.0 webkitgtk-6.0) -o /tmp/test-dnr
// Converts sample DNR rules and asks WebKit to compile the result (proves the syntax is accepted).
#undef NDEBUG  // the checks below have side effects and must run in Release builds
#include <webkit/webkit.h>
#include <cassert>
#include <cstdio>
#include "../src/dnr.hpp"
static const char *rules = R"J([
 {"id":1,"priority":1,"action":{"type":"block"},"condition":{"urlFilter":"||ads.example.com^","resourceTypes":["script","image"]}},
 {"id":2,"priority":1,"action":{"type":"block"},"condition":{"urlFilter":"/banner/*/ad.gif"}},
 {"id":3,"priority":2,"action":{"type":"allow"},"condition":{"urlFilter":"||ads.example.com/ok^","initiatorDomains":["Example.org"]}},
 {"id":4,"priority":1,"action":{"type":"block"},"condition":{"requestDomains":["tracker.net","metrics.io"],"domainType":"thirdParty"}},
 {"id":5,"priority":1,"action":{"type":"block"},"condition":{"regexFilter":"^https://[a-z]+\\.cdn\\.com/ad[0-9]+\\.js","isUrlFilterCaseSensitive":true}},
 {"id":6,"priority":1,"action":{"type":"block"},"condition":{"regexFilter":"(a|b)\\.js"}},
 {"id":7,"priority":1,"action":{"type":"redirect","redirect":{"url":"https://x/"}},"condition":{"urlFilter":"x"}},
 {"id":8,"priority":1,"action":{"type":"block"},"condition":{"urlFilter":"||frames.example.net^","resourceTypes":["sub_frame"]}},
 {"id":9,"priority":3,"action":{"type":"allowAllRequests"},"condition":{"requestDomains":["trusted.com"],"resourceTypes":["main_frame"]}},
 {"id":10,"priority":1,"action":{"type":"upgradeScheme"},"condition":{"urlFilter":"|http://*","excludedInitiatorDomains":["localhost"]}},
 {"id":11,"priority":1,"action":{"type":"block"},"condition":{"urlFilter":"pixel.gif|","excludedResourceTypes":["image"]}}
])J";
static GMainLoop *loop; static bool compiled = false;
int main(int argc, char **argv) {
    JsonParser *p = json_parser_new();
    assert(json_parser_load_from_data(p, rules, -1, nullptr));
    auto rs = dnr::parse_rules(json_parser_get_root(p));
    assert(rs.size() == 11);
    dnr::Stats st; std::string j = dnr::to_webkit(rs, &st);
    std::printf("emitted=%d redirect_skipped=%d regex_skipped=%d other_skipped=%d\n", st.emitted, st.skipped_unsupported_action, st.skipped_regex, st.skipped_other);
    std::printf("%s\n", j.c_str());
    assert(st.skipped_unsupported_action == 1 && st.skipped_regex == 1);
    // ordering: priority 3 (allowAllRequests) is last, allow(2) after the priority-1 blocks
    assert(j.rfind("ignore-previous-rules") > j.find("\"block\""));
    char tmpl[] = "/tmp/dnrtestXXXXXX"; char *dir = mkdtemp(tmpl);
    WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(dir);
    loop = g_main_loop_new(nullptr, FALSE);
    GBytes *bytes = g_bytes_new(j.data(), j.size());
    webkit_user_content_filter_store_save(store, "t", bytes, nullptr, [](GObject *s, GAsyncResult *r, gpointer) {
        GError *e = nullptr;
        WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(s), r, &e);
        if (e) std::printf("WEBKIT COMPILE FAILED: %s\n", e->message); else { std::puts("WEBKIT COMPILE OK"); compiled = true; }
        (void)f; g_main_loop_quit(loop);
    }, nullptr);
    g_timeout_add_seconds(60, [](gpointer) -> gboolean { std::puts("TIMEOUT"); g_main_loop_quit(loop); return G_SOURCE_REMOVE; }, nullptr);
    g_main_loop_run(loop);
    return compiled ? 0 : 1;
}
