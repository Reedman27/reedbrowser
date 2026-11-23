# Fedora 45 target

Required development packages:

```bash
sudo dnf install gcc-c++ cmake make pkgconf-pkg-config gtk4-devel webkitgtk6.0-devel sqlite-devel
```

The relevant pkg-config modules are:

- `gtk4`
- `webkitgtk-6.0`
- `sqlite3` is found through CMake's `FindSQLite3`

The executable uses GTK4 and WebKitGTK 6.0 directly. It does not use FLTK, GTK3, `webkit2gtk-4.1`, X11 window reparenting, or software framebuffer transport.
