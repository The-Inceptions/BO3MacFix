// (A copy of wip/aae417/quietcrash.c, so that make fuzz and make sanitize build it themselves: tests/quietcrash.dylib.)
// quietcrash.dylib: put into a test program with DYLD_INSERT_LIBRARIES so that a fatal signal ends it with a plain
// exit status (128 + the signal's number, as a shell would say it) instead of the way that makes macOS write a crash
// report into ~/Library/Logs/DiagnosticReports. For unit tests, mutant runs and fuzzers, which crash on purpose and
// often: on 2026-11-18 a morning of them left some twenty reports in the user's folder, four under the game's name.
// A sanitizer build needs ASAN_OPTIONS=abort_on_error=0 (and UBSAN_OPTIONS=abort_on_error=0) as well: its own abort
// would otherwise come first. The handler writes one line to stderr with async-signal-safe calls only.
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void quiet_fatal(int sig) {
    static const char text[] = "quietcrash: the program died of signal ";
    char line[80];
    size_t n = sizeof text - 1;
    memcpy(line, text, n);
    if (sig >= 10) line[n++] = (char)('0' + sig / 10);
    line[n++] = (char)('0' + sig % 10);
    line[n++] = '\n';
    (void)write(STDERR_FILENO, line, n);
    _exit(128 + sig);
}

__attribute__((constructor)) static void quietcrash_init(void) {
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS };
    static char stack[65536];  // its own stack, so that a stack overflow is caught too
    stack_t alt = { .ss_sp = stack, .ss_size = sizeof stack, .ss_flags = 0 };
    sigaltstack(&alt, NULL);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = quiet_fatal;
    action.sa_flags = SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        sigaction(signals[i], &action, NULL);
}
