# tapecap — Makefile
#
# Builds a universal (arm64 + x86_64) macOS command-line binary that captures
# raw DV / HDV bitstreams from a FireWire camcorder or deck.
#
# Requirements: Xcode command-line tools on macOS. FireWire headers are part of
# the macOS SDK through macOS 15 (Sequoia); they were removed in macOS 26, so
# this only builds on SDKs up to and including macOS 15.
#
#   make            # build build/tapecap (universal)
#   make ARCHS=...  # override architectures, e.g. ARCHS="-arch arm64"
#   make install    # install to $(PREFIX)/bin (default /usr/local)
#   make clean

CXX        ?= clang++
ARCHS      ?= -arch arm64 -arch x86_64
MIN        ?= -mmacosx-version-min=11.0
STD        ?= -std=c++14
OPT        ?= -O2

# The vendored AVCVideoServices code is decades-old Apple sample code; silence
# the expected deprecation / encoding noise but keep real warnings.
WARN       := -Wall \
              -Wno-deprecated-declarations \
              -Wno-invalid-source-encoding \
              -Wno-unused-variable \
              -Wno-unused-private-field \
              -Wno-unused-function \
              -Wno-writable-strings \
              -Wno-format-security

INCLUDES   := -Ithird_party/AVCVideoServices
FRAMEWORKS := -framework IOKit -framework CoreFoundation -framework CoreServices

CXXFLAGS   := $(ARCHS) $(MIN) $(STD) $(OPT) $(WARN) $(INCLUDES)
LDFLAGS    := $(ARCHS) $(MIN) $(FRAMEWORKS)

BUILD      := build
BIN        := $(BUILD)/tapecap

VENDOR_SRCS := $(wildcard third_party/AVCVideoServices/*.cpp)
APP_SRCS    := $(wildcard src/*.cpp)
SRCS        := $(VENDOR_SRCS) $(APP_SRCS)
OBJS        := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))

PREFIX     ?= /usr/local

.PHONY: all clean install lipo
all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)
	@echo "Built $@"

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Show the architectures baked into the final binary.
lipo: $(BIN)
	lipo -info $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/tapecap
	@echo "Installed to $(DESTDIR)$(PREFIX)/bin/tapecap"

clean:
	rm -rf $(BUILD)
