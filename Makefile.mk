#!/usr/bin/make -f
# Makefile for alo.lv2 #
# -------------------- #
#

AR  ?= ar
CC  ?= gcc
CXX ?= g++

# --------------------------------------------------------------
# Fallback to Linux if no other OS defined

ifneq ($(MACOS),true)
ifneq ($(WIN32),true)
LINUX=true
endif
endif

# --------------------------------------------------------------
# Set build and link flags

BASE_FLAGS = -Wall -Wextra -pipe -Wno-unused-parameter
BASE_OPTS  = -O3 -ffast-math

# --------------------------------------------------------------
# Optional debug toggles (off by default)
#
# - SAFE_MATH=true: disable -ffast-math to rule out UB/precision artifacts
# - MATH_CHECKS=true: adds -DALO_MATH_CHECKS (RT-safe clamps for NaN/Inf)
# - SANITIZE=address|undefined|...: enables compiler sanitizers (debugging)

SANITIZE_CFLAGS  =
SANITIZE_LDFLAGS =

ifeq ($(SAFE_MATH),true)
BASE_OPTS  = -O3 -fno-fast-math -fno-unsafe-math-optimizations -fno-finite-math-only -fno-associative-math
endif

ifeq ($(MATH_CHECKS),true)
BASE_FLAGS += -DALO_MATH_CHECKS
endif

ifneq ($(SANITIZE),)
SANITIZE_CFLAGS  = -fno-omit-frame-pointer -fsanitize=$(SANITIZE)
SANITIZE_LDFLAGS = -fsanitize=$(SANITIZE)
endif

ifeq ($(MACOS),true)
# MacOS linker flags
LINK_OPTS  = -Wl,-dead_strip -Wl,-dead_strip_dylibs
else
# Common linker flags
ifneq ($(SANITIZE),)
LINK_OPTS  = -Wl,-O1 -Wl,--as-needed
else
LINK_OPTS  = -Wl,-O1 -Wl,--as-needed -Wl,--strip-all
endif
endif

ifneq ($(WIN32),true)
# not needed for Windows
BASE_FLAGS += -fPIC -DPIC
endif

ifeq ($(DEBUG),true)
BASE_FLAGS += -DDEBUG -O0 -g
LINK_OPTS   =
else
BASE_FLAGS += -DNDEBUG $(BASE_OPTS) -fvisibility=hidden
CXXFLAGS   += -fvisibility-inlines-hidden
endif

BASE_FLAGS += $(SANITIZE_CFLAGS)
LINK_OPTS  += $(SANITIZE_LDFLAGS)

BUILD_C_FLAGS   = $(BASE_FLAGS) -std=c99 -std=gnu99 $(CFLAGS)
BUILD_CXX_FLAGS = $(BASE_FLAGS) -std=c++11 $(CXXFLAGS) $(CPPFLAGS)

ifeq ($(MACOS),true)
# 'no-undefined' is always enabled on MacOS
LINK_FLAGS      = $(LINK_OPTS) $(LDFLAGS)
else
# add 'no-undefined'
LINK_FLAGS      = $(LINK_OPTS) -Wl,--no-undefined $(LDFLAGS)
endif

# --------------------------------------------------------------
# Set shared lib extension

LIB_EXT = .so

ifeq ($(MACOS),true)
LIB_EXT = .dylib
endif

ifeq ($(WIN32),true)
LIB_EXT = .dll
endif

# --------------------------------------------------------------
# Static analysis helpers
#
# The `scan` target runs the Clang static analyzer, using any options passed
# via SCAN_OPTS.  If `scan-build` is not installed the target is a no‑op
# and simply prints a message.  The invocation intentionally does not fail
# the build so you can integrate this into CI without breaking on warnings.
#
# The `tidy` target uses clang-tidy and will consult an existing
# compile_commands.json when present (use Bear or CMake to generate one).
# It is also tolerant of missing binaries.

SCANDIR ?= $(PWD)/source
SCAN_OPTS ?= --status-bugs

all: ;
# No-op placeholder.  The real default target is defined in the
# top-level Makefile (which includes this file).
.PHONY: scan
scan:
	@command -v scan-build >/dev/null 2>&1 || { echo "scan-build not found, skipping"; exit 0; }
	@echo "Running scan-build once on the build rules..."
	# run analyzer over a single invocation of make to avoid recursive targets
	scan-build $(SCAN_OPTS) $(MAKE) all || true
SRCS := $(shell find source -name '*.c' -o -name '*.cpp')

.PHONY: tidy
tidy:
	@command -v clang-tidy >/dev/null 2>&1 || { echo "clang-tidy not found, skipping"; exit 0; }
	@echo "Running clang-tidy on $(SRCS)..."
	@if [ -f compile_commands.json ]; then \
		exec clang-tidy -p . $(SRCS) -- -I. ; \
	else \
		exec clang-tidy $(SRCS) -- -I.; \
	fi || true

# --------------------------------------------------------------
# Optional cppcheck static analysis
# The `cppcheck` rule is quiet by default and tolerates missing binary.
# It mirrors the command documented in AGENTS.md so developers can quickly
# run a broad style check without memorizing the full invocation.

.PHONY: cppcheck
cppcheck:
	@command -v cppcheck >/dev/null 2>&1 || { echo "cppcheck not found, skipping"; exit 0; }
	@echo "Running cppcheck on source tree..."
	cppcheck --enable=all --inconclusive --quiet . || true

# --------------------------------------------------------------
# Set shared library CLI arg

SHARED = -shared

ifeq ($(MACOS),true)
SHARED = -dynamiclib
endif

# --------------------------------------------------------------
