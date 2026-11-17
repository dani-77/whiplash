PREFIX ?= /usr/local
WAYLAND_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

all: heart/xdg-shell-protocol.h heart/xdg-decoration-unstable-v1-protocol.h
	cabal build --offline exe:whiplash

heart/xdg-shell-protocol.h:
	wayland-scanner server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

heart/xdg-decoration-unstable-v1-protocol.h:
	wayland-scanner server-header \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@

# Run nested inside the current Wayland session.
run: all
	$$(cabal list-bin --offline whiplash) -s "$(or $(CMD),alacritty)"

install: all
	install -Dm755 $$(cabal list-bin --offline whiplash) $(DESTDIR)$(PREFIX)/bin/whiplash

clean:
	cabal clean
	rm -f heart/*-protocol.h

.PHONY: all run install clean
