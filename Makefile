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
# the plugin's packets and rules for joining a game on the local network, its listener over the loopback (aae_lan.c)
TEST_TARGETS += tests/test_aae_lan
# compiled scripts and All-around Enhancement 4's patch scripts (gsc_obj.c)
TEST_TARGETS += tests/test_gsc_obj
# loading them and redirecting calls, against a made-up game (gsc_detours.c)
TEST_TARGETS += tests/test_gsc_detours
# the script variable table's free list and the bigger table for the server (script_vars.c)
TEST_TARGETS += tests/test_script_vars
# glyphs drawn twice as detailed into the font atlas (font_resolution.c)
TEST_TARGETS += tests/test_font_resolution
# the UI's Lua heap growing past its 60 MB (lua_heap.c)
TEST_TARGETS += tests/test_lua_heap
# releases of Lua reference 0 (lua_unref.c)
TEST_TARGETS += tests/test_lua_unref
# the second address space for loaded files (pmem_spill.c)
TEST_TARGETS += tests/test_pmem_spill
# All-around Enhancement's rules for traversal (aae_mantle.c)
TEST_TARGETS += tests/test_aae_mantle
# the plugin's rules for a host who leaves, and the handlers of its detour and hooks (aae_hostmig.c)
TEST_TARGETS += tests/test_aae_hostmig
# T7Overcharged's fastjump: its rules, its table of times in the air, its handlers (aae_move_fastjump.c), and again with
# the thread sanitizer, since the server's and the client's movement use that table at once
TEST_TARGETS += tests/test_aae_move_fastjump tests/test_aae_move_fastjump_tsan
# T7Overcharged's movement switches: its commands, its start and its dvars (aae_movement.c)
TEST_TARGETS += tests/test_aae_movement tests/test_aae_movement_x86_64
# startweaponanim's weapAnim value (weapon_anim.h)
TEST_TARGETS += tests/test_weapon_anim
# T7Overcharged's slide changes, their places and its own slide push compared with T7Overcharged (aae_move_slide.c)
TEST_TARGETS += tests/test_aae_move_slide tests/test_aae_move_slide_x86
# the thunks for detours inside the game's functions (detour.h); x86_64 code, so this test is built for x86_64
TEST_TARGETS += tests/test_detour
# All-around Enhancement's prompts in the world (aae_prompt.c) and the test for its plugin (aae_plugin.c)
TEST_TARGETS += tests/test_aae_prompt
# T7Overcharged's changes to sprinting: its rules, and the handlers against a made-up game (aae_move_sprint.c)
TEST_TARGETS += tests/test_aae_move_sprint
# when the mod's free aim is on, and the crosshair test's hook (aae_freeaim.c)
TEST_TARGETS += tests/test_aae_freeaim
# the left-hand gestures' entities at the camera, against a made-up game (aae_linkcam.c)
TEST_TARGETS += tests/test_aae_linkcam
# free aim for players on other machines: what is sent, what the server keeps (aae_freeaim_net.c)
TEST_TARGETS += tests/test_aae_freeaim_net
# T7Overcharged's jumpslowdown: its rules, hooks and detours against a made-up game (aae_move_jump.c)
TEST_TARGETS += tests/test_aae_move_jump
# T7Overcharged's bigger table of client effects: its rules, and the six hooks against a made-up cache (aae_clientfx.c)
TEST_TARGETS += tests/test_aae_clientfx
# T7Overcharged's weapon attachment and local client guards (t7o_guards.c)
TEST_TARGETS += tests/test_t7o_guards
# the pause for everybody in a co-op game: the plugin's packets and rules, machines playing together (aae_coop_pause.c)
TEST_TARGETS += tests/test_aae_coop_pause
# T7Overcharged's aim status where the game draws its hold breath hint (aae_breath.c)
TEST_TARGETS += tests/test_aae_breath
# T7Overcharged's moves of the first person gun's bones: the texts, the rules and the hook (aae_viewmodel.c)
TEST_TARGETS += tests/test_aae_viewmodel
# a client field nobody registered: which places get a script error or a field that reads as 0 (clientfield_guard.c)
TEST_TARGETS += tests/test_clientfield_guard
# the grenade's prediction line: T7Overcharged's test, the plugin's sums and the hooks (aae_arc.c)
TEST_TARGETS += tests/test_aae_arc
# the mod's widgets on the first person weapon: the plugin's tables, sums and methods (aae_vmwidget.c)
TEST_TARGETS += tests/test_aae_vmwidget
# two writers of one of its bones at once, under the thread sanitizer (aae_vmwidget.c)
TEST_TARGETS += tests/test_aae_vmwidget_tsan
# the one hook on DObjCalcSkel for both pieces that work after the view model's skeleton (aae_skeleton.c)
TEST_TARGETS += tests/test_aae_skeleton
# T7Overcharged's smaller pieces: customkeymap (t7o_small.c)
TEST_TARGETS += tests/test_t7o_small

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
# All-around Enhancement 4's patch scripts: the compiled script format (gsc_obj.c) and loading them (gsc_detours.c)
SOURCES += source/gsc_obj.c source/gsc_detours.c
# the server's script variables: counting them and a higher limit (script_vars.c)
SOURCES += source/script_vars.c
# SharpFonts: menu and HUD text from glyphs twice as detailed (font_resolution.c)
SOURCES += source/font_resolution.c
# LuaHeapGrowth: more memory for the UI's Lua VM when its 60 MB are full (lua_heap.c)
SOURCES += source/lua_heap.c
# LuaUnrefFix: releasing Lua reference 0 does nothing (lua_unref.c)
SOURCES += source/lua_unref.c
# MoreMapMemory: 2 GB more address space for loaded files (pmem_spill.c)
SOURCES += source/pmem_spill.c
# AAEMantling: mantling by the rules All-around Enhancement 4 gives its Windows plugin (aae_mantle.c)
SOURCES += source/aae_mantle.c
# AAEHostMigration: a host who leaves hands the game on instead of ending it, as the plugin makes it (aae_hostmig.c)
SOURCES += source/aae_hostmig.c
# AAEMovement: T7Overcharged's movement changes that the mod switches on; the switches (aae_movement.c)
SOURCES += source/aae_movement.c
# its patch fastjump: the next jump at once, more air control, the speed kept through a landing (aae_move_fastjump.c)
SOURCES += source/aae_move_fastjump.c
# its jumpslowdown: no penalty for jumping again and again (aae_move_jump.c)
SOURCES += source/aae_move_jump.c
# its changes to sprinting (aae_move_sprint.c)
SOURCES += source/aae_move_sprint.c
# whether the mod with that plugin is the one loaded (aae_plugin.c)
SOURCES += source/aae_plugin.c
# AAEPrompts: the mod's prompts in the world, kept by distance as the plugin makes the game keep them (aae_prompt.c)
SOURCES += source/aae_prompt.c
# AAEMovement: T7Overcharged's slide changes (aae_move_slide.c)
SOURCES += source/aae_move_slide.c
# AAEFreeAim: a crosshair in the mod's third person view, and shots that go to what is under it (aae_freeaim.c);
# what a player on another machine sends the server for that (aae_freeaim_net.c)
SOURCES += source/aae_freeaim.c source/aae_freeaim_net.c
# MoreClientEffects: T7Overcharged's table of 2048 client effects instead of the game's 1024 (aae_clientfx.c)
SOURCES += source/aae_clientfx.c
# AAEGestures: the mod's left-hand gestures put at the camera, as its plugin does (aae_linkcam.c)
SOURCES += source/aae_linkcam.c
# AAECoopPause: the pause for everybody in a co-op game, as the mod's plugin does it (aae_coop_pause.c)
SOURCES += source/aae_coop_pause.c
# AAEHoldBreath: the aim status the mod's T7Overcharged works out where the game draws its hold breath hint
# (aae_breath.c)
SOURCES += source/aae_breath.c
# AAEGunPosition: the mod's moves of the first person gun's bones, as its T7Overcharged makes them (aae_viewmodel.c)
SOURCES += source/aae_viewmodel.c
# AAEGrenadeArc: the line that shows where a held grenade will fly (aae_arc.c)
SOURCES += source/aae_arc.c
# AAEViewmodelWidgets: the two UI element methods the plugin adds for widgets on the first person weapon
# (aae_vmwidget.c)
SOURCES += source/aae_vmwidget.c
# the hook both of them do their work after the view model's skeleton through (aae_skeleton.c)
SOURCES += source/aae_skeleton.c
# the arguments of T7Overcharged's printsubtitle command, which prints AAE's game messages (t7overcharged.c)
SOURCES += source/t7o_subtitle.c
# T7OCrashGuards: two of T7Overcharged's guards against crashes of the game itself (t7o_guards.c)
SOURCES += source/t7o_guards.c
# ClientFieldGuard: a script error instead of a crash for a client field nobody registered (clientfield_guard.c)
SOURCES += source/clientfield_guard.c
# AAELanJoin: joining a game of the mod on the local network as its plugin makes it work (aae_lan.c)
SOURCES += source/aae_lan.c
# T7Overcharged's smaller pieces: customkeymap, SendMenuResponse's debounce, the dvars it changes (t7o_small.c)
SOURCES += source/t7o_small.c
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
	@# each setting's default agrees in the code, its settings page row (settings.c) and README.md
	python3 tests/test_settings_defaults.py
	python3 tests/test_hook_returns.py
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

tests/test_weapon_anim: tests/test_weapon_anim.c source/weapon_anim.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_weapon_anim.c

tests/test_stream_stall_recovery: tests/test_stream_stall_recovery.c source/stream_stall_recovery.c source/stream_stall_recovery.h
	$(CLANG) -std=c11 -O1 -Wall -Wextra -Werror -o $@ tests/test_stream_stall_recovery.c source/stream_stall_recovery.c

# aae_build.c against made-up Lua constants, fastfiles and caches, and the installed All-around Enhancement's
# core_mod.ff if there is one, with the address and undefined behaviour sanitizers since it parses files
tests/test_aae_build: tests/test_aae_build.c source/aae_build.c source/aae_build.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_aae_build.c \
		source/aae_build.c -lz

# gsc_obj.c against made-up scripts and patch scripts, and the installed All-around Enhancement's core_mod.ff if
# there is one, with the address and undefined behaviour sanitizers since it parses files
tests/test_gsc_obj: tests/test_gsc_obj.c tests/fake_gsc.h source/gsc_obj.c source/gsc_obj.h
	$(CLANG) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_gsc_obj.c \
		source/gsc_obj.c -lz

# gsc_detours.c against a made-up game: script assets, the table of loaded scripts, a loader and linker, mapname
tests/test_gsc_detours: tests/test_gsc_detours.c tests/fake_gsc.h source/gsc_detours.c source/gsc_detours.h \
		source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_gsc_detours.c \
		source/gsc_obj.c

# script_vars.c against a made-up game that sets up, takes from and returns to the variable table as the game does
tests/test_script_vars: tests/test_script_vars.c source/script_vars.c source/script_vars.h source/offsets.h source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_script_vars.c

# font_resolution.c against a made-up game: an atlas, its allocator, a FreeType and a glyph function
tests/test_font_resolution: tests/test_font_resolution.c source/font_resolution.c source/font_resolution.h \
		source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h source/prepatch.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_font_resolution.c \
		source/gsc_obj.c

# lua_heap.c against a made-up heap that asks its callbacks as the game's dlmalloc does
tests/test_lua_heap: tests/test_lua_heap.c source/lua_heap.c source/lua_heap.h source/offsets.h source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_lua_heap.c

# lua_unref.c: which releases reach the game's luaL_unref
tests/test_lua_unref: tests/test_lua_unref.c source/lua_unref.c source/lua_unref.h source/offsets.h source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_lua_unref.c

# pmem_spill.c against a made-up PMem: stacks, address spaces and the game's own allocation
tests/test_pmem_spill: tests/test_pmem_spill.c source/pmem_spill.c source/pmem_spill.h source/offsets.h source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tests/test_pmem_spill.c

# aae_mantle.c: the mod's traversal rules, and its hooks against a made-up scan, dvars and per-mode dvar. Undefined
# behaviour stops the test: a division by zero is a crash in the game (x86_64) and nothing at all on an Apple CPU
tests/test_aae_mantle: tests/test_aae_mantle.c source/aae_mantle.c source/aae_mantle.h source/aae_plugin.c \
		source/aae_plugin.h source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h source/prepatch.h \
		source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_mantle.c source/gsc_obj.c

# detour.h: its thunks against two made-up game functions in assembly
tests/test_detour: tests/test_detour.c source/detour.h
	$(CLANG) -arch x86_64 -std=gnu11 -O1 -g -Wall -Wextra -Werror -o $@ tests/test_detour.c

# T7Overcharged's slide changes against a made-up game, with the cases its own slide push was compared on
# (tests/slide_cases.h)
tests/test_aae_move_slide: tests/test_aae_move_slide.c tests/slide_cases.h source/aae_move_slide.c \
		source/aae_move_slide.h source/aae_movement.h source/detour.h source/offsets.h source/dobby.h source/prepatch.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_move_slide.c
# and for x86_64, the game's own code, where the cases are compared with every NaN's bits
tests/test_aae_move_slide_x86: tests/test_aae_move_slide.c tests/slide_cases.h source/aae_move_slide.c \
		source/aae_move_slide.h source/aae_movement.h source/detour.h source/offsets.h source/dobby.h source/prepatch.h
	$(CLANG) -arch x86_64 -std=gnu11 -O1 -g -Wall -Wextra -Werror -o $@ tests/test_aae_move_slide.c

# aae_hostmig.c: the plugin's rules and the detour's and hooks' handlers against a made-up game
tests/test_aae_hostmig: tests/test_aae_hostmig.c source/aae_hostmig.c source/aae_hostmig.h source/aae_plugin.c \
		source/aae_plugin.h source/detour.h source/offsets.h source/dobby.h source/prepatch.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_hostmig.c

# aae_move_fastjump.c: T7Overcharged's rules and its table of times in the air, the hooks and detours against a
# made-up game, and the same cases as T7Overcharged's own code under Wine (tests/fastjump_cases.h)
FASTJUMP_TEST_FILES = tests/test_aae_move_fastjump.c tests/fastjump_cases.h source/aae_move_fastjump.c \
		source/aae_move_fastjump.h source/aae_movement.h source/detour.h source/offsets.h source/dobby.h source/prepatch.h
tests/test_aae_move_fastjump: $(FASTJUMP_TEST_FILES)
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_move_fastjump.c
tests/test_aae_move_fastjump_tsan: $(FASTJUMP_TEST_FILES)
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread -o $@ tests/test_aae_move_fastjump.c

# aae_movement.c: T7Overcharged's movement switches against a made-up game (the test includes the source)
tests/test_aae_movement: tests/test_aae_movement.c source/aae_movement.c source/aae_movement.h source/aae_plugin.h \
		source/gsc_obj.c source/gsc_obj.h source/offsets.h source/detour.h source/dobby.h source/prepatch.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_movement.c source/gsc_obj.c
# the same built for x86_64, the game's own: that build has the detours' thunks, and runs the perk test's setup
# (Rosetta on an Apple CPU, so without the sanitizers, like test_detour)
tests/test_aae_movement_x86_64: tests/test_aae_movement.c source/aae_movement.c source/aae_movement.h \
		source/aae_plugin.h source/gsc_obj.c source/gsc_obj.h source/offsets.h source/detour.h source/dobby.h \
		source/prepatch.h
	$(CLANG) -arch x86_64 -std=gnu11 -O1 -g -Wall -Wextra -Werror -o $@ tests/test_aae_movement.c source/gsc_obj.c

# aae_prompt.c and aae_plugin.c: the handlers against a made-up prompt element and made-up models
tests/test_aae_prompt: tests/test_aae_prompt.c tests/prompt_glide_frames.h source/aae_prompt.c source/aae_prompt.h \
		source/aae_plugin.c source/aae_plugin.h source/detour.h source/offsets.h source/dobby.h source/prepatch.h \
		source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_prompt.c

# aae_freeaim.c: the plugin's rule for free aim being on, and the crosshair test's hook against a made-up game
tests/test_aae_freeaim: tests/test_aae_freeaim.c tests/freeaim_cases.h tests/freeaim_endpos_cases.h tests/freeaim_endpos_side.h source/aae_freeaim.c source/aae_freeaim.h \
		source/aae_plugin.c source/aae_freeaim_net.c source/aae_freeaim_net.h \
		source/aae_plugin.h source/detour.h source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h \
		source/prepatch.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_freeaim.c source/gsc_obj.c source/aae_freeaim_net.c

# aae_breath.c: T7Overcharged's aim status and the hint's hook against a made-up game
tests/test_aae_breath: tests/test_aae_breath.c source/aae_breath.c source/aae_breath.h source/aae_plugin.c \
		source/aae_plugin.h source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_breath.c source/gsc_obj.c

# aae_move_sprint.c: T7Overcharged's rules for sprinting against its own (the numbers in tests/move_sprint_cases.h),
# and the handlers against a made-up game, through every value a compared register can hold
tests/test_aae_move_sprint: tests/test_aae_move_sprint.c tests/move_sprint_cases.h source/aae_move_sprint.c \
		source/aae_move_sprint.h source/aae_movement.h source/detour.h source/offsets.h source/dobby.h source/prepatch.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_move_sprint.c

# aae_clientfx.c: T7Overcharged's rules for its table of client effects, and the six hooks against a made-up cache
tests/test_aae_clientfx: tests/test_aae_clientfx.c source/aae_clientfx.c source/aae_clientfx.h source/gsc_obj.c \
		source/gsc_obj.h source/offsets.h source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_clientfx.c source/gsc_obj.c

# aae_freeaim_net.c: what a client sends the server, what the server keeps and looks up, against a made-up game and
# against what the plugin's own code gave for the same cases
tests/test_aae_freeaim_net: tests/test_aae_freeaim_net.c tests/freeaim_net_cases.h tests/freeaim_net_side.h \
		source/aae_freeaim_net.c source/aae_freeaim_net.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_freeaim_net.c

# aae_move_jump.c: T7Overcharged's jumpslowdown rules, its hooks and detours against a made-up game, two threads
# landing at once, and the cases T7Overcharged's own code gave under Wine
tests/test_aae_move_jump: tests/test_aae_move_jump.c tests/move_jump_cases.h tests/move_jump_side.h \
		source/aae_move_jump.c source/aae_move_jump.h source/aae_movement.h source/detour.h source/offsets.h \
		source/dobby.h source/prepatch.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_move_jump.c

# t7o_guards.c: the weapon attachment guard against a made-up weapon field lookup, the local client guard against a
# made-up controller lookup
tests/test_t7o_guards: tests/test_t7o_guards.c source/t7o_guards.c source/t7o_guards.h source/detour.h \
		source/offsets.h source/dobby.h source/prepatch.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_t7o_guards.c

# aae_linkcam.c: the plugin's rules for entities linked to the camera, and its hooks against a made-up game
tests/test_aae_linkcam: tests/test_aae_linkcam.c tests/linkcam_cases.h tests/linkcam_side.h source/aae_linkcam.c \
		source/aae_linkcam.h source/aae_plugin.c source/aae_plugin.h source/detour.h source/offsets.h source/dobby.h \
		source/prepatch.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_linkcam.c

# aae_coop_pause.c: the plugin's packets and rules against made-up games, a host and three clients over a made-up
# network that loses and reorders packets, and the hooks
tests/test_aae_coop_pause: tests/test_aae_coop_pause.c source/aae_coop_pause.c source/aae_coop_pause.h \
		source/aae_plugin.c source/aae_plugin.h source/detour.h source/gsc_obj.c source/gsc_obj.h source/offsets.h \
		source/dobby.h source/prepatch.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_coop_pause.c source/gsc_obj.c
# aae_viewmodel.c: T7Overcharged's reading of the dvars and the file, its rules, and the hook against a made-up game
tests/test_aae_viewmodel: tests/test_aae_viewmodel.c source/aae_viewmodel.c source/aae_viewmodel.h \
		source/aae_plugin.c source/aae_plugin.h source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h \
		source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_viewmodel.c source/gsc_obj.c

# clientfield_guard.c: its rules, and its hook against a made-up lookup, script engine and mod
tests/test_clientfield_guard: tests/test_clientfield_guard.c source/clientfield_guard.c source/clientfield_guard.h \
		source/offsets.h source/dobby.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_clientfield_guard.c

# aae_arc.c: T7Overcharged's arc test, the plugin's checks and sums, and the hooks against a made-up game
tests/test_aae_arc: tests/test_aae_arc.c tests/arc_bend_cases.h source/aae_arc.c source/aae_arc.h source/aae_skeleton.h \
		source/aae_plugin.c \
		source/aae_plugin.h source/detour.h source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h \
		source/prepatch.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_arc.c source/gsc_obj.c

# aae_vmwidget.c: the plugin's tables and sums, its two methods and the updates they give an element, against a
# made-up game and Lua stack
tests/test_aae_vmwidget: tests/test_aae_vmwidget.c source/aae_vmwidget.c source/aae_vmwidget.h source/aae_plugin.c \
		source/aae_plugin.h source/aae_skeleton.h source/havok.h source/offsets.h source/dobby.h source/prepatch.h \
		source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_vmwidget.c

# aae_vmwidget.c's bones written by two threads at once, in the order that left the plugin's count odd, and many at once
# (it uses only the bone writers of the piece it includes, so the rest may go unused)
tests/test_aae_vmwidget_tsan: tests/test_aae_vmwidget_tsan.c source/aae_vmwidget.c source/aae_vmwidget.h \
		source/aae_plugin.c source/aae_plugin.h source/aae_skeleton.h source/havok.h source/offsets.h source/dobby.h \
		source/prepatch.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=thread -o $@ \
		tests/test_aae_vmwidget_tsan.c

# aae_skeleton.c: the one hook on DObjCalcSkel, the order of the pieces' work and the two calls, against a made-up
# game; it finds out where the hook returns to through the frame pointers
tests/test_aae_skeleton: tests/test_aae_skeleton.c source/aae_skeleton.c source/aae_skeleton.h source/offsets.h \
		source/dobby.h
	$(CLANG) -std=gnu11 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Werror -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -o $@ tests/test_aae_skeleton.c

# aae_lan.c: the plugin's packets and rules, and the host's listener talking to the test over the loopback
tests/test_aae_lan: tests/test_aae_lan.c source/aae_lan.c source/aae_lan.h source/aae_plugin.c source/aae_plugin.h \
		source/detour.h source/gsc_obj.c source/gsc_obj.h source/offsets.h source/dobby.h source/prepatch.h \
		source/steam.h source/t7overcharged.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_aae_lan.c source/gsc_obj.c

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

# t7o_small.c: T7Overcharged's rules, and its game side against a made-up game (the test includes the source)
tests/test_t7o_small: tests/test_t7o_small.c source/t7o_small.c source/t7o_small.h source/offsets.h source/dobby.h \
		source/havok.h source/t7overcharged.h source/gsc_obj.c source/gsc_obj.h
	$(CLANG) -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-o $@ tests/test_t7o_small.c source/gsc_obj.c

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
