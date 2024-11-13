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

build: aloschen.lv2/aloschen$(LIB_EXT) aloschen.lv2/manifest.ttl

aloschen.lv2/aloschen$(LIB_EXT): aloschen.c
	$(CXX) $^ $(BUILD_CXX_FLAGS) $(LINK_FLAGS) -lm $(SHARED) -o $@

aloschen.lv2/manifest.ttl: aloschen.lv2/manifest.ttl.in
	sed -e "s|@LIB_EXT@|$(LIB_EXT)|" $< > $@

# --------------------------------------------------------------

clean:
	rm -f aloschen.lv2/aloschen$(LIB_EXT) aloschen.lv2/manifest.ttl

# --------------------------------------------------------------

install: build
	install -d $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2
	install -d $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/modgui

	install -m 644 aloschen.lv2/*.so  $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/
	install -m 644 aloschen.lv2/*.ttl $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/
	install -m 644 aloschen.lv2/modgui/* $(DESTDIR)$(PREFIX)/lib/lv2/aloschen.lv2/modgui

# --------------------------------------------------------------
