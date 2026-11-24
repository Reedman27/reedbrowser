Name:           reedbrowser
Version:        0.5.2
Release:        1%{?dist}
Summary:        A fast, quiet WebKitGTK browser
License:        AGPL-3.0-only
URL:            https://github.com/Reedman27/ReedBrowser
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.20
BuildRequires:  pkgconf-pkg-config
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(webkitgtk-6.0)
BuildRequires:  sqlite-devel
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(libarchive)
BuildRequires:  pkgconfig(libcurl)
Requires:       webkitgtk6.0

%description
ReedBrowser is a lightweight desktop web browser using GTK4 and WebKitGTK.
It has a local-only new-tab page, tabs, bookmarks, history, downloads and
private tabs. WebKit handles rendering, GPU compositing and process isolation.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md
%{_bindir}/reedbrowser

%changelog
* Mon Nov 23 2026 ReedBrowser contributors - 0.5.2-1
- Add HTTPS installation from official Firefox Add-ons listing URLs
- Resolve current XPI through the AMO API and reuse permission confirmation

* Mon Nov 23 2026 ReedBrowser contributors - 0.5.1-1
- First compiled and runtime-tested build (GTK 4.14 / WebKitGTK 2.52, Xvfb)
- Fix find-in-page: search as you type, Enter/Ctrl+G next, Ctrl+Shift+G previous,
  match count, highlights cleared on Esc
- Show an empty address bar on the local new-tab page instead of about:blank

* Sun Nov 22 2026 ReedBrowser contributors - 0.5.0-1
- WebExtensions runtime preview (content scripts, background pages, storage, tabs)
- Rebuild browser shell on native GTK4 and WebKitGTK 6.0
