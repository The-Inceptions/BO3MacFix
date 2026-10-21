#!/usr/bin/env python3
# Every setting's default is written in three places, and they must agree:
#   - the code that reads it: read_int_setting("Name", default), read_int_setting_or_env, read_hex_setting
#     (bo3macfix.c), and the launcher's read_setting(path, "Name", default);
#   - its row on the settings page (source/settings.c), which the page shows while settings.ini has no line for it;
#   - the Default column of README.md's settings table.
# On 10-20 a find-and-replace left the page showing AAEBankFix and AudioFollowsDefault as Off while the code had them
# On (fixed in 1971d98); this catches that. A choice row's default must also be one of its values, so the page has a
# label for it (Off/30/40/60 shows PauseMenuFPS's 30 as "30", QuickSave's 96 as "F5").
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent

# Settings deliberately left out of a place, with the reason. Keep these short: anything else missing is a failure.
# No row on the settings page: set by hand in settings.ini only when AAE's build check fails, and README's second
# table gives an example rather than a default, since the default is computed: what aae_build.c reads from AAE's
# core_mod.ff.
NO_ROW = {
    "AAEChangelist": "file-only, default computed from AAE's core_mod.ff (0 = use it)",
    "AAEBuildTime": "file-only text, default computed from AAE's core_mod.ff (empty = use it)",
}
README_EXAMPLE_ONLY = set(NO_ROW)

C_STRING = r'"((?:[^"\\]|\\.)*)"'
failures = []


def fail(message):
    failures.append(message)


def number(text):
    try:
        return int(text.strip(), 0)
    except ValueError:
        return None


# --- the code: every read of a setting by name, with its default --------------------------------------------------
code = {}  # name -> [(where, default)], default None for text settings
read_call = re.compile(r'\bread_(int_|hex_)?setting(_or_env)?\(\s*(?:[A-Za-z_]\w*\s*,\s*)?"(\w+)"\s*,\s*([^,()]+)\)')
string_call = re.compile(r'\bread_string_setting(?:_or_env)?\(\s*"(\w+)"')
direct_call = re.compile(r'\bsettings_ini_read\w*\(\s*[^,()]+,\s*"(\w+)"')
for path in sorted(list(root.glob("source/*.[cm]")) + list(root.glob("launcher/*.[cm]"))):
    text = path.read_text(errors="replace")
    for match in read_call.finditer(text):
        where = f"{path.relative_to(root)}:{text.count(chr(10), 0, match.start()) + 1}"
        value = number(match.group(4))
        if value is None:
            fail(f"{match.group(3)}: {where} reads it with {match.group(4).strip()!r}, not a plain number; if the "
                 f"default is computed, list it here with the reason")
        code.setdefault(match.group(3), []).append((where, value))
    for match in string_call.finditer(text):
        code.setdefault(match.group(1), []).append((str(path.relative_to(root)), None))
    for match in direct_call.finditer(text):
        fail(f"{match.group(1)}: {path.relative_to(root)} reads it with settings_ini_read directly; use the "
             f"read_*_setting helpers so this test sees its default")

# --- the settings page's rows ---------------------------------------------------------------------------------------
rows = {}  # name -> (default, values or None for a toggle, labels)
settings_c = (root / "source/settings.c").read_text()
row_line = re.compile(r'^    \{ ' + C_STRING + r', ' + C_STRING + r', ' + C_STRING + r', ' + C_STRING + r', (.*) \},\s*$',
                      re.M)
toggle = re.compile(r'^Toggle(?:_Live)?\(\s*(-?\w+)\s*(?:,\s*\w+\s*)?\)$')
choice = re.compile(r'^(true|false), (\d+), \{ ([^}]*) \}, \{ ((?:\s*' + C_STRING + r'\s*,?)*) \}, (-?\w+), (\w+)$')
for match in row_line.finditer(settings_c):
    name, rest = match.group(1), match.group(5)
    if name in rows:
        fail(f"{name}: two rows in source/settings.c")
    if (m := toggle.match(rest)) is not None:
        rows[name] = (number(m.group(1)), None, None)
    elif (m := choice.match(rest)) is not None:
        values = [number(v) for v in m.group(3).split(",")]
        labels = re.findall(C_STRING, m.group(4))
        if not len(values) == len(labels) == int(m.group(2)):
            fail(f"{name}: its row has {m.group(2)} choices, {len(values)} values and {len(labels)} labels")
        rows[name] = (number(m.group(6)), values, labels)
    else:
        fail(f"{name}: can't read its row in source/settings.c: {rest}")
if len(rows) < 40:
    fail(f"found only {len(rows)} rows in source/settings.c; has the row format changed?")

# --- README.md's settings table -------------------------------------------------------------------------------------
readme = {}  # name -> default text
readme_examples = set()
table = None
for line in (root / "README.md").read_text().splitlines():
    cells = [c.strip() for c in line.strip().strip("|").split("|")] if line.startswith("|") else None
    if cells is None:
        table = None
    elif cells[:2] in (["Setting", "Default"], ["Setting", "Example"]):
        table = cells[1]
    elif table is not None and (m := re.fullmatch(r"`(\w+)`", cells[0])) is not None:
        if table == "Default":
            if m.group(1) in readme:
                fail(f"{m.group(1)}: two rows in README.md's settings table")
            readme[m.group(1)] = cells[1]
        else:
            readme_examples.add(m.group(1))
if len(readme) < 40:
    fail(f"found only {len(readme)} rows in README.md's settings table; has its header changed?")

# --- the three agree ------------------------------------------------------------------------------------------------
for name in sorted(set(code) | set(rows) | set(readme) | readme_examples | set(NO_ROW)):
    reads = code.get(name, [])
    if not reads:
        fail(f"{name}: nothing in source/ or launcher/ reads it (renamed?)")
    defaults = {value for _, value in reads}
    if len(defaults) > 1:
        fail(f"{name}: the code reads it with different defaults: "
             + ", ".join(f"{where} {value}" for where, value in reads))
    code_default = reads[0][1] if reads else None

    if name in NO_ROW:
        if name in rows:
            fail(f"{name}: has a row in source/settings.c but is listed in NO_ROW")
    elif name not in rows:
        fail(f"{name}: no row in source/settings.c; add one, or list it in NO_ROW with the reason")
    else:
        default, values, labels = rows[name]
        if values is None:
            if default not in (0, 1):
                fail(f"{name}: its toggle row's default is {default}, not 0 or 1")
        elif default not in values:
            fail(f"{name}: its row's default {default} isn't one of its values {values}, so the page has no label")
        if len(defaults) == 1 and default != code_default:
            if values is None:
                shown = "On" if default else "Off"
            else:
                shown = labels[values.index(default)] if default in values else str(default)
            fail(f"{name}: the page shows {shown!r} ({default}) by default, the code defaults to {code_default} "
                 f"({reads[0][0]})")

    if name in README_EXAMPLE_ONLY:
        if name in readme:
            fail(f"{name}: has a default in README.md but is listed in README_EXAMPLE_ONLY")
        if name not in readme_examples:
            fail(f"{name}: README.md's table of settings set by hand doesn't list it")
    elif name not in readme:
        fail(f"{name}: no row in README.md's settings table")
    elif len(defaults) == 1 and number(readme[name]) != code_default:
        fail(f"{name}: README.md says the default is {readme[name]}, the code defaults to {code_default} "
             f"({reads[0][0]})")

for message in failures:
    print("FAIL: " + message)
if failures:
    sys.exit(1)
print(f"settings defaults: {len(rows)} settings agree in the code, source/settings.c and README.md "
      f"({len(NO_ROW)} file-only: {', '.join(sorted(NO_ROW))})")
