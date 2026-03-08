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
# unit test harness

TEST_BIN := tests/run_transport_tests tests/run_dsp_tests tests/run_engine_tests
TEST_SRCS_TRANSPORT := tests/transport_test.c
TEST_SRCS_DSP := tests/dsp_test.c

.PHONY: tests check
# build separate test executables for transport and DSP helpers
tests: $(TEST_BIN)
	@echo "Tests are up-to-date (binaries built). To force rebuild, use 'make -B tests' or 'make clean && make tests'."
	@echo "Running transport tests..."
	@./tests/run_transport_tests; rc1=$$?; \
	if [ $$rc1 -ne 0 ]; then echo "ERROR: transport tests FAILED (rc=$$rc1)"; exit $$rc1; fi; \
	echo "transport tests PASSED"
	@echo "Running dsp tests..."
	@./tests/run_dsp_tests; rc2=$$?; \
	if [ $$rc2 -ne 0 ]; then echo "ERROR: dsp tests FAILED (rc=$$rc2)"; exit $$rc2; fi; \
	echo "dsp tests PASSED"
	@echo "Running engine tests..."
	@./tests/run_engine_tests; rc3=$$?; \
	if [ $$rc3 -ne 0 ]; then echo "ERROR: engine tests FAILED (rc=$$rc3)"; exit $$rc3; fi; \
	echo "engine tests PASSED"
	@echo "All tests completed successfully."

tests/run_transport_tests: $(TEST_SRCS_TRANSPORT) source/transport.c source/transport.h source/alo_util.c
	$(CC) -Isource $^ $(BUILD_C_FLAGS) -DUNIT_TESTS -UNDEBUG $(LINK_FLAGS) -lm -o $@
	chmod +x $@

tests/run_dsp_tests: $(TEST_SRCS_DSP) source/alo_util.c source/sampler_cache.c source/slice_sampler.c source/transient_detector.c
	$(CC) -Isource $^ $(BUILD_C_FLAGS) -DUNIT_TESTS -UNDEBUG $(LINK_FLAGS) -lm -o $@
	chmod +x $@

tests/run_engine_tests: tests/engine_test.c source/button_logic.c source/alo_util.c source/loop_state.c source/transport.c source/slice_sampler.c source/sampler_cache.c source/transient_detector.c
	$(CC) -Isource $^ $(BUILD_C_FLAGS) -DUNIT_TESTS -UNDEBUG $(LINK_FLAGS) -lm -o $@
	chmod +x $@

check: tests scan
	@echo "CI check complete — all tests passed, no analyzer bugs."

safe:
	$(MAKE) SAFE_MATH=true


# --------------------------------------------------------------
# aloschen build rules

build: aloschen.lv2/aloschen$(LIB_EXT) aloschen.lv2/aloschen_ui$(LIB_EXT) aloschen.lv2/manifest.ttl

aloschen.lv2/aloschen$(LIB_EXT): source/aloschen.c source/loop_engine.c source/loop_state.c source/loop_playback.c source/alo_util.c source/slice_sampler.c source/transport.c source/sampler_cache.c source/button_logic.c source/transient_detector.c
	$(CC) $^ $(BUILD_C_FLAGS) $(LINK_FLAGS) -Wall -Wextra -lm $(SHARED) -o $@

aloschen.lv2/aloschen_ui$(LIB_EXT): source/aloschen_ui.c
	$(CC) $^ $(BUILD_C_FLAGS) $(LINK_FLAGS) -Wall -Wextra -lX11 -lm $(SHARED) -o $@

aloschen.lv2/manifest.ttl: aloschen.lv2/manifest.ttl.in
	sed -e "s|@LIB_EXT@|$(LIB_EXT)|" $< > $@

# --------------------------------------------------------------

clean:
	rm -f aloschen.lv2/aloschen$(LIB_EXT) aloschen.lv2/aloschen_ui$(LIB_EXT) aloschen.lv2/manifest.ttl tests/run_transport_tests tests/run_dsp_tests tests/run_engine_tests

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
