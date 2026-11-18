#!/bin/sh
# The launcher's StartWithMod (launcher/launcher.m with source/start_mod.c): what it puts on the game's command line
# and tells the game, seen through its dry run (BO3MACFIX_LAUNCHER_DRYRUN, which starts nothing and writes nothing in
# the game's folder), against made-up game, workshop and BO3MacFix folders in a temporary folder. Builds the launcher.
cd "$(dirname "$0")/.." || exit 1
make -C launcher >/dev/null || { echo "FAIL: the launcher doesn't build"; exit 1; }
launcher=$(pwd)/launcher/AppBundleExe
temp=${TMPDIR:-/tmp}
root=$(mktemp -d "${temp%/}/bo3mf_launcher_XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT
game="$root/steamapps/common/Call of Duty Black Ops III"
macos="$game/CoDBlkOps3.app/Contents/MacOS"
macfix="$game/BO3MacFix"
own="$root/own"
item="$root/steamapps/workshop/content/311210/2631943123"
download="$root/steamapps/workshop/downloads/311210/2631943123"
mkdir -p "$macos" "$macfix" "$own" "$item" || exit 1
printf 'TAff' > "$item/core_mod.ff"
failed=0

# the launcher's dry run with these settings (NAME=value ...) and then its arguments, "--" between them; StartWithMod's
# files in a folder of their own ($own) unless the settings say otherwise. Its output goes to $out. TEST_PRELOAD is a
# library for the launcher's process (wip/aae417/quietcrash.dylib for mutant runs: a shell drops DYLD_ variables, so
# it comes under another name).
run() {
    settings=""
    while [ "$1" != "--" ]; do settings="$settings $1"; shift; done
    shift
    # shellcheck disable=SC2086
    out=$(env -u BO3MACFIX_STARTWITHMOD -u BO3MACFIX_STARTWITHMOD_DIR -u BO3MACFIX_NOCODEPATCHES \
        -u BO3MACFIX_PREPATCHEDEXE -u BO3MACFIX_STARTWITHMOD_LAUNCH \
        ${TEST_PRELOAD:+DYLD_INSERT_LIBRARIES=$TEST_PRELOAD} \
        BO3MACFIX_LAUNCHER_DRYRUN="$macos" BO3MACFIX_STARTWITHMOD_DIR="$own" $settings "$launcher" "$@" 2>&1)
    status=$?
}
expect() {
    case "$out" in *"$1"*) ;; *) echo "FAIL ($2): expected \"$1\" in:"; echo "$out"; failed=1 ;; esac
}
forbid() {
    case "$out" in *"$1"*) echo "FAIL ($2): didn't expect \"$1\" in:"; echo "$out"; failed=1 ;; esac
}
file_is() {
    if [ "$(cat "$1" 2>/dev/null)" != "$2" ]; then
        echo "FAIL ($3): $1 holds \"$(cat "$1" 2>/dev/null)\", not \"$2\""
        failed=1
    fi
}
gone() {
    if [ -e "$1" ]; then echo "FAIL ($2): $1 is there"; failed=1; fi
}

# nothing remembered: the arguments as they came, the game told so
run -- "+set" "r_fullscreen" "1"
expect "StartWithMod: no mod was loaded at the last quit" "nothing"
expect "argument 1: $launcher
argument 2: +set
argument 3: r_fullscreen
argument 4: 1
environment: BO3MACFIX_STARTWITHMOD_LAUNCH=nothing " "nothing"
forbid "fs_game" "nothing"
[ "$status" = 0 ] || { echo "FAIL (nothing): the dry run ended with $status"; failed=1; }

# remembered and there: "+set fs_game <mod>" after the player's own arguments (the launcher's options taken out as
# before), the marker written
echo "2631943123" > "$own/lastmod.txt"
run -- "--playername" "Adem" "+set" "r_fullscreen" "1"
expect "StartWithMod: 2631943123: started with it (+set fs_game)" "given"
expect "argument 2: +set
argument 3: r_fullscreen
argument 4: 1
argument 5: +set
argument 6: fs_game
argument 7: 2631943123
environment: BO3MACFIX_STARTWITHMOD_LAUNCH=given 2631943123" "given"
forbid "Adem" "given"
file_is "$own/lastmod.pending" "2631943123" "given"
gone "$macfix/lastmod.pending" "given"

# that start never got to the menus (the game removes the marker there): this one is the stock game, the mod forgotten
run --
expect "StartWithMod: 2631943123: the last start with it didn't finish loading it" "failed"
expect "environment: BO3MACFIX_STARTWITHMOD_LAUNCH=failed 2631943123" "failed"
forbid "fs_game" "failed"
gone "$own/lastmod.pending" "failed"
gone "$own/lastmod.txt" "failed"

# off in settings.ini, on again at env
echo "2631943123" > "$own/lastmod.txt"
printf 'SkipIntro = 1\nStartWithMod = 0\n' > "$macfix/settings.ini"
run --
expect "StartWithMod: StartWithMod is off" "off"
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=off " "off"
forbid "fs_game" "off"
gone "$own/lastmod.pending" "off"
run BO3MACFIX_STARTWITHMOD=1 --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=given 2631943123" "on at env"
rm -f "$own/lastmod.pending" "$macfix/settings.ini"
run BO3MACFIX_STARTWITHMOD=0 --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=off " "off at env"

# NoCodePatches: nothing of the library runs, so no start with the mod either
printf 'NoCodePatches = 1\n' > "$macfix/settings.ini"
run --
expect "StartWithMod: off, since NoCodePatches is on" "NoCodePatches"
forbid "fs_game" "NoCodePatches"
rm -f "$macfix/settings.ini"

# settings.ini read as the library reads it, for every setting the launcher reads: past a UTF-8 byte order mark on
# the first line, in quotes, beyond an int (the library takes the most an int holds), the last line that sets it
printf '\357\273\277StartWithMod = 0\n' > "$macfix/settings.ini"
run --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=off " "byte order mark, StartWithMod"
printf '\357\273\277NoCodePatches = 1\n' > "$macfix/settings.ini"
run --
expect "StartWithMod: off, since NoCodePatches is on" "byte order mark, NoCodePatches"
expect "NoCodePatches is on, so the game's code stays as shipped" "byte order mark, NoCodePatches"
gone "$own/lastmod.pending" "byte order mark, NoCodePatches"
printf '\357\273\277PrePatchedExe = 0\nSkipIntro = 1\n' > "$macfix/settings.ini"
run --
expect "PrePatchedExe is off" "byte order mark, PrePatchedExe"
rm -f "$own/lastmod.pending"
printf "StartWithMod = '0'\n" > "$macfix/settings.ini"
run --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=off " "quoted"
printf 'StartWithMod = 4294967296\n' > "$macfix/settings.ini"
run --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=given 2631943123" "beyond an int"
rm -f "$own/lastmod.pending"
printf 'StartWithMod = 0\nStartWithMod = 1\n' > "$macfix/settings.ini"
run --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=given 2631943123" "the last line"
rm -f "$own/lastmod.pending" "$macfix/settings.ini"

# the player's own fs_game or map: nothing added
run -- "+set" "fs_game" "1234"
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=busy 2631943123" "own fs_game"
expect "argument 3: fs_game
argument 4: 1234
environment" "own fs_game"
run -- "+devmap zm_zod"
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=busy 2631943123" "a map"
gone "$own/lastmod.pending" "busy"

# the mod's files gone, or Steam downloading it
mv "$item/core_mod.ff" "$item/core_mod.ff.away"
run --
expect "StartWithMod: 2631943123: its files aren't there" "missing"
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=missing 2631943123" "missing"
mv "$item/core_mod.ff.away" "$item/core_mod.ff"
mkdir -p "$download"
run --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=updating 2631943123" "updating"
forbid "fs_game" "updating"
rmdir "$download"

# a dry run without a folder of its own writes nothing: the made-up BO3MacFix folder then is the game's own
echo "2631943123" > "$macfix/lastmod.txt"
run BO3MACFIX_STARTWITHMOD_DIR= --
expect "BO3MACFIX_STARTWITHMOD_LAUNCH=given 2631943123" "dry run"
expect "lastmod.txt in $macfix; a dry run, so nothing written)" "dry run"
gone "$macfix/lastmod.pending" "dry run"
gone "$macfix/launcher.log" "dry run"
expect "dry run: the pre-patched executable is neither checked nor made" "dry run"

# a real start that can't be made (no game in the made-up folder): no game ran, so the marker goes again
cp "$launcher" "$macos/AppBundleExe"
printf 'PrePatchedExe = 0\n' > "$macfix/settings.ini"
env -u BO3MACFIX_STARTWITHMOD -u BO3MACFIX_STARTWITHMOD_DIR -u BO3MACFIX_NOCODEPATCHES -u BO3MACFIX_PREPATCHEDEXE \
    -u BO3MACFIX_LAUNCHER_DRYRUN ${TEST_PRELOAD:+DYLD_INSERT_LIBRARIES=$TEST_PRELOAD} "$macos/AppBundleExe" \
    >/dev/null 2>&1
status=$?
out=$(cat "$macfix/launcher.log" 2>/dev/null)
[ "$status" = 1 ] || { echo "FAIL (no game): the launcher ended with $status"; failed=1; }
expect "StartWithMod: 2631943123: started with it (+set fs_game)" "no game"
expect "couldn't start CoDBlkOps3_Exe" "no game"
gone "$macfix/lastmod.pending" "no game"
file_is "$macfix/lastmod.txt" "2631943123" "no game"

[ $failed = 0 ] && echo "launcher StartWithMod: all passed"
exit $failed
