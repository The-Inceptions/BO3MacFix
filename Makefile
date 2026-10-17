# Modified by Mickael DE LA ROQUE on 2026-09-08 to build and test the
# unofficial custom-map stream-stall workaround.
TARGET = BO3MacFix.dylib
TEST_TARGET = tests/test_stream_stall_recovery

DEBUG   ?= 0
ARCH    ?= x86_64
SDK     ?= macosx
OSXVER  ?= 10.13

SYSROOT  := $(shell xcrun --sdk $(SDK) --show-sdk-path)
ifeq ($(SYSROOT),)
$(error Could not find SDK "$(SDK)")
endif
CLANG    := clang
CC       := $(CLANG) -isysroot $(SYSROOT) -arch $(ARCH) -mmacosx-version-min=$(OSXVER)

CFLAGS  = -O1 -Wall -g -I include/ -I src/ -fvisibility=hidden
LDFLAGS = -shared -Wl,-undefined -Wl,dynamic_lookup -Llib -ldobby

FRAMEWORKS = -framework CoreFoundation -framework IOKit -framework CoreGraphics -framework ImageIO

SOURCES = source/bo3macfix.c source/bo3macnative.m source/stream_stall_recovery.c source/utilities.c source/steam.c source/exports.c source/havok.c source/t7overcharged.c source/cpu_tuning.c
SOURCES += source/controllers.c source/gpu_tweaks.c
# PlayStation controllers through macOS's GameController framework (gc_bridge.m), weak-linked because it needs macOS 11.3
SOURCES += source/gc_bridge.m
FRAMEWORKS += -framework Foundation -weak_framework GameController -weak_framework CoreHaptics

# MetalFX upscaling (metalfx.m), MetalFX is weak-linked because it needs macOS 13
SOURCES += source/metalfx.m source/gpu_diag.m

# the pre-patched executable (prepatch.c), the launcher builds it with BO3MacFix_BuildPrepatched
SOURCES += source/prepatch.c

# settings.ini's options for an in-game menu, the settings.ini writer and restarting (settings.c)
SOURCES += source/settings.c
# the in-game settings page that uses it (settings_menu.c)
SOURCES += source/settings_menu.c

# quitting without a crash and audio following the default output device (qol.c)
SOURCES += source/qol.c source/rawmouse.c
FRAMEWORKS += -framework AudioToolbox -framework CoreAudio
FRAMEWORKS += -framework Metal -framework QuartzCore -framework AppKit -weak_framework MetalFX

all: $(TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): tests/test_stream_stall_recovery.c source/stream_stall_recovery.c source/stream_stall_recovery.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_stream_stall_recovery.c source/stream_stall_recovery.c

$(TARGET): $(SOURCES) source/t7overcharged_lua.h source/settings_menu_lua.h
	$(CC) $(CFLAGS) $(FRAMEWORKS) $(DEFINES) $(LDFLAGS) -o $@ $(SOURCES)

# the Lua half of our T7Overcharged support, embedded as a byte array
source/t7overcharged_lua.h: source/t7overcharged.lua
	cd source && xxd -i t7overcharged.lua > t7overcharged_lua.h

# the in-game settings page's Lua (settings_menu.c), embedded as a byte array
source/settings_menu_lua.h: source/settings_menu.lua
	cd source && xxd -i settings_menu.lua > settings_menu_lua.h

clean:
	rm -f -- $(TARGET) $(TEST_TARGET) source/t7overcharged_lua.h source/settings_menu_lua.h

.PHONY: all clean test
