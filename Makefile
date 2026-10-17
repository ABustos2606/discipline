# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos

PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2
DESTDIR ?=
OPT ?= -O2
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
SAN ?= 0
BUILD_DIR ?= build
ifeq ($(SAN),1)
override BUILD_DIR := $(BUILD_DIR)/san
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
endif
ifeq ($(SAN),thread)
override BUILD_DIR := $(BUILD_DIR)/tsan
SAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g
endif

BUNDLE := $(BUILD_DIR)/discipline.lv2
SOURCES := src/lv2_entry.cpp src/discipline.cpp src/discipline_return.cpp src/pair_registry.cpp
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/obj/%.o,$(SOURCES))
METADATA := manifest.ttl discipline.ttl discipline_return.ttl
TTL_TARGETS := $(addprefix $(BUNDLE)/,$(METADATA))
LV2_CFLAGS = $(shell $(PKG_CONFIG) --cflags lv2)
COMPILE_FLAGS = $(CPPFLAGS) $(LV2_CFLAGS) $(CXXFLAGS) $(OPT) -std=c++17 -fPIC \
	-fvisibility=hidden -Wall -Wextra -fno-math-errno -fno-trapping-math $(SAN_FLAGS)

.PHONY: all install test test-timing test-hosts render lint clean
all: $(BUNDLE)/discipline.so $(TTL_TARGETS)

$(BUILD_DIR)/obj/%.o: src/%.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) -MMD -MP -c $< -o $@

$(BUNDLE)/discipline.so: $(OBJECTS) src/exports.map
	@mkdir -p "$(@D)"
	$(CXX) $(OBJECTS) $(LDFLAGS) $(SAN_FLAGS) -shared -Wl,--no-undefined \
		-Wl,--version-script=src/exports.map -o $@

$(BUNDLE)/%.ttl: discipline.lv2/%.ttl
	@mkdir -p "$(@D)"
	install -m 644 $< $@

$(BUILD_DIR)/test_scaffold: tests/test_scaffold.cpp src/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) $< $(LDFLAGS) $(SAN_FLAGS) -ldl -pthread -o $@

$(BUILD_DIR)/test_core: tests/test_core.cpp tests/host.cpp tests/host.hpp src/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) tests/test_core.cpp tests/host.cpp $(LDFLAGS) $(SAN_FLAGS) -ldl -pthread -o $@

$(BUILD_DIR)/test_dsp: tests/test_dsp.cpp tests/host.cpp tests/host.hpp $(wildcard src/dsp/*.hpp) src/host_config.hpp src/pair_registry.cpp src/pair_registry.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) tests/test_dsp.cpp tests/host.cpp src/pair_registry.cpp $(LDFLAGS) $(SAN_FLAGS) -ldl -pthread -o $@

$(BUILD_DIR)/render: tools/render.cpp tests/host.cpp tests/host.hpp src/ports.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(COMPILE_FLAGS) tools/render.cpp tests/host.cpp $(LDFLAGS) $(SAN_FLAGS) -ldl -pthread -o $@

render: all $(BUILD_DIR)/render

test: all $(BUILD_DIR)/test_scaffold $(BUILD_DIR)/test_dsp $(BUILD_DIR)/test_core $(BUILD_DIR)/render
	$(PYTHON) tests/ttl_check.py $(BUNDLE)
	$(BUILD_DIR)/test_scaffold $(BUNDLE)/discipline.so
	$(BUILD_DIR)/test_dsp
	$(BUILD_DIR)/test_core $(BUNDLE)/discipline.so
	$(BUILD_DIR)/test_core $(BUNDLE)/discipline.so --timing
	$(PYTHON) tests/render_check.py $(BUILD_DIR)/render $(BUNDLE)/discipline.so

test-timing: all $(BUILD_DIR)/test_core
	$(BUILD_DIR)/test_core $(BUNDLE)/discipline.so --timing

test-hosts: all
	$(PYTHON) tests/host_smoke.py $(BUNDLE)

install: all
	install -d "$(DESTDIR)$(LV2DIR)/discipline.lv2"
	install -m 755 $(BUNDLE)/discipline.so "$(DESTDIR)$(LV2DIR)/discipline.lv2/"
	install -m 644 $(TTL_TARGETS) LICENSE "$(DESTDIR)$(LV2DIR)/discipline.lv2/"

lint:
	@if command -v lv2lint >/dev/null 2>&1; then \
		LV2_PATH="$(DESTDIR)$(LV2DIR):$${LV2_PATH:-/usr/local/lib/lv2:/usr/lib/lv2}" \
		lv2lint 'https://github.com/ABustos2606/discipline#discipline' && \
		LV2_PATH="$(DESTDIR)$(LV2DIR):$${LV2_PATH:-/usr/local/lib/lv2:/usr/lib/lv2}" \
		lv2lint 'https://github.com/ABustos2606/discipline#return'; \
	else echo 'SKIP lv2lint: not installed (install the bundle with make install first)'; fi

clean:
	$(RM) $(OBJECTS) $(OBJECTS:.o=.d) $(BUNDLE)/discipline.so $(TTL_TARGETS) $(BUNDLE)/LICENSE $(BUILD_DIR)/test_scaffold $(BUILD_DIR)/test_dsp $(BUILD_DIR)/test_core $(BUILD_DIR)/render

-include $(OBJECTS:.o=.d)
