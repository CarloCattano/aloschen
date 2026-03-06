#!/usr/bin/make -f
# Makefile for aloschen.lv2 #
# ----------------------- #
#

include Makefile.mk

# --------------------------------------------------------------

PREFIX  ?= /usr/local
DESTDIR ?=

# --------------------------------------------------------------
# Default target is to build all plugins

all: build

# --------------------------------------------------------------
# aloschen build rules

build: aloschen.lv2/aloschen$(LIB_EXT) aloschen.lv2/aloschen_ui$(LIB_EXT) aloschen.lv2/manifest.ttl

aloschen.lv2/aloschen$(LIB_EXT): aloschen.c loop_engine.c
	$(CC) $^ $(BUILD_C_FLAGS) $(LINK_FLAGS) -lm $(SHARED) -o $@ -Wall -Wextra

aloschen.lv2/aloschen_ui$(LIB_EXT): aloschen_ui.c
	$(CC) $^ $(BUILD_C_FLAGS) $(LINK_FLAGS) -lX11 -lm $(SHARED) -o $@ -Wall -Wextra

aloschen.lv2/manifest.ttl: aloschen.lv2/manifest.ttl.in
	sed -e "s|@LIB_EXT@|$(LIB_EXT)|" $< > $@

# --------------------------------------------------------------

clean:
	rm -f aloschen.lv2/aloschen$(LIB_EXT) aloschen.lv2/aloschen_ui$(LIB_EXT) aloschen.lv2/manifest.ttl

# --------------------------------------------------------------

install: build
	install -d $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2
	install -d $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/modgui

	install -m 644 aloschen.lv2/*.so  $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/
	install -m 644 aloschen.lv2/*.ttl $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/
	install -m 644 aloschen.lv2/modgui/* $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/modgui

# --------------------------------------------------------------
# Local dev convenience: deploy to MOD Desktop

MOD_DESKTOP_PLUGINS ?= $(HOME)/Desktop/mod-desktop-0.0.12-linux-x86_64/mod-desktop/plugins

.PHONY: deploy-mod-desktop
deploy-mod-desktop: build
	@echo "Deploying aloschen.lv2 -> $(MOD_DESKTOP_PLUGINS)"
	rm -rf "$(MOD_DESKTOP_PLUGINS)/aloschen.lv2"
	cp -r aloschen.lv2 "$(MOD_DESKTOP_PLUGINS)"

# --------------------------------------------------------------
