#!/usr/bin/make -f
# Aloschen VST3 (DPF). Build from repo root with `make vst3`.
# Requires the pinned DPF checkout (vst3/README.md).

NAME = aloschen
DPF_PATH ?= ../deps/DPF
DPF_REV = 4238e1c7f0351bbe488d79f0899c540543ac7583
DPF_TARGET_DIR = ../build/vst3
DPF_BUILD_DIR = ../build/vst3-obj

FILES_DSP = AloschenPlugin.cpp
FILES_UI  = AloschenUI.cpp

ENGINE_SRCS = ../source/aloschen.c ../source/loop_engine.c ../source/loop_state.c \
	../source/loop_playback.c ../source/alo_util.c ../source/slice_sampler.c \
	../source/transport.c ../source/sampler_cache.c ../source/button_logic.c \
	../source/transient_detector.c alo_bridge.c
ENGINE_LIB = $(DPF_BUILD_DIR)/libaloengine.a
ENGINE_OBJS = $(patsubst %.c,$(DPF_BUILD_DIR)/engine/%.o,$(notdir $(ENGINE_SRCS)))

# Apply the tracked clock fix to the pinned dependency before parsing DPF's rules.
ifneq ($(shell git -C "$(DPF_PATH)" rev-parse HEAD 2>/dev/null),$(DPF_REV))
$(error VST3 requires DPF $(DPF_REV); see vst3/README.md)
endif
DPF_PATCH := $(abspath dpf-transport.patch)
DPF_READY := $(shell git -C "$(DPF_PATH)" apply --reverse --check "$(DPF_PATCH)" 2>/dev/null || git -C "$(DPF_PATH)" apply "$(DPF_PATCH)" && echo yes)
ifneq ($(DPF_READY),yes)
$(error Could not apply dpf-transport.patch to $(DPF_PATH))
endif

include $(DPF_PATH)/Makefile.plugins.mk

BUILD_CXX_FLAGS += -I. -I../source -Wall -Wextra -Wno-unused-parameter
EXTRA_DSP_LIBS += -lm
DPF_RESOURCE_DIR = $(DPF_TARGET_DIR)/aloschen.vst3/Contents/Resources

.PHONY: bundle-notices
bundle-notices: $(vst3)
	install -Dm644 $(DPF_PATH)/LICENSE $(DPF_RESOURCE_DIR)/LICENSE-DPF
	install -Dm644 $(DPF_PATH)/dgl/src/nanovg/LICENSE.txt $(DPF_RESOURCE_DIR)/LICENSE-NanoVG
	install -Dm644 $(DPF_PATH)/dgl/src/resources/LICENSE-DejaVuSans.ttf.txt $(DPF_RESOURCE_DIR)/LICENSE-DejaVuSans

# Engine archive is a prerequisite, so DPF's link rule picks it up via $^.
$(vst3): $(ENGINE_LIB)

vpath %.c ../source .

$(DPF_BUILD_DIR)/engine/%.o: %.c
	-@mkdir -p $(dir $@)
	$(CC) -I../source -I. -std=gnu99 -O3 -MMD -MP -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -DNDEBUG -c $< -o $@

$(ENGINE_LIB): $(ENGINE_OBJS)
	$(AR) rcs $@ $^

-include $(ENGINE_OBJS:.o=.d)

TARGETS = vst3

all: $(TARGETS) bundle-notices
