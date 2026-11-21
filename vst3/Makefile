#!/usr/bin/make -f
# Aloschen VST3 (DPF). Build from repo root with `make vst3`.
# Requires deps/DPF (gitignored): git clone --recursive https://github.com/DISTRHO/DPF deps/DPF

NAME = aloschen
DPF_PATH ?= ../deps/DPF
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

include $(DPF_PATH)/Makefile.plugins.mk

BUILD_CXX_FLAGS += -I. -I../source -Wall -Wextra -Wno-unused-parameter
EXTRA_DSP_LIBS += -lm

# Engine archive is a prerequisite, so DPF's link rule picks it up via $^.
$(vst3): $(ENGINE_LIB)

vpath %.c ../source .

$(DPF_BUILD_DIR)/engine/%.o: %.c
	-@mkdir -p $(dir $@)
	$(CC) -I../source -I. -std=gnu99 -O3 -ffast-math -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -DNDEBUG -c $< -o $@

$(ENGINE_LIB): $(ENGINE_OBJS)
	$(AR) rcs $@ $^

TARGETS = vst3

all: $(TARGETS)
