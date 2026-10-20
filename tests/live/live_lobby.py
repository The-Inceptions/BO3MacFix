# Live test of the zombies save/resume through the game's real menus.
# Backs up every user file, seeds BO3MacFix/checkpoint.ini (or starts without one), launches the game at the title
# screen (All-around Enhancement, no +map) with the test hooks (BO3MACFIX_TESTHOOKS=1; BO3MACFIX_OPENSETTINGS=1 opens
# the settings page by itself; BO3MACFIX_CHECKPOINT_REPORT logs what the match has so many seconds in) and prints
# BO3MacFix's lines while the tester drives the menus with keys.sh. Stops when DIR/stop_LABEL appears, the game exits
# or the time is up; then kills only its own game, keeps checkpoint.ini's last state, puts every file back (checked
# twice) and releases the game lock. Takes the lock itself; refuses to start while any CoDBlkOps3_Exe runs.
#
# python3 -I tests/live/live_lobby.py LABEL --out DIR --dylib PATH [--seed CHECKPOINT | --fresh] [--no-open]
#     [--give GUNS] [--perks PERKS] [--report SECONDS] [--minutes 13]
#   DIR (outside the repo) gets the backups, the log, the console log and checkpoint.ini's last state.
#   --fresh: no checkpoint.ini, so a quit save isn't held back by a deeper saved game.
#   --no-open: the page stays closed (it opening by itself over a menu can leave that menu ignoring keys).
#   --give/--perks: BO3MACFIX_CHECKPOINT_GIVE/_PERKS, a loadout given 12 s into a match.
#
# Menu path with AAE 3.9.5 (keys.sh codes: Return 36, Esc 53, Up 126, Down 125, F10 109):
#   title "Press ENTER to Start": Return -> AAE main menu (JOIN DISCORD focused): Down x2 (ZOMBIES), Return ->
#   PRIVATE GAME is focused: Return -> lobby (CAREERS focused): Up (SETUP GAME), Return -> CHANGE MAP: Return ->
#   Down (STANDARD), Return -> the current map is focused (Nacht der Untoten: Down x3 is Kino der Toten), Return ->
#   Esc closes the flyout -> Up (START GAME), Return -> "Choose Crew Member": Return -> the match loads.
#   In a match: Esc opens the start menu, F10 the settings page; on the page Down x60 is "Delete saved game" (the
#   last row: Down stops there, the page doesn't wrap around), Up 1 "Resume saved game", Up 2 "Save game". Count from
#   the end, not the top: the page has a row per setting plus the 3 saved game rows (49 since day 4's MenuFPS,
#   BackgroundFPS, MainWaitFix, ModWarningDialog and QuickSave), so the old Down x45 now stops on AutoSave, and a
#   Return there switches it in the user's settings.ini.
import argparse, os, re, shutil, signal, subprocess, sys, time

game = os.path.expanduser("~/Library/Application Support/Steam/steamapps/common/Call of Duty Black Ops III")
macos = game + "/CoDBlkOps3.app/Contents/MacOS"
profile = os.path.expanduser("~/Library/Application Support/Call of Duty Black Ops III")
trees = [game + "/players", profile + "/players"]
macfix = game + "/BO3MacFix"
files = [macfix + "/checkpoint.ini", macfix + "/displays.ini", macfix + "/settings.ini"]
checkpoint = macfix + "/checkpoint.ini"
mutations = game + "/players/AAECustomMutations_Default.cfg"
console = profile + "/console_mp.log"
lock = "/private/tmp/claude-501/bo3-game.lock"

ap = argparse.ArgumentParser()
ap.add_argument("label"); ap.add_argument("--out", required=True); ap.add_argument("--dylib", required=True)
ap.add_argument("--seed")
ap.add_argument("--minutes", type=float, default=13); ap.add_argument("--report", default="40")
ap.add_argument("--fresh", action="store_true"); ap.add_argument("--no-open", action="store_true")
ap.add_argument("--give"); ap.add_argument("--perks")
a = ap.parse_args()
here = os.path.abspath(a.out)
os.makedirs(here, exist_ok=True)

def running():
    return subprocess.run(["pgrep", "-l", "CoDBlkOps3_Exe"], capture_output=True, text=True).stdout.strip()

if running(): sys.exit("the game is already running: " + running())
try:
    os.mkdir(lock)
except FileExistsError:
    sys.exit("the game lock is taken: " + open(lock + "/owner").read().strip())
open(lock + "/owner", "w").write("resume-ui\n")
taken = time.time()

backup = os.path.join(here, "backup_" + a.label)
if os.path.exists(backup):
    shutil.rmtree(lock, ignore_errors=True)
    sys.exit("backup folder exists: " + backup)
os.makedirs(backup)
for i, tree in enumerate(trees):
    shutil.copytree(tree, os.path.join(backup, "tree%d" % i), symlinks=True)
existed = {}
for i, path in enumerate(files):
    existed[path] = os.path.exists(path)
    if existed[path]: shutil.copy2(path, os.path.join(backup, "file%d" % i))
print("backed up to", backup, flush=True)

def same(p, q):
    with open(p, "rb") as x, open(q, "rb") as y: return x.read() == y.read()

def restore():
    changed = []
    for i, tree in enumerate(trees):
        saved = os.path.join(backup, "tree%d" % i)
        for root, dirs, names in os.walk(tree):
            for name in names:
                now = os.path.join(root, name)
                old = os.path.join(saved, os.path.relpath(now, tree))
                if not os.path.exists(old):
                    os.remove(now); changed.append("removed " + now)
                elif not same(now, old):
                    shutil.copy2(old, now); changed.append("put back " + now)
        for root, dirs, names in os.walk(saved):
            for name in names:
                old = os.path.join(root, name)
                now = os.path.join(tree, os.path.relpath(old, saved))
                if not os.path.exists(now):
                    os.makedirs(os.path.dirname(now), exist_ok=True)
                    shutil.copy2(old, now); changed.append("put back missing " + now)
    for i, path in enumerate(files):
        old = os.path.join(backup, "file%d" % i)
        if existed[path]:
            if not os.path.exists(path) or not same(path, old):
                shutil.copy2(old, path); changed.append("put back " + path)
        elif os.path.exists(path):
            os.remove(path); changed.append("removed " + path)
    for name in os.listdir(macfix):
        if name.startswith("checkpoint.ini.tmp"):
            os.remove(os.path.join(macfix, name)); changed.append("removed " + name)
    for line in changed: print("  restore: " + line[-150:], flush=True)
    return changed

def setvar(text, name, value):
    eol = b"\r\n" if b"\r\n" in text else b"\n"
    line = b"modvar " + name + b" " + value
    if re.search(rb"(?im)^modvar\s+" + name + rb"\s.*$", text):
        return re.sub(rb"(?im)^modvar\s+" + name + rb"\s[^\r\n]*", line, text)
    return text + (b"" if text.endswith(b"\n") else eol) + line + eol

GREP = r"BO3MacFix|display tests|rror|arning|quit with status|HKAI_InitMapData|Could not load"
stopfile = os.path.join(here, "stop_" + a.label)
if os.path.exists(stopfile): os.remove(stopfile)
log = os.path.join(here, a.label + ".log")
awake = subprocess.Popen(["caffeinate", "-u", "-t", str(int(a.minutes * 60) + 120)])
p = None
try:
    cfg = open(mutations, "rb").read()
    # AAE's every-perk options off, so the perks the resume restores are the only ones; round 1 unless resumed
    cfg = setvar(cfg, b"tfoption_perkaholic", b"0")
    cfg = setvar(cfg, b"tfoption_spawn_with_quick_res", b"0")
    cfg = setvar(cfg, b"tfoption_starting_round", b"1")
    open(mutations, "wb").write(cfg)
    if a.seed:
        shutil.copy2(a.seed, checkpoint)
        print("seeded checkpoint.ini from", a.seed, flush=True)
    elif a.fresh and os.path.exists(checkpoint):
        os.remove(checkpoint)
        print("no checkpoint.ini for this run", flush=True)
    env = dict(os.environ, SteamAppId="311210", DYLD_INSERT_LIBRARIES=a.dylib, WINEPREFIX=game + "/BO3MacFix/pfx",
               BO3MACFIX_FRAMESTATS="0", BO3MACFIX_METALFXUPSCALE="0", BO3MACFIX_AUTOSAVE="0",
               BO3MACFIX_TESTHOOKS="1", BO3MACFIX_TESTTEXT=os.path.join(here, "cmd.txt"),
               BO3MACFIX_CHECKPOINT_REPORT=a.report)
    if not a.no_open: env["BO3MACFIX_OPENSETTINGS"] = "1"
    if a.give: env["BO3MACFIX_CHECKPOINT_GIVE"] = a.give
    if a.perks: env["BO3MACFIX_CHECKPOINT_PERKS"] = a.perks
    args = [macos + "/CoDBlkOps3_Exe.bo3macfix", "+set", "fs_game", "2631943123"]
    with open(log, "wb") as out:
        p = subprocess.Popen(args, cwd=macos, env=env, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    print("launched pid", p.pid, flush=True)
    start = time.time(); seen = 0; why = None
    while True:
        lines = open(log, "rb").read().decode(errors="replace").splitlines()
        for line in lines[seen:]:
            if re.search(GREP, line): print("[%5.0f s] %s" % (time.time() - start, line[:400]), flush=True)
        seen = len(lines)
        if p.poll() is not None: why = "the game exited by itself (%s)" % p.returncode; break
        if os.path.exists(stopfile): why = "stop file"; break
        if time.time() - taken > a.minutes * 60: why = "time's up"; break
        time.sleep(0.5)
    print("stopped:", why, flush=True)
finally:
    if p is not None and p.poll() is None:
        os.killpg(p.pid, signal.SIGKILL); p.wait()
    awake.terminate()
    if os.path.exists(console): shutil.copy2(console, os.path.join(here, a.label + ".console.log"))
    if os.path.exists(checkpoint): shutil.copy2(checkpoint, os.path.join(here, a.label + "_checkpoint.ini"))
    else: print("checkpoint.ini: none at the end", flush=True)
    time.sleep(1)
    changed = restore()
    left = restore()
    print("files put back: %d; differences left after: %d" % (len(changed), len(left)), flush=True)
    shutil.rmtree(lock, ignore_errors=True)
    print("lock released", flush=True)
