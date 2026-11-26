// Build: see CMakeLists (test-abp). Usage: test-abp list1.txt [list2.txt ...]
// Parses the given filter lists, converts them, and asks WebKit to compile every chunk.
#undef NDEBUG
#include <webkit/webkit.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "../src/abp.hpp"
static GMainLoop *loop; static bool ok_flag; static std::string err_msg;
static bool compile(WebKitUserContentFilterStore *store, const std::string &json) {
    GBytes *bytes = g_bytes_new(json.data(), json.size());
    ok_flag = false; err_msg.clear();
    webkit_user_content_filter_store_save(store, "t", bytes, nullptr, [](GObject *s, GAsyncResult *r, gpointer) {
        GError *e = nullptr;
        WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(s), r, &e);
        if (e) { err_msg = e->message; g_error_free(e); } else { ok_flag = true; webkit_user_content_filter_unref(f); }
        g_main_loop_quit(loop);
    }, nullptr);
    g_main_loop_run(loop);
    g_bytes_unref(bytes);
    return ok_flag;
}
int main(int argc, char **argv) {
    abp::Lists L;
    for (int i = 1; i < argc; ++i) { std::ifstream f(argv[i]); std::stringstream ss; ss << f.rdbuf(); abp::parse_list(ss.str(), L); }
    std::printf("lines=%d network=%d exceptions=%d cosmetic=%d skipped(option=%d cosmetic=%d other=%d)\n", L.st.lines, L.st.network, L.st.exceptions, L.st.cosmetic, L.st.skipped_option, L.st.skipped_cosmetic, L.st.skipped_other);
    dnr::Stats ds; auto chunks = abp::to_webkit_chunks(L, 40000, &ds);
    std::printf("net rules emitted=%d skipped(regex=%d action=%d other=%d); chunks=%zu\n", ds.emitted, ds.skipped_regex, ds.skipped_unsupported_action, ds.skipped_other, chunks.size());
    char tmpl[] = "/tmp/abptestXXXXXX"; char *dir = mkdtemp(tmpl);
    WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(dir);
    loop = g_main_loop_new(nullptr, FALSE);
    int bad = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        bool ok = compile(store, chunks[i]);
        std::printf("chunk %zu (%zu bytes): %s %s\n", i, chunks[i].size(), ok ? "OK" : "FAILED", err_msg.c_str());
        if (!ok) { ++bad; std::ofstream("/tmp/abp-failed-chunk.json") << chunks[i]; }
    }
    return bad ? 1 : 0;
}
