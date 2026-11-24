// Build: g++ -std=c++17 -Isrc tools/test-importer.cpp $(pkg-config --cflags --libs json-glib-1.0 sqlite3) -o /tmp/test-importer
#include "../src/importer.hpp"
#include <cassert>
#include <cstdio>
static void dump(const char *n, const std::vector<Bookmark> &v) { std::printf("%s: %zu\n", n, v.size()); for (auto &b : v) std::printf("  [%s] %s -> %s\n", b.folder.c_str(), b.title.c_str(), b.uri.c_str()); }
int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::string err;
    auto c = importer::parse_chromium(dir + "/Bookmarks", &err); dump("chromium", c);
    assert(c.size() == 3 && c[0].folder == "Bookmarks bar" && c[1].folder == "Bookmarks bar / Dev" && c[2].folder == "Other bookmarks");
    auto h = importer::parse_html_file(dir + "/bookmarks.html", &err); dump("html", h);
    assert(h.size() == 3 && h[1].folder == "Toolbar / News & Stuff" && h[1].title == "A & B" && h[2].folder == "Toolbar");
    auto f = importer::parse_firefox(dir + "/places.sqlite", &err); dump("firefox", f);
    assert(f.size() == 2); assert((f[0].folder == "Bookmarks Toolbar / Work") != (f[1].folder == "Bookmarks Toolbar / Work")); assert(f[0].folder == "Other Bookmarks" || f[1].folder == "Other Bookmarks");
    Store s; assert(s.open(":memory:"));
    assert(s.import_bookmarks(c) == 3); assert(s.import_bookmarks(c) == 0);
    Bookmark b; auto all = s.bookmarks_full(); assert(s.get_bookmark(all[0].id, &b));
    assert(s.update_bookmark(b.id, "Renamed", b.uri, "X")); s.get_bookmark(b.id, &b); assert(b.title == "Renamed" && b.folder == "X");
    s.delete_bookmark(b.id); assert(s.count_bookmarks() == 2);
    std::puts("ALL OK");
}
