# Modified by Mickael DE LA ROQUE on 2026-09-08 to build and test the
# unofficial custom-map stream-stall workaround.
TARGET = BO3MacFix.dylib
TEST_TARGETS = tests/test_stream_stall_recovery tests/test_display_policy tests/test_sdl_displays tests/test_prepatch \
	tests/test_settings_ini tests/test_notice_queue tests/test_controllers tests/test_gc_bridge tests/test_frame_limit \
	tests/test_aae_build

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
LDFLAGS = -shared -Wl,-undefined -Wl,dynamic_lookup -Llib -ldobby -lz

FRAMEWORKS = -framework CoreFoundation -framework IOKit -framework CoreGraphics -framework ImageIO

SOURCES = source/bo3macfix.c source/bo3macnative.m source/stream_stall_recovery.c source/utilities.c source/steam.c source/exports.c source/havok.c source/t7overcharged.c source/cpu_tuning.c
SOURCES += source/controllers.c source/gpu_tweaks.c
# which frame rate limit applies to a frame, and the pause menu's state for PauseMenuFPS (frame_limit.c)
SOURCES += source/frame_limit.c
# PlayStation controllers through macOS's GameController framework (gc_bridge.m), weak-linked because it needs macOS 11.3
SOURCES += source/gc_bridge.m
FRAMEWORKS += -framework Foundation -weak_framework GameController -weak_framework CoreHaptics

# MetalFX upscaling (metalfx.m), MetalFX is weak-linked because it needs macOS 13
SOURCES += source/metalfx.m source/gpu_diag.m

# the pre-patched executable (prepatch.c), the launcher builds it with BO3MacFix_BuildPrepatched
SOURCES += source/prepatch.c

# the build All-around Enhancement checks for, read from its fastfile (aae_build.c)
SOURCES += source/aae_build.c

# settings.ini's options for an in-game menu, the settings.ini writer and restarting (settings.c), with the
# settings.ini reading and writing (settings_ini.c)
SOURCES += source/settings.c source/settings_ini.c
# the in-game settings page that uses it (settings_menu.c)
SOURCES += source/settings_menu.c

# quitting without a crash and audio following the default output device (qol.c)
SOURCES += source/qol.c source/rawmouse.c
FRAMEWORKS += -framework AudioToolbox -framework CoreAudio
FRAMEWORKS += -framework Metal -framework QuartzCore -framework AppKit -weak_framework MetalFX

# following display changes (display_follow.m, display_policy.c), with SDL's display list (sdl_displays.c and its
# AppKit half sdl_displays_cocoa.m) and the engine's video code (engine_display.c) doing the moving, and the display
# tests' hooks (display_testhooks.m)
SOURCES += source/display_follow.m source/display_policy.c source/sdl_displays.c source/sdl_displays_cocoa.m
SOURCES += source/engine_display.c source/display_testhooks.m
# on-screen notices in the game (notices.m with the queue in notice_queue.c, drawn by notices.lua) and pausing when the
# controller disconnects
SOURCES += source/notices.m source/notice_queue.c
# ColorSync for display UUIDs, CoreVideo for the refresh rate of SDL's display modes
FRAMEWORKS += -framework ColorSync -framework CoreVideo

all: $(TARGET)

test: $(TEST_TARGETS)
	for test in $(TEST_TARGETS); do ./$$test || exit 1; done
	@# notices.lua against a made-up LUI, with any Lua 5.x
	if command -v lua >/dev/null; then lua tests/test_notices.lua; else echo "no lua, skipping tests/test_notices.lua"; fi

tests/test_stream_stall_recovery: tests/test_stream_stall_recovery.c source/stream_stall_recovery.c source/stream_stall_recovery.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_stream_stall_recovery.c source/stream_stall_recovery.c

# aae_build.c against made-up Lua constants, fastfiles and caches, and the installed All-around Enhancement's
# core_mod.ff if there is one, with the address and undefined behaviour sanitizers since it parses files
tests/test_aae_build: tests/test_aae_build.c source/aae_build.c source/aae_build.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_aae_build.c \
		source/aae_build.c -lz

# settings_ini.c's reading and writing, against odd settings.ini files
tests/test_settings_ini: tests/test_settings_ini.c source/settings_ini.c source/settings_ini.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_settings_ini.c source/settings_ini.c

# display_policy.c's decisions are plain C, so they're tested with made-up displays, without the game
tests/test_display_policy: tests/test_display_policy.c source/display_policy.c source/display_policy.h source/display_follow.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_display_policy.c source/display_policy.c

# sdl_displays.c against a fake SDL device (the test includes the source itself), for the dylib's macOS version since
# the CVDisplayLink calls it makes are deprecated from macOS 15
tests/test_sdl_displays: tests/test_sdl_displays.c source/sdl_displays.c source/sdl_displays.h source/engine_display.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -mmacosx-version-min=$(OSXVER) -framework CoreFoundation \
		-framework CoreGraphics -framework CoreVideo -o $@ tests/test_sdl_displays.c

# prepatch.c's builder and runtime against a made-up executable with the game's layout (the test includes the source
# itself), with the address and undefined behaviour sanitizers since it's all byte offsets
tests/test_prepatch: tests/test_prepatch.c source/prepatch.c source/prepatch.h source/offsets.h source/dobby.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_prepatch.c

# the notices' queue and low battery levels (notice_queue.c) are plain C too
tests/test_notice_queue: tests/test_notice_queue.c source/notice_queue.c source/notice_queue.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_notice_queue.c source/notice_queue.c

# which frame rate limit applies to a frame and the pause menu's state (frame_limit.c), plain C as well
tests/test_frame_limit: tests/test_frame_limit.c source/frame_limit.c source/frame_limit.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_frame_limit.c source/frame_limit.c

# controllers.c (when the game is asked to look for controllers, the version SDL is given) against a fake game image and
# fake HID devices, and gc_bridge.m's player slots as the game's XInput calls see them, with stand-in controllers
tests/test_controllers: tests/test_controllers.c source/controllers.c source/controllers.h source/offsets.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -mmacosx-version-min=$(OSXVER) \
		-framework CoreFoundation -framework IOKit -o $@ tests/test_controllers.c

tests/test_gc_bridge: tests/test_gc_bridge.m source/gc_bridge.m source/gc_bridge.h source/controllers.h source/offsets.h
	$(CLANG) -O1 -Wall -Wextra -Werror -Wno-unused-parameter -mmacosx-version-min=$(OSXVER) -framework Foundation \
		-weak_framework GameController -weak_framework CoreHaptics -framework IOKit -o $@ tests/test_gc_bridge.m

$(TARGET): $(SOURCES) source/t7overcharged_lua.h source/settings_menu_lua.h source/notices_lua.h
	$(CC) $(CFLAGS) $(FRAMEWORKS) $(DEFINES) $(LDFLAGS) -o $@ $(SOURCES)

# the Lua half of our T7Overcharged support, embedded as a byte array
source/t7overcharged_lua.h: source/t7overcharged.lua
	cd source && xxd -i t7overcharged.lua > t7overcharged_lua.h

# the in-game settings page's Lua (settings_menu.c), embedded as a byte array
source/settings_menu_lua.h: source/settings_menu.lua
	cd source && xxd -i settings_menu.lua > settings_menu_lua.h

# the notices' Lua (notices.m), embedded as a byte array
source/notices_lua.h: source/notices.lua
	cd source && xxd -i notices.lua > notices_lua.h

clean:
	rm -f -- $(TARGET) $(TEST_TARGETS) source/t7overcharged_lua.h source/settings_menu_lua.h source/notices_lua.h

.PHONY: all clean test
