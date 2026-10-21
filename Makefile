# Modified by Mickael DE LA ROQUE on 2026-09-08 to build and test the
# unofficial custom-map stream-stall workaround.
TARGET = BO3MacFix.dylib
TEST_TARGETS = tests/test_stream_stall_recovery tests/test_display_policy tests/test_sdl_displays tests/test_prepatch \
	tests/test_settings_ini tests/test_notice_queue tests/test_controllers tests/test_gc_bridge tests/test_frame_limit \
	tests/test_players_cfg tests/test_aae_build tests/test_notch tests/test_notch_hooks tests/test_checkpoint \
	tests/test_zm_inventory tests/test_zm_loadout tests/test_checkpoint_autosave tests/test_checkpoint_autosave_tsan \
	tests/test_aae_cheats tests/test_script_errors tests/test_quick_save tests/test_dynres tests/test_gpu_diag \
	tests/test_t7o_subtitle
TEST_TARGETS += tests/test_launch_args

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
# repeated script errors in console_mp.log (script_errors.c)
SOURCES += source/script_errors.c
SOURCES += source/controllers.c source/gpu_tweaks.c
# which players folder a .cfg comes from, for mods that save them from Lua (players_cfg.c)
SOURCES += source/players_cfg.c
# which frame rate limit applies to a frame, and the pause menu's and the menus' state for PauseMenuFPS and MenuFPS
# (frame_limit.c), and whether the game is in the background for BackgroundFPS (app_focus.m)
SOURCES += source/frame_limit.c source/app_focus.m
# DynamicResolution's controller (dynres.c), plain C like frame_limit.c
SOURCES += source/dynres.c
# PlayStation controllers through macOS's GameController framework (gc_bridge.m), weak-linked because it needs macOS 11.3
SOURCES += source/gc_bridge.m
FRAMEWORKS += -framework Foundation -weak_framework GameController -weak_framework CoreHaptics

# MetalFX upscaling (metalfx.m), MetalFX is weak-linked because it needs macOS 13
SOURCES += source/metalfx.m source/gpu_diag.m

# the pre-patched executable (prepatch.c), the launcher builds it with BO3MacFix_BuildPrepatched
SOURCES += source/prepatch.c
# whether the command line starts a map at launch, for SkipIntro (launch_args.c)
SOURCES += source/launch_args.c

# the build All-around Enhancement checks for, read from its fastfile (aae_build.c)
SOURCES += source/aae_build.c
# a map started with AAE keeping cheats off, so its bank and weapon locker load (aae_cheats.c)
SOURCES += source/aae_cheats.c
# the arguments of T7Overcharged's printsubtitle command, which prints AAE's game messages (t7overcharged.c)
SOURCES += source/t7o_subtitle.c
# saving a zombies game and resuming it through AAE's starting options (checkpoint.c, checkpoint.lua), and giving its
# weapons back with their camo, attachments and ammo types, and its perks (zm_inventory.c, zm_loadout.c)
SOURCES += source/checkpoint.c source/zm_inventory.c source/zm_loadout.c
# saving it by itself at each new round, on a background queue, and deleting the save (checkpoint_autosave.c)
SOURCES += source/checkpoint_autosave.c

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
# keeping the picture below the built-in display's camera housing in fullscreen (notch.m, AvoidNotch), with its
# geometry in notch_layout.c
SOURCES += source/notch.m source/notch_layout.c
# on-screen notices in the game (notices.m with the queue in notice_queue.c, drawn by notices.lua) and pausing when the
# controller disconnects
SOURCES += source/notices.m source/notice_queue.c
# a key, or L3 + R3 in the pause menu, saving a solo zombies game like the settings page's Save game (quick_save.m, with
# its rules in quick_save_rules.c; checkpoint.lua does the saving)
SOURCES += source/quick_save.m source/quick_save_rules.c
# ColorSync for display UUIDs, CoreVideo for the refresh rate of SDL's display modes
FRAMEWORKS += -framework ColorSync -framework CoreVideo

all: $(TARGET)

test: $(TEST_TARGETS)
	for test in $(TEST_TARGETS); do ./$$test || exit 1; done
	@# every setting can be overridden at env (BO3MACFIX_<NAME>)
	sh tests/test_env_overrides.sh
	@# notices.lua against a made-up LUI, with any Lua 5.x
	if command -v lua >/dev/null; then lua tests/test_notices.lua; else echo "no lua, skipping tests/test_notices.lua"; fi
	@# the T7Overcharged stand-in's watch for missing Engine and CoD functions (BO3MACFIX_LUAMISSING)
	if command -v lua >/dev/null; then lua tests/test_t7overcharged.lua; else echo "no lua, skipping tests/test_t7overcharged.lua"; fi
	@# checkpoint.lua (saving and resuming a zombies game) against a made-up Engine and AAE
	if command -v lua >/dev/null; then lua tests/test_checkpoint.lua; else echo "no lua, skipping tests/test_checkpoint.lua"; fi
	@# AutoSave's round trigger and the Delete saved game row in checkpoint.lua
	if command -v lua >/dev/null; then lua tests/test_autosave.lua; else echo "no lua, skipping tests/test_autosave.lua"; fi
	@# the quick save (a key, or L3 + R3 in the pause menu) and the saved game's details on the settings page
	if command -v lua >/dev/null; then lua tests/test_quick_save.lua; else echo "no lua, skipping tests/test_quick_save.lua"; fi

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

# AvoidNotch's geometry (notch_layout.c) against Aspyr's mouse mapping, plain C
tests/test_notch: tests/test_notch.c source/notch_layout.c source/notch_layout.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_notch.c source/notch_layout.c

# notch.m's letterbox and Metal client rect hooks against a made-up ASL window and stand-ins for Aspyr's functions
tests/test_notch_hooks: tests/test_notch_hooks.m source/notch.m source/notch.h source/notch_layout.c source/notch_layout.h \
		source/offsets.h
	$(CLANG) -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -mmacosx-version-min=$(OSXVER) \
		-framework AppKit -o $@ tests/test_notch_hooks.m source/notch_layout.c

# prepatch.c's builder and runtime against a made-up executable with the game's layout (the test includes the source
# itself), with the address and undefined behaviour sanitizers since it's all byte offsets
tests/test_prepatch: tests/test_prepatch.c source/prepatch.c source/prepatch.h source/offsets.h source/dobby.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_prepatch.c

# the notices' queue and low battery levels (notice_queue.c) are plain C too
# quick_save_rules.c: when L3 + R3 count as held, and the request waiting for checkpoint.lua
tests/test_dynres: tests/test_dynres.c source/dynres.c source/dynres.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_dynres.c source/dynres.c

tests/test_launch_args: tests/test_launch_args.c source/launch_args.c source/launch_args.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_launch_args.c \
		source/launch_args.c

tests/test_quick_save: tests/test_quick_save.c source/quick_save_rules.c source/quick_save_rules.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_quick_save.c source/quick_save_rules.c

tests/test_notice_queue: tests/test_notice_queue.c source/notice_queue.c source/notice_queue.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_notice_queue.c source/notice_queue.c

# which frame rate limit applies to a frame and the pause menu's state (frame_limit.c), plain C as well
tests/test_frame_limit: tests/test_frame_limit.c source/frame_limit.c source/frame_limit.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_frame_limit.c source/frame_limit.c

# which players folder a .cfg comes from (players_cfg.c), against made-up folders
tests/test_players_cfg: tests/test_players_cfg.c source/players_cfg.c source/players_cfg.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_players_cfg.c source/players_cfg.c

# checkpoint.c (AAE's mutations lines, saving, applying and clearing a checkpoint) with made-up files, and the
# sanitizers since it parses files
tests/test_checkpoint: tests/test_checkpoint.c source/checkpoint.c source/checkpoint.h source/zm_loadout.c \
		source/zm_loadout.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_checkpoint.c \
		source/checkpoint.c source/zm_loadout.c

# zm_loadout.c (a loadout's names and checkpoint.ini text, reading the script VM's variables, restoring) against a
# made-up memory image and made-up game calls, with the sanitizers since it's all offsets and parsing
tests/test_zm_loadout: tests/test_zm_loadout.c source/zm_loadout.c source/zm_loadout.h tests/fake_vm.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_zm_loadout.c \
		source/zm_loadout.c

# aae_cheats.c's decision and its reads of the game's command and mod name, against made-up memory (the test
# includes the source itself)
tests/test_aae_cheats: tests/test_aae_cheats.c source/aae_cheats.c source/aae_cheats.h source/offsets.h \
		source/dobby.h source/zm_loadout.c source/zm_loadout.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_aae_cheats.c \
		source/zm_loadout.c

# printsubtitle's arguments (t7o_subtitle.c), as AAE's HUD gives them
tests/test_t7o_subtitle: tests/test_t7o_subtitle.c source/t7o_subtitle.c source/t7o_subtitle.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_t7o_subtitle.c \
		source/t7o_subtitle.c

# script_errors.c's QuietScriptErrors: which script errors it shows, against made-up script engine memory (the test
# includes the source itself)
tests/test_script_errors: tests/test_script_errors.c source/script_errors.c source/script_errors.h source/offsets.h \
		source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_script_errors.c

# zm_inventory.c's pass through the game's cheat check for BO3MacFix's give and take commands, against a made-up
# g_entities (the test includes the source)
tests/test_zm_inventory: tests/test_zm_inventory.c source/zm_inventory.c source/zm_inventory.h source/offsets.h \
		source/dobby.h source/zm_loadout.c source/zm_loadout.h tests/fake_vm.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -o $@ \
		tests/test_zm_inventory.c source/zm_loadout.c

# checkpoint_autosave.c (AutoSave's background writer, its rules, deleting the save, the lock) with made-up files, with
# the address and undefined behaviour sanitizers, and again with the thread sanitizer since it's about two threads
tests/test_checkpoint_autosave: tests/test_checkpoint_autosave.c source/checkpoint_autosave.c source/checkpoint_autosave.h \
		source/checkpoint.c source/checkpoint.h source/zm_loadout.c source/zm_loadout.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_checkpoint_autosave.c \
		source/checkpoint_autosave.c source/checkpoint.c source/zm_loadout.c

tests/test_checkpoint_autosave_tsan: tests/test_checkpoint_autosave.c source/checkpoint_autosave.c \
		source/checkpoint_autosave.h source/checkpoint.c source/checkpoint.h source/zm_loadout.c source/zm_loadout.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread -o $@ tests/test_checkpoint_autosave.c \
		source/checkpoint_autosave.c source/checkpoint.c source/zm_loadout.c

# controllers.c (when the game is asked to look for controllers, the version SDL is given) against a fake game image and
# fake HID devices, and gc_bridge.m's player slots as the game's XInput calls see them, with stand-in controllers
tests/test_controllers: tests/test_controllers.c source/controllers.c source/controllers.h source/offsets.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -mmacosx-version-min=$(OSXVER) \
		-framework CoreFoundation -framework IOKit -o $@ tests/test_controllers.c

# gpu_diag.m's command buffer timing (FrameStats detail, DynamicResolution): the commit swizzle, with a stand-in command
# buffer class and another thread committing while it goes in
tests/test_gpu_diag: tests/test_gpu_diag.m source/gpu_diag.m source/gpu_diag.h source/metalfx.h
	$(CLANG) -O1 -Wall -Wextra -Werror -Wno-unused-parameter -mmacosx-version-min=$(OSXVER) -framework AppKit \
		-framework Metal -framework QuartzCore -o $@ tests/test_gpu_diag.m

tests/test_gc_bridge: tests/test_gc_bridge.m source/gc_bridge.m source/gc_bridge.h source/controllers.h source/offsets.h
	$(CLANG) -O1 -Wall -Wextra -Werror -Wno-unused-parameter -mmacosx-version-min=$(OSXVER) -framework Foundation \
		-weak_framework GameController -weak_framework CoreHaptics -framework IOKit -o $@ tests/test_gc_bridge.m

$(TARGET): $(SOURCES) $(wildcard source/*.h) source/t7overcharged_lua.h source/settings_menu_lua.h source/notices_lua.h source/checkpoint_lua.h
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

source/checkpoint_lua.h: source/checkpoint.lua
	cd source && xxd -i checkpoint.lua > checkpoint_lua.h

clean:
	rm -f -- $(TARGET) $(TEST_TARGETS) source/t7overcharged_lua.h source/settings_menu_lua.h source/notices_lua.h source/checkpoint_lua.h

.PHONY: all clean test
