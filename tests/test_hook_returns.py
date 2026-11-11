#!/usr/bin/env python3
# The hks_load_dll hook must return an int, never a bool.
#
# The game's package.loadlib ends by jumping to hks_load_dll, so that function's return value is loadlib's number of
# results: 1 for the function it pushed, 0 for none. A hook declared bool compiles to "mov al, 1", which leaves the
# upper bytes of eax as the last call had them. That passed by luck until All-around Enhancement 4.1.7 (2026-11-09):
# loadlib then gave the mod's Lua nothing back, the mod took T7Overcharged as missing and quit the game.
import pathlib
import re
import sys

source = (pathlib.Path(__file__).resolve().parent.parent / "source" / "bo3macfix.c").read_text()
match = re.search(r"install_hook_name\(hks_load_dll,\s*([A-Za-z_0-9 ]+?)\s*,", source)
if match is None:
    sys.exit("test_hook_returns.py: no hks_load_dll hook found in source/bo3macfix.c")
failures = []
if match.group(1) != "int":
    failures.append("the hks_load_dll hook returns '%s', it must return int" % match.group(1))
body = source[match.end():]
body = body[:body.index("\n}\n")]
for constant in re.findall(r"return\s+(true|false)\s*;", body):
    failures.append("the hks_load_dll hook has 'return %s;', use 1 or 0" % constant)
if failures:
    sys.exit("test_hook_returns.py: " + "; ".join(failures))
print("test_hook_returns.py: all passed")
