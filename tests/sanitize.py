#!/usr/bin/env python3
# Builds every C and Objective-C test again with the address and undefined behaviour sanitizers, stopping at the
# first fault, and runs it: `make sanitize` (both architectures), `make sanitize SANITIZE_ARCHS=arm64`,
# `make sanitize SANITIZE_ONLY='lan$'` (a regular expression; one that matches no test is an error).
#
# Each test's own compile command comes from `make -n -B <test>`, so a test added to the Makefile is taken along
# without a change here. The command is changed only in its sanitizer flags, -Werror (warnings that only these flags
# bring must not stop a build) and its output, which goes to tests/san/<arch>/. A test whose rule builds it for one
# architecture only (-arch x86_64: the detours' thunks) is built for that one. Tests under the thread sanitizer are
# left to `make test`, which runs them already.
#
# x86_64 is the library's own architecture: on an Apple CPU those builds run under Rosetta, sanitizers included.
# Output: tests/san/<arch>/<test>.log for each, a line per test and a summary here; the exit status is 1 when a test
# failed to build or failed under the sanitizers.
import os
import re
import shlex
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
# A test that crashes ends with exit status 128 + the signal and leaves no crash report in the user's
# ~/Library/Logs/DiagnosticReports: tests/quietcrash.dylib (make sanitize builds it), and the sanitizers exit instead
# of aborting. Python starts the tests itself, so the DYLD_ variable reaches them (macOS drops it only for its own
# protected programs).
QUIETCRASH = os.path.join(ROOT, "tests", "quietcrash.dylib")
RUN_ENV = dict(os.environ,
               DYLD_INSERT_LIBRARIES=QUIETCRASH,
               ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_stack_use_after_return=1:strict_string_checks=1:"
                            "detect_invalid_pointer_pairs=0:check_initialization_order=1",
               UBSAN_OPTIONS="print_stacktrace=1:abort_on_error=0:halt_on_error=1")


def prerequisites():
    """Each test's prerequisites, from make's database."""
    out = subprocess.run(["make", "-p", "-q", "-r", "all"], cwd=ROOT, capture_output=True, text=True).stdout
    found = {}
    for line in out.splitlines():
        if line.startswith("tests/test_") and ":" in line and not line.startswith("#"):
            target, _, rest = line.partition(":")
            if " " not in target.strip():
                found.setdefault(target.strip(), set()).update(rest.split())
    return found


def compile_command(test):
    """The commands make runs to build test, as argument lists (the last one that writes -o test)."""
    out = subprocess.run(["make", "-n", "-B", test], cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        return None, out.stderr
    text = out.stdout.replace("\\\n", " ")
    for line in text.splitlines():
        words = shlex.split(line)
        if "-o" in words and words[words.index("-o") + 1] == test:
            return words, ""
    return None, "no compile command for %s in make -n" % test


def sanitized(words, arch, out_path, needs):
    """The command with the sanitizers, for arch, writing out_path; None when the test isn't for this."""
    if any(w.startswith("-fsanitize=thread") for w in words):
        return None
    # detour.h records its places into the tests' own tables on any CPU but x86_64, where it makes the real thunks:
    # a test with detours runs on x86_64 only in the variant its rule builds for x86_64 alone
    if arch == "x86_64" and "-arch" not in words and "source/detour.h" in needs:
        return None
    fixed_arch = None
    result = []
    skip = False
    for i, w in enumerate(words):
        if skip:
            skip = False
            continue
        if w == "-arch":
            fixed_arch = words[i + 1]
            skip = True
            continue
        if w.startswith("-fsanitize") or w.startswith("-fno-sanitize") or w == "-Werror":
            continue
        if w == "-o":
            result += ["-o", out_path]
            skip = True
            continue
        result.append(w)
    if fixed_arch is not None and fixed_arch != arch:
        return None
    # the compiler, then the architecture and the sanitizers
    return [result[0], "-arch", arch] + SANITIZE + result[1:]


def main(arguments):
    if not os.path.exists(QUIETCRASH):
        print("sanitize: %s isn't there (make sanitize builds it): a test that crashes would leave a crash report"
              % QUIETCRASH)
        return 1
    archs = os.environ.get("SANITIZE_ARCHS", "arm64 x86_64").split()
    tests = [t for argument in arguments for t in argument.split() if t.startswith("tests/")]
    wanted = os.environ.get("SANITIZE_ONLY")
    if wanted:
        tests = [t for t in tests if re.search(wanted, t)]
        # an expression that takes nothing is a mistake, not a pass
        if not tests:
            print("sanitize: SANITIZE_ONLY=%s matches no test" % wanted)
            return 1
    failures = []
    count = 0
    needs = prerequisites()
    for arch in archs:
        out_dir = os.path.join("tests", "san", arch)
        os.makedirs(os.path.join(ROOT, out_dir), exist_ok=True)
        for test in tests:
            words, error = compile_command(test)
            if words is None:
                failures.append((arch, test, "no build command: " + error.strip()))
                print("%-7s %-45s NO COMMAND" % (arch, test), flush=True)
                continue
            out_path = os.path.join(out_dir, os.path.basename(test))
            command = sanitized(words, arch, out_path, needs.get(test, set()))
            if command is None:
                continue
            log_path = os.path.join(ROOT, out_path + ".log")
            started = time.time()
            with open(log_path, "w") as log:
                built = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                if built.returncode != 0:
                    failures.append((arch, test, "build failed, see " + out_path + ".log"))
                    print("%-7s %-45s BUILD FAILED" % (arch, test), flush=True)
                    continue
                log.write("\n---- run\n")
                log.flush()
                try:
                    ran = subprocess.run(["./" + out_path], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                         env=RUN_ENV, timeout=float(os.environ.get("SANITIZE_TIMEOUT", "900")))
                    code = ran.returncode
                except subprocess.TimeoutExpired:
                    code = "timeout"
            count += 1
            took = time.time() - started
            if code != 0:
                failures.append((arch, test, "exit %s, see %s.log" % (code, out_path)))
            print("%-7s %-45s %s (%.0f s)" % (arch, test, "ok" if code == 0 else "FAILED: exit %s" % code, took),
                  flush=True)
    print("\nsanitize: %d test runs, %d failed" % (count, len(failures)))
    for arch, test, what in failures:
        print("  %s %s: %s" % (arch, test, what))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
