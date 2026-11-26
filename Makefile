PREFIX := /usr

# Build and install in one go:
#
#   sudo make install
#
# or build as your user first, so cabal does not run as root (it would
# keep its cache in root's home):
#
#   make && sudo make install
#
# build/whiplash only rebuilds when a source file is newer, so the second
# form never calls cabal as root.

WAYLAND_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

PROTOCOL_HEADERS = \
	heart/xdg-shell-protocol.h \
	heart/xdg-decoration-unstable-v1-protocol.h \
	heart/wlr-layer-shell-unstable-v1-protocol.h \
	heart/wlr-output-power-management-unstable-v1-protocol.h

ICON_SIZES = 16 32 48 64 128 192 256 512

SOURCES = whiplash.cabal app/Main.hs heart/heart.c heart/heart.h heart/x11.c heart/x11.h \
	src/Whiplash.hs $(wildcard src/Whiplash/*.hs)

.PHONY: build run install uninstall clean

build: build/whiplash

build/whiplash: $(SOURCES) $(PROTOCOL_HEADERS)
	cabal build --offline exe:whiplash
	install -Dm755 "$$(cabal list-bin --offline exe:whiplash)" $@

heart/xdg-shell-protocol.h:
	wayland-scanner server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

heart/xdg-decoration-unstable-v1-protocol.h:
	wayland-scanner server-header \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

heart/wlr-layer-shell-unstable-v1-protocol.h:
	wayland-scanner server-header protocols/wlr-layer-shell-unstable-v1.xml $@

heart/wlr-output-power-management-unstable-v1-protocol.h:
	wayland-scanner server-header protocols/wlr-output-power-management-unstable-v1.xml $@

# Try it nested in the current Wayland session, without the autostart
# (CMD=foot to open something other than alacritty).
run: build/whiplash
	./build/whiplash -n -s "$(or $(CMD),alacritty)"

# The .desktop session entries launch whiplash-session, a small wrapper
# that gives the session a D-Bus bus and keeps its log in
# $XDG_STATE_HOME/whiplash/whiplash.log. The example config goes to
# share/whiplash so it is on disk without the source checkout.
install: build/whiplash
	install -Dm755 build/whiplash $(DESTDIR)$(PREFIX)/bin/whiplash
	install -Dm755 data/whiplash-session $(DESTDIR)$(PREFIX)/bin/whiplash-session
	install -Dm644 data/whiplash.desktop $(DESTDIR)$(PREFIX)/share/wayland-sessions/whiplash.desktop
	install -Dm644 data/whiplash-x11.desktop $(DESTDIR)$(PREFIX)/share/xsessions/whiplash-x11.desktop
	install -Dm644 data/whiplash-portals.conf $(DESTDIR)/etc/xdg-desktop-portal/whiplash-portals.conf
	install -Dm644 examples/config.toml $(DESTDIR)$(PREFIX)/share/whiplash/config.toml
	for s in $(ICON_SIZES); do \
		install -Dm644 assets/logo/whiplash-$$s.png \
			$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$$s/apps/whiplash.png; \
	done

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/whiplash
	rm -f $(DESTDIR)$(PREFIX)/bin/whiplash-session
	rm -f $(DESTDIR)$(PREFIX)/share/wayland-sessions/whiplash.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/xsessions/whiplash-x11.desktop
	rm -f $(DESTDIR)/etc/xdg-desktop-portal/whiplash-portals.conf
	rm -f $(DESTDIR)$(PREFIX)/share/whiplash/config.toml
	for s in $(ICON_SIZES); do \
		rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$$s/apps/whiplash.png; \
	done

clean:
	cabal clean
	rm -rf build
	rm -f $(PROTOCOL_HEADERS)
