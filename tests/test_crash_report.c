// crash_report.c: the summary of a made-up report line by line; the real reports of the game on this Mac (the two of
// the user's crashes of 2026-11-13/14 in wip/night/evidence when the repository has them, and whatever lies in the two
// DiagnosticReports folders, only read); every one of them cut at every length; random bytes, random JSON-like text
// and the made-up report with bytes changed; nesting past the limit; values of the wrong kind; tiny output buffers;
// and the file reader with a pipe, a link, an empty file, a huge one and one it may not read. Built with the address
// and undefined behaviour sanitizers, so a read past the end of a report is a failure.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../source/crash_report.h"

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char summary[16384];

// summarises a copy of text in a buffer of exactly length bytes, so the sanitizer sees any read past the end
static crash_report_result_t summarise_for(const char *text, size_t length, int pid, const char *const *executables) {
    char *copy = malloc(length > 0 ? length : 1);
    memcpy(copy, text, length);
    crash_report_result_t result = crash_report_summary(copy, length, pid, executables, summary, sizeof(summary));
    free(copy);
    return result;
}

static crash_report_result_t summarise(const char *text, size_t length, int pid) {
    return summarise_for(text, length, pid, NULL);
}

// the game where Steam puts it, for someone; the report says /Users/USER and a "*" for steamapps/common/...
#define Game_Folder "/Users/someone/Library/Application Support/Steam/steamapps/common/Call of Duty Black Ops III/" \
                    "CoDBlkOps3.app/Contents/MacOS"
static const char *const the_game[] = { Game_Folder "/CoDBlkOps3_Exe", Game_Folder "/CoDBlkOps3_Exe.bo3macfix", NULL };

static bool has(const char *needle) {
    return strstr(summary, needle) != NULL;
}

// ---- a made-up report with the shape of macOS's: the game's main thread crashed in the game, the render thread in
// BO3MacFix, a frame without a symbol in a library, one in no image at all, and the process's "main" thread
static const char made_up[] =
    "{\"app_name\":\"CoDBlkOps3_Exe.bo3macfix\",\"timestamp\":\"2026-11-14 01:42:16.00 -0800\",\"bug_type\":\"309\","
    "\"os_version\":\"macOS 27.2.1 (26B131)\",\"name\":\"CoDBlkOps3_Exe.bo3macfix\"}\n"
    "{\n"
    "  \"uptime\" : 45000,\n"
    "  \"captureTime\" : \"2026-11-14 01:42:13.2480 -0800\",\n"
    "  \"pid\" : 97846,\n"
    "  \"procLaunch\" : \"2026-11-14 01:41:36.3023 -0800\",\n"
    "  \"procName\" : \"CoDBlkOps3_Exe.bo3macfix\",\n"
    "  \"procPath\" : \"\\/Users\\/USER\\/Library\\/Application Support\\/Steam\\/*\\/CoDBlkOps3.app\\/Contents\\/MacOS\\/"
    "CoDBlkOps3_Exe.bo3macfix\",\n"
    "  \"exception\" : {\"codes\":\"0x0000000000000001, 0x0000000000000278\",\"rawCodes\":[1,632],"
    "\"type\":\"EXC_BAD_ACCESS\",\"signal\":\"SIGSEGV\",\"subtype\":\"KERN_INVALID_ADDRESS at 0x0000000000000278\"},\n"
    "  \"termination\" : {\"flags\":0,\"code\":11,\"namespace\":\"SIGNAL\",\"indicator\":\"Segmentation fault: 11\"},\n"
    "  \"asi\" : {\"libsystem_c.dylib\":[\"abort() called\",\"line \\\"two\\\"\\u0041\\n\"]},\n"
    "  \"faultingThread\" : 2,\n"
    "  \"threads\" : [\n"
    "    {\"id\":1,\"queue\":\"com.apple.main-thread\",\"name\":\"main\",\"frames\":[{\"imageOffset\":17083,"
    "\"symbol\":\"__psynch_cvwait\",\"symbolLocation\":11,\"imageIndex\":3}]},\n"
    "    {\"id\":2,\"name\":\"Win (WinID 12)\",\"frames\":[{\"imageOffset\":299006,\"imageIndex\":1},"
    "{\"imageOffset\":299150,\"symbol\":\"listener_main\",\"symbolLocation\":14,\"imageIndex\":1},"
    "{\"imageOffset\":529400,\"imageIndex\":2},{\"imageOffset\":140705911565184,\"imageIndex\":4}]},\n"
    "    {\"triggered\":true,\"id\":3,\"name\":\"Main\",\"threadState\":{\"flavor\":\"x86_THREAD_STATE\","
    "\"rip\":{\"value\":4312458481,\"matchesCrashFrame\":0},\"rax\":{\"value\":63},"
    "\"rosetta\":{\"tmp0\":{\"value\":18446744073709551615}}},\n"
    "     \"frames\":[{\"imageOffset\":4760845176,\"imageIndex\":4},{\"imageOffset\":17475803,\"imageIndex\":0},"
    "{\"imageOffset\":18827034,\"imageIndex\":0},{\"imageOffset\":23195,\"symbol\":\"_pthread_start\","
    "\"symbolLocation\":115,\"imageIndex\":3}]}\n"
    "  ],\n"
    "  \"usedImages\" : [\n"
    "    {\"source\":\"P\",\"arch\":\"x86_64\",\"base\":4294967296,\"size\":26628096,\"name\":\"CoDBlkOps3_Exe.bo3macfix\"},\n"
    "    {\"source\":\"P\",\"arch\":\"x86_64\",\"base\":4916940800,\"size\":569344,\"name\":\"BO3MacFix.dylib\"},\n"
    "    {\"source\":\"P\",\"arch\":\"x86_64\",\"base\":140703540000000,\"name\":\"libsystem_c.dylib\"},\n"
    "    {\"source\":\"P\",\"arch\":\"x86_64\",\"base\":140703541000000,\"name\":\"libsystem_pthread.dylib\"},\n"
    "    {\"size\":0,\"source\":\"A\",\"base\":0,\"uuid\":\"00000000-0000-0000-0000-000000000000\"}\n"
    "  ],\n"
    "  \"vmSummary\" : \"ReadOnly portion of Libraries: Total=1.6G\\nWritable regions: Total=8.1G\"\n"
    "}\n";

static const char made_up_summary[] =
    "process: CoDBlkOps3_Exe.bo3macfix, pid 97846, started 2026-11-14 01:41:36.3023 -0800, crashed 2026-11-14 "
    "01:42:13.2480 -0800\n"
    "exception: EXC_BAD_ACCESS, SIGSEGV, KERN_INVALID_ADDRESS at 0x0000000000000278\n"
    "termination: Segmentation fault: 11\n"
    "asi: libsystem_c.dylib: abort() called\n"
    "asi: libsystem_c.dylib: line \"two\"A \n"
    "crashed thread 2 \"Main\", rip 0x1010ae4f1\n"
    "   0  ? +0x11bc4bb78\n"
    "   1  game 0x1010aa8db\n"
    "   2  game 0x1011f471a\n"
    "   3  libsystem_pthread.dylib _pthread_start+115\n";

static void test_made_up(void) {
    CHECK(summarise(made_up, sizeof(made_up) - 1, 0) == Crash_Report_Summarised);
    CHECK(strcmp(summary, made_up_summary) == 0);
    if (strcmp(summary, made_up_summary) != 0)
        printf("got:\n%s\n", summary);
    // the pid it must be of
    CHECK(summarise(made_up, sizeof(made_up) - 1, 97846) == Crash_Report_Summarised);
    CHECK(strcmp(summary, made_up_summary) == 0);
    CHECK(summarise(made_up, sizeof(made_up) - 1, 97845) == Crash_Report_Other_Process);
    CHECK(has("another process (pid 97846)"));
    // the program it must be of: the game's, in the game's folder
    CHECK(summarise_for(made_up, sizeof(made_up) - 1, 97846, the_game) == Crash_Report_Summarised);
    CHECK(strcmp(summary, made_up_summary) == 0);
    const char *const stand_in[] = { "/private/tmp/stand-in/CoDBlkOps3_Exe.bo3macfix", NULL };
    CHECK(summarise_for(made_up, sizeof(made_up) - 1, 97846, stand_in) == Crash_Report_Other_Process);
    CHECK(has("a program elsewhere (/Users/USER/Library/Application Support/Steam/*/CoDBlkOps3.app/Contents/MacOS/"
              "CoDBlkOps3_Exe.bo3macfix)"));
    const char *const none[] = { NULL };
    CHECK(summarise_for(made_up, sizeof(made_up) - 1, 0, none) == Crash_Report_Other_Process);

    // another thread crashed: the game's "Main" thread follows, 8 frames at most, and not the process's "main"
    char other[sizeof(made_up) + 64];
    snprintf(other, sizeof(other), "%s", made_up);
    char *faulting = strstr(other, "\"faultingThread\" : 2");
    faulting[strlen("\"faultingThread\" : ")] = '1';
    CHECK(summarise(other, strlen(other), 0) == Crash_Report_Summarised);
    CHECK(has("crashed thread 1 \"Win (WinID 12)\", no rip in the report\n"
              "   0  BO3MacFix+0x48ffe\n"
              "   1  BO3MacFix+0x4908e listener_main\n"
              "   2  libsystem_c.dylib +0x813f8\n"
              "   3  ? +0x7ff8a5e06b80\n"
              "the game's thread 2 \"Main\":\n"
              "   0  ? +0x11bc4bb78\n"));
    CHECK(!has("__psynch_cvwait"));
    // only the first thread called "Main"
    const char *two_mains = "{}\n{\"exception\":{},\"faultingThread\":0,\"threads\":[{\"name\":\"Win\"},"
                            "{\"name\":\"Main\"},{\"name\":\"Main\"}]}";
    CHECK(summarise(two_mains, strlen(two_mains), 0) == Crash_Report_Summarised);
    CHECK(has("the game's thread 1 \"Main\":") && !has("the game's thread 2"));

    // without faultingThread, the thread marked triggered
    snprintf(other, sizeof(other), "%s", made_up);
    faulting = strstr(other, "\"faultingThread\"");
    memcpy(faulting, "\"faultingThreadX\"", 17);
    CHECK(summarise(other, strlen(other), 0) == Crash_Report_Summarised);
    CHECK(has("crashed thread 2 \"Main\", rip 0x1010ae4f1\n"));

    // a body with trailing spaces and without its last line break is still whole
    CHECK(summarise(made_up, sizeof(made_up) - 2, 0) == Crash_Report_Summarised);
}

// ---- frames: 16 of the crashed thread, 8 of Main
static void test_frame_counts(void) {
    static char text[65536];
    int n = snprintf(text, sizeof(text), "{\"bug_type\":\"309\"}\n{\"pid\":5,\"exception\":{\"type\":\"EXC_CRASH\"},"
                                         "\"faultingThread\":0,\"usedImages\":[{\"name\":\"CoDBlkOps3_Exe\"}],"
                                         "\"threads\":[{\"name\":\"Win\",\"frames\":[");
    for (int i = 0; i < 40; i++)
        n += snprintf(text + n, sizeof(text) - (size_t)n, "%s{\"imageOffset\":%d,\"imageIndex\":0}", i ? "," : "",
                      0x1000 + i);
    n += snprintf(text + n, sizeof(text) - (size_t)n, "]},{\"name\":\"Main\",\"frames\":[");
    for (int i = 0; i < 40; i++)
        n += snprintf(text + n, sizeof(text) - (size_t)n, "%s{\"imageOffset\":%d,\"imageIndex\":0}", i ? "," : "",
                      0x2000 + i);
    n += snprintf(text + n, sizeof(text) - (size_t)n, "]}]}");
    CHECK(summarise(text, (size_t)n, 5) == Crash_Report_Summarised);
    CHECK(has("  15  game 0x10000100f\n"));
    CHECK(!has("game 0x100001010"));
    CHECK(has("   7  game 0x100002007\n"));
    CHECK(!has("game 0x100002008"));
    CHECK(has("crashed thread 0 \"Win\", no rip in the report"));
}

// ---- values of the wrong kind: the summary goes on without them, never past the text
static void test_wrong_kinds(void) {
    const char *bodies[] = {
        "{\"exception\":{},\"threads\":[]}",
        "{\"exception\":{\"type\":5,\"signal\":[1],\"subtype\":{\"a\":1}},\"threads\":[1,\"x\",null,{}]}",
        "{\"exception\":{\"type\":\"\"},\"threads\":[{}],\"faultingThread\":0}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{}],\"faultingThread\":\"0\"}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"frames\":{}}],\"faultingThread\":-1}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"frames\":[1,{},[]]}],\"faultingThread\":0}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"frames\":[{\"imageIndex\":-1,\"imageOffset\":-5}]}],"
        "\"faultingThread\":0,\"usedImages\":[]}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"frames\":[{\"imageIndex\":99999999999999999999,"
        "\"imageOffset\":18446744073709551616}]}],\"faultingThread\":0}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"name\":\"first\"}],\"faultingThread\":18446744073709551616}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"frames\":[{\"imageIndex\":1.5,\"imageOffset\":1e3}]}],"
        "\"faultingThread\":1e2,\"usedImages\":[{\"name\":7}]}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"frames\":[{\"imageIndex\":0,\"imageOffset\":4294967296}]}],"
        "\"faultingThread\":0,\"usedImages\":[{\"name\":\"CoDBlkOps3_Exe\"}]}",
        "{\"exception\":{\"type\":\"E\\u0000\\u00e9\\ud83d\\\\x\"},\"threads\":[{\"name\":\"\\u0007\",\"threadState\":"
        "{\"rip\":{\"value\":-1}}}],\"faultingThread\":0,\"pid\":-9223372036854775808}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"threadState\":{\"rip\":7}}],\"faultingThread\":0,"
        "\"asi\":{\"a\":\"not an array\",\"b\":[1,2],\"c\":[\"x\",\"y\",\"z\",\"w\",\"v\"]},\"pid\":1e9}",
        "{\"exception\":{\"type\":\"E\"},\"threads\":[{\"triggered\":\"true\"},{\"triggered\":true,\"name\":\"T\"}]}",
    };
    const char *expected[] = {
        "exception: (none given)\ncrashed thread: not in the report\n",
        "exception: (none given)\n",
        "exception: (none given)\ncrashed thread 0, no rip in the report\n  (no frames)\n",
        "crashed thread: not in the report\n",
        "crashed thread: not in the report\n",
        "   0  ?\n   1  ?\n   2  ?\n",
        "   0  ?\n",
        "   0  ?\n",
        "crashed thread: not in the report\n",
        "crashed thread: not in the report\n",
        "   0  CoDBlkOps3_Exe +0x100000000\n",
        ("pid -9223372036854775808, started ?, crashed ?\nexception: E\?\?\?\\x\ncrashed thread 0 \"\?\", no rip in the "
         "report\n"),
        "asi: c: x\nasi: c: y\nasi: c: z\nasi: c: w\ncrashed thread 0, no rip in the report\n",
        "crashed thread 1 \"T\", no rip in the report\n",
    };
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        char text[2048];
        int n = snprintf(text, sizeof(text), "{}\n%s", bodies[i]);
        CHECK(summarise(text, (size_t)n, 0) == Crash_Report_Summarised);
        if (!has(expected[i])) {
            printf("FAIL %s:%d: wrong kinds %zu: expected \"%s\" in:\n%s\n", __FILE__, __LINE__, i, expected[i],
                   summary);
            failures++;
        }
    }
    // a key that only starts like the one looked for isn't it
    const char *prefix = "{}\n{\"pidx\":1,\"pid\":2,\"exception\":{\"typeX\":\"no\",\"type\":\"yes\"},\"threads\":[]}";
    CHECK(summarise(prefix, strlen(prefix), 2) == Crash_Report_Summarised && has("exception: yes\n"));
    // bytes past ASCII in a name are shown as '?' (only a path keeps them)
    const char *raw = "{}\n{\"exception\":{\"type\":\"E\xc3\xa9\x7f\"},\"threads\":[]}";
    CHECK(summarise(raw, strlen(raw), 0) == Crash_Report_Summarised && has("exception: E\?\?\?\n"));
    // the pid must be a whole number to count
    const char *fraction = "{}\n{\"exception\":{},\"threads\":[],\"pid\":12.0}";
    CHECK(summarise(fraction, strlen(fraction), 12) == Crash_Report_Other_Process);
    // a long string is cut, the text around it read on
    static char text[300000];
    int n = snprintf(text, sizeof(text), "{}\n{\"exception\":{\"type\":\"");
    memset(text + n, 'A', 200000);
    n += 200000;
    n += snprintf(text + n, sizeof(text) - (size_t)n, "\"},\"threads\":[],\"procName\":\"Z\"}");
    CHECK(summarise(text, (size_t)n, 0) == Crash_Report_Summarised);
    CHECK(has("process: Z,"));
    CHECK(strlen(summary) < 2000);
}

// ---- what isn't one whole report
static void test_unreadable(void) {
    const char *texts[] = {
        "",
        "{}",                                            // no body
        "{}\n",                                          // an empty body
        "not json\n{\"exception\":{},\"threads\":[]}",   // no header
        "{}\n{\"exception\":{},\"threads\":[]",          // cut off
        "{}\n{\"exception\":{},\"threads\":[]}}",        // something behind
        "{}\n{\"exception\":{},\"threads\":[]} x",       // something behind
        "{}\n{\"exception\":{},\"threads\":[],}",        // a comma too many
        "{}\n{\"exception\":{},\"threads\":[1x2]}",      // no comma between two values
        "{}\n{\"exception\":{} \"threads\":[]}",         // nor between two members
        "{}\n{\"exception\":{},\"threads\":[01]}",       // a number JSON doesn't write
        "{}\n{\"exception\":{},\"threads\":[1.]}",
        "{}\n{\"exception\":{},\"threads\":[-]}",
        "{}\n{\"exception\":{},\"threads\":[1e]}",
        "{}\n{\"exception\":{},\"threads\":[tru]}",
        "{}\n{\"exception\":{},\"threads\":[\"\\x\"]}",   // an escape JSON doesn't have
        "{}\n{\"exception\":{},\"threads\":[\"\\u12\"]}", // a short \u
        "{}\n{\"exception\":{},\"threads\":[\"a\nb\"]}",  // a line break inside a string
        "{}\n{\"exception\":{},\"threads\":[\"abc]}",     // a string that doesn't end
        "{}\n{\"exception\":{},\"threads\":{}}",          // threads not an array
        "{}\n{\"threads\":[]}",                           // no exception
        "{}\n[{\"exception\":{},\"threads\":[]}]",        // the body not an object
        "{\"a\":{\"b\":{}}}\n{\"exception\":{},\"threads\":[]}", // a header nested deeper than a header is
        "{} {}\n{\"exception\":{},\"threads\":[]}",
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        if (summarise(texts[i], strlen(texts[i]), 0) != Crash_Report_Unreadable) {
            printf("FAIL %s:%d: unreadable text %zu was read: %s\n", __FILE__, __LINE__, i, summary);
            failures++;
        }
        CHECK(summary[0] != '\0'); // says why
    }
    CHECK(crash_report_summary(NULL, 10, 0, NULL, summary, sizeof(summary)) == Crash_Report_Unreadable);
    // the size limit: a whole report a byte too big isn't read
    static char big[Crash_Report_Max_Size + 2];
    int n = snprintf(big, sizeof(big), "{}\n{\"exception\":{},\"threads\":[],\"x\":\"");
    memset(big + n, 'x', sizeof(big) - (size_t)n);
    memcpy(big + Crash_Report_Max_Size - 1, "\"}", 2);
    CHECK(crash_report_summary(big, Crash_Report_Max_Size + 1, 0, NULL, summary, sizeof(summary)) == Crash_Report_Unreadable);
    CHECK(has("too big"));
    CHECK(crash_report_summary(big, Crash_Report_Max_Size, 0, NULL, summary, sizeof(summary)) == Crash_Report_Unreadable);
    big[Crash_Report_Max_Size - 2] = '"', big[Crash_Report_Max_Size - 1] = '}';
    CHECK(crash_report_summary(big, Crash_Report_Max_Size, 0, NULL, summary, sizeof(summary)) == Crash_Report_Summarised);
}

// ---- nesting: at the limit it is read, one deeper it isn't, and a million deep ends at once
static void test_nesting(void) {
    static char text[2100000];
    // the body is one level, so Crash_Report_Max_Depth - 1 arrays inside it are allowed
    for (int depth = Crash_Report_Max_Depth - 1; depth <= Crash_Report_Max_Depth; depth++) {
        int n = snprintf(text, sizeof(text), "{}\n{\"exception\":{},\"threads\":[],\"x\":");
        for (int i = 0; i < depth; i++)
            text[n++] = '[';
        for (int i = 0; i < depth; i++)
            text[n++] = ']';
        text[n++] = '}';
        crash_report_result_t result = summarise(text, (size_t)n, 0);
        CHECK(result == (depth < Crash_Report_Max_Depth ? Crash_Report_Summarised : Crash_Report_Unreadable));
    }
    int n = snprintf(text, sizeof(text), "{}\n{\"x\":");
    for (int i = 0; i < 1000000; i++)
        text[n++] = i % 2 ? '[' : '{';
    clock_t started = clock();
    CHECK(summarise(text, (size_t)n, 0) == Crash_Report_Unreadable);
    CHECK(clock() - started < CLOCKS_PER_SEC);
}

// ---- tiny output buffers: always a string, never past its end
static void test_small_out(void) {
    for (size_t size = 0; size < 64; size++) {
        char *out = malloc(size > 0 ? size : 1);
        crash_report_result_t result = crash_report_summary(made_up, sizeof(made_up) - 1, 0, NULL, out, size);
        CHECK(result == Crash_Report_Summarised);
        if (size > 0)
            CHECK(strlen(out) < size);
        free(out);
    }
}

// ---- cut at every length: whole or unreadable, never anything else, never past the cut
static int cuts_whole;
static void cut_everywhere(const char *name, const char *text, size_t length) {
    size_t last_brace = length;
    while (last_brace > 0 && text[last_brace - 1] != '}')
        last_brace--;
    for (size_t cut = 0; cut <= length; cut++) {
        crash_report_result_t result = summarise(text, cut, 0);
        // only the line break or spaces after the body's last brace may go
        crash_report_result_t expected = cut >= last_brace ? Crash_Report_Summarised : Crash_Report_Unreadable;
        if (result != expected) {
            printf("FAIL %s:%d: %s cut at %zu of %zu: %d, not %d\n", __FILE__, __LINE__, name, cut, length, result,
                   expected);
            failures++;
            return;
        }
        cuts_whole += result == Crash_Report_Summarised;
    }
}

// ---- random input: anything comes back, nothing crashes
static uint64_t seed = 0x5eed2026111813ull;
static uint32_t random32(void) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(seed >> 33);
}

static void test_random(void) {
    static char text[16384];
    const char json_chars[] = "{}[]\":,0123456789-+.eEtrufalsn \\u\n\tAZaz";
    for (int round = 0; round < 3000; round++) {
        size_t length = random32() % sizeof(text);
        bool json_like = round % 2 == 1;
        for (size_t i = 0; i < length; i++)
            text[i] = json_like ? json_chars[random32() % (sizeof(json_chars) - 1)] : (char)random32();
        if (json_like && length > 3)
            memcpy(text, "{}\n", 3);
        crash_report_result_t result = summarise(text, length, round % 3 ? 0 : 5);
        CHECK(result == Crash_Report_Summarised || result == Crash_Report_Unreadable ||
              result == Crash_Report_Other_Process);
    }
    // the made-up report with one to four bytes changed, many times
    static char changed[sizeof(made_up)];
    int summarised = 0;
    for (int round = 0; round < 20000; round++) {
        memcpy(changed, made_up, sizeof(made_up));
        int changes = 1 + (int)(random32() % 4);
        for (int k = 0; k < changes; k++)
            changed[random32() % (sizeof(made_up) - 1)] = (char)(round % 2 ? random32() : "{}[]\":,0x\\u1"[random32() % 12]);
        crash_report_result_t result = summarise(changed, sizeof(made_up) - 1, 0);
        CHECK(result == Crash_Report_Summarised || result == Crash_Report_Unreadable ||
              result == Crash_Report_Other_Process);
        summarised += result == Crash_Report_Summarised;
    }
    // most changes inside strings leave a whole report: the summaries were made
    CHECK(summarised > 1000);
}

// ---- a report's procPath against a program's real path
static void test_paths(void) {
    const char *report = "/Users/USER/Library/Application Support/Steam/*/CoDBlkOps3.app/Contents/MacOS/"
                         "CoDBlkOps3_Exe.bo3macfix";
    CHECK(crash_report_path_matches(report, the_game[1]));
    CHECK(!crash_report_path_matches(report, the_game[0]));
    CHECK(!crash_report_path_matches(report, "/private/tmp/stand-in/CoDBlkOps3_Exe.bo3macfix"));
    CHECK(!crash_report_path_matches(report, "/Users//Library/Application Support/Steam/x/CoDBlkOps3.app/Contents/"
                                             "MacOS/CoDBlkOps3_Exe.bo3macfix"));
    CHECK(!crash_report_path_matches(report, "/Users/someone"));
    CHECK(!crash_report_path_matches(report, "/Volumes/Games/Library/Application Support/Steam/x/CoDBlkOps3.app/"
                                             "Contents/MacOS/CoDBlkOps3_Exe.bo3macfix"));
    CHECK(!crash_report_path_matches(report, Game_Folder "/CoDBlkOps3_Exe.bo3macfix.old"));
    // a library on another disk, without the home folder
    CHECK(crash_report_path_matches("/Volumes/Games/SteamLibrary/*/CoDBlkOps3.app/Contents/MacOS/CoDBlkOps3_Exe",
                                    "/Volumes/Games/SteamLibrary/steamapps/common/Call of Duty Black Ops III/"
                                    "CoDBlkOps3.app/Contents/MacOS/CoDBlkOps3_Exe"));
    // no "*": the same path only
    CHECK(crash_report_path_matches("/a/b/c", "/a/b/c") && !crash_report_path_matches("/a/b/c", "/a/b/cd") &&
          !crash_report_path_matches("/a/b/cd", "/a/b/c") && !crash_report_path_matches("/a/b/c", "/a/b"));
    // stars anywhere, one or more, the last one backing off
    CHECK(crash_report_path_matches("/a/*/c/*/e", "/a/b1/b2/c/d/e") && crash_report_path_matches("/a/*", "/a/x/y"));
    CHECK(crash_report_path_matches("/a/*b", "/a/bbb") && !crash_report_path_matches("/a/*b", "/a/bbc"));
    CHECK(crash_report_path_matches("/a/**/c", "/a//c") && !crash_report_path_matches("/a/*/c", "/a/c"));
    CHECK(crash_report_path_matches("/a/*", "/a/") && crash_report_path_matches("/a/**", "/a/"));
    CHECK(!crash_report_path_matches(NULL, "/a") && !crash_report_path_matches("/a", NULL));
    // a path past ASCII, written raw or as \u escapes in the report
    char text[1024];
    const char *body_start = "{}\n{\"exception\":{},\"threads\":[],\"procPath\":\"";
    snprintf(text, sizeof(text), "%s/Volumes/Jeux vid\\u00e9o/*/CoDBlkOps3_Exe\"}", body_start);
    const char *const french[] = { "/Volumes/Jeux vid\xc3\xa9o/SteamLibrary/CoDBlkOps3_Exe", NULL };
    CHECK(summarise_for(text, strlen(text), 0, french) == Crash_Report_Summarised);
    snprintf(text, sizeof(text), "%s/Volumes/Jeux vid\xc3\xa9o/*/CoDBlkOps3_Exe\"}", body_start);
    CHECK(summarise_for(text, strlen(text), 0, french) == Crash_Report_Summarised);
    snprintf(text, sizeof(text), "%s/Volumes/Jeux vid\\u00e8o/*/CoDBlkOps3_Exe\"}", body_start);
    CHECK(summarise_for(text, strlen(text), 0, french) == Crash_Report_Other_Process);
    snprintf(text, sizeof(text), "%s/Volumes/Jeux \\u0800\\ud83d/*/CoDBlkOps3_Exe\"}", body_start);
    const char *const wide[] = { "/Volumes/Jeux \xe0\xa0\x80?/x/CoDBlkOps3_Exe", NULL };
    CHECK(summarise_for(text, strlen(text), 0, wide) == Crash_Report_Summarised);
    // a character past ASCII that doesn't fit at the end of the 1 KB the path is read into: left out whole, nothing
    // written past it
    static char long_path[1400];
    int n = snprintf(long_path, sizeof(long_path), "%s/", body_start);
    memset(long_path + n, 'a', 1021);
    n += 1021;
    n += snprintf(long_path + n, sizeof(long_path) - (size_t)n, "\\u00e9\"}");
    const char *const short_path[] = { "/x", NULL };
    CHECK(summarise_for(long_path, (size_t)n, 0, short_path) == Crash_Report_Other_Process);
    CHECK(has("aaa)\n") && !has("\xc3"));
    // no procPath at all, or not a string: never the game's
    snprintf(text, sizeof(text), "{}\n{\"exception\":{},\"threads\":[],\"procPath\":5}");
    CHECK(summarise_for(text, strlen(text), 0, the_game) == Crash_Report_Other_Process && has("no procPath"));
}

// ---- the real reports
static int real_reports;
static void test_real_report(const char *path, const char *name) {
    size_t length;
    const char *why;
    char *text = crash_report_read(path, &length, &why);
    if (text == NULL) {
        printf("(%s: not read: %s)\n", path, why);
        return;
    }
    real_reports++;
    crash_report_result_t result = summarise(text, length, 0);
    CHECK(result == Crash_Report_Summarised);
    CHECK(has("process: CoDBlkOps3_Exe"));
    CHECK(has("exception: EXC_"));
    CHECK(has("crashed thread "));
    // the user's two crashes, as wip/aae417/ips.py shows them (wip/night/evidence/crash1-threads.txt, crash2-threads.txt)
    if (strcmp(name, "CoDBlkOps3_Exe.bo3macfix-2026-11-13-182631.ips") == 0) {
        CHECK(has("process: CoDBlkOps3_Exe.bo3macfix, pid 48812, started 2026-11-13 18:25:01.5111 -0800, crashed "
                  "2026-11-13 18:26:04.6255 -0800\n"));
        CHECK(has("exception: EXC_CRASH, SIGABRT\ntermination: Abort trap: 6\nasi: libsystem_c.dylib: abort() called\n"
                  "crashed thread 19 \"Win (WinID 12)\", no rip in the report\n"));
        CHECK(has("   2  libsystem_pthread.dylib pthread_kill+259\n   3  libsystem_c.dylib abort+126\n"
                  "   4  libsystem_malloc.dylib malloc_vreport+857\n   5  libsystem_malloc.dylib malloc_zone_error+183\n"));
        CHECK(has("the game's thread 6 \"Main\":\n   0  ? +0x11c2ac984\n   1  game 0x10147616f\n"));
        if (!has("abort+126"))
            printf("%s", summary);
    }
    if (strcmp(name, "CoDBlkOps3_Exe.bo3macfix-2026-11-14-014216.ips") == 0) {
        CHECK(has("exception: EXC_BAD_ACCESS, SIGSEGV, KERN_INVALID_ADDRESS at 0x0000000000000278\n"
                  "termination: Segmentation fault: 11\ncrashed thread 5 \"Main\", rip 0x1010ae4f1\n"
                  "   0  ? +0x11bc4bb78\n   1  game 0x1010aa8db\n   2  game 0x1011f471a\n   3  game 0x100fc19f4\n"));
        CHECK(!has("the game's thread"));
        if (!has("game 0x1010aa8db"))
            printf("%s", summary);
    }
    // its pid, read back
    const char *pid_at = strstr(summary, ", pid ");
    int pid = pid_at != NULL ? atoi(pid_at + 6) : 0;
    CHECK(pid > 0);
    CHECK(summarise(text, length, pid) == Crash_Report_Summarised);
    CHECK(summarise(text, length, pid + 1) == Crash_Report_Other_Process);
    // the game's program where Steam puts it, never a stand-in elsewhere
    CHECK(summarise_for(text, length, pid, the_game) == Crash_Report_Summarised);
    const char *const elsewhere[] = { "/private/tmp/CoDBlkOps3_Exe", "/private/tmp/CoDBlkOps3_Exe.bo3macfix", NULL };
    CHECK(summarise_for(text, length, pid, elsewhere) == Crash_Report_Other_Process);
    // every cut of the first three (seconds each with the sanitizers)
    if (real_reports <= 3)
        cut_everywhere(name, text, length);
    free(text);
}

static void test_real_reports(void) {
    char user_folder[1024];
    const char *home = getenv("HOME");
    snprintf(user_folder, sizeof(user_folder), "%s/Library/Logs/DiagnosticReports", home != NULL ? home : "/nonexistent");
    const char *folders[] = { getenv("BO3MACFIX_TEST_REPORTS"), "wip/night/evidence", user_folder,
                              "/Library/Logs/DiagnosticReports" };
    char seen[8][256];
    int count = 0;
    for (size_t f = 0; f < sizeof(folders) / sizeof(folders[0]) && count < 8; f++) {
        if (folders[f] == NULL)
            continue;
        DIR *dir = opendir(folders[f]);
        if (dir == NULL)
            continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && count < 8) {
            size_t length = strlen(entry->d_name);
            if (strncmp(entry->d_name, "CoDBlkOps3_Exe", 14) != 0 || length < 5 ||
                strcmp(entry->d_name + length - 4, ".ips") != 0 || length >= sizeof(seen[0]))
                continue;
            bool again = false;
            for (int i = 0; i < count; i++)
                again |= strcmp(seen[i], entry->d_name) == 0;
            if (again)
                continue;
            snprintf(seen[count++], sizeof(seen[0]), "%s", entry->d_name);
            char path[2048];
            snprintf(path, sizeof(path), "%s/%s", folders[f], entry->d_name);
            test_real_report(path, entry->d_name);
        }
        closedir(dir);
    }
    printf("crash_report: %d real reports summarised, %d of them cut at every length (%d cuts whole)\n", real_reports,
           real_reports < 3 ? real_reports : 3, cuts_whole);
}

// ---- the file reader, in a temporary folder of its own ($TMPDIR, else /tmp)
static char folder[1024];

static void test_read(void) {
    char path[1024], link_path[1024];
    size_t length;
    const char *why;
    snprintf(path, sizeof(path), "%s/report.ips", folder);
    FILE *file = fopen(path, "wb");
    fwrite(made_up, 1, sizeof(made_up) - 1, file);
    fclose(file);
    char *text = crash_report_read(path, &length, &why);
    CHECK(text != NULL && length == sizeof(made_up) - 1 && text[length] == '\0' && memcmp(text, made_up, length) == 0);
    free(text);
    // a link isn't followed
    snprintf(link_path, sizeof(link_path), "%s/link.ips", folder);
    CHECK(symlink(path, link_path) == 0);
    CHECK(crash_report_read(link_path, &length, &why) == NULL && length == 0);
    // a pipe without a writer: refused at once, not waited on
    snprintf(path, sizeof(path), "%s/pipe.ips", folder);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(crash_report_read(path, &length, &why) == NULL && strcmp(why, "not a regular file") == 0);
    // empty
    snprintf(path, sizeof(path), "%s/empty.ips", folder);
    fclose(fopen(path, "wb"));
    CHECK(crash_report_read(path, &length, &why) == NULL && strcmp(why, "empty") == 0);
    // 100 MB (sparse, so nothing is written to the disk)
    snprintf(path, sizeof(path), "%s/huge.ips", folder);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 100 << 20) == 0);
    close(fd);
    CHECK(crash_report_read(path, &length, &why) == NULL && strcmp(why, "too big for a crash report") == 0);
    // exactly the limit is read, a byte more isn't
    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && ftruncate(fd, Crash_Report_Max_Size) == 0);
    close(fd);
    text = crash_report_read(path, &length, &why);
    CHECK(text != NULL && length == Crash_Report_Max_Size);
    free(text);
    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && ftruncate(fd, Crash_Report_Max_Size + 1) == 0);
    close(fd);
    CHECK(crash_report_read(path, &length, &why) == NULL);
    unlink(path);
    // not allowed to read
    snprintf(path, sizeof(path), "%s/secret.ips", folder);
    file = fopen(path, "wb");
    fputs(made_up, file);
    fclose(file);
    chmod(path, 0);
    if (getuid() != 0)
        CHECK(crash_report_read(path, &length, &why) == NULL && strcmp(why, "not allowed to read it") == 0);
    // not there, a folder
    CHECK(crash_report_read("/nonexistent/x.ips", &length, &why) == NULL && strcmp(why, "can't open it") == 0);
    CHECK(crash_report_read(folder, &length, &why) == NULL);
    char command[1200];
    snprintf(command, sizeof(command), "chmod -R u+rwx '%s'; rm -rf '%s'", folder, folder);
    system(command);
}

int main(void) {
    const char *temporary = getenv("TMPDIR");
    snprintf(folder, sizeof(folder), "%s/bo3mf_crashreport_XXXXXX",
             temporary != NULL && temporary[0] != '\0' ? temporary : "/tmp");
    if (mkdtemp(folder) == NULL) {
        printf("FAIL: no temporary folder\n");
        return 1;
    }
    test_made_up();
    test_frame_counts();
    test_wrong_kinds();
    test_unreadable();
    test_paths();
    test_nesting();
    test_small_out();
    cut_everywhere("the made-up report", made_up, sizeof(made_up) - 1);
    test_random();
    test_read();
    test_real_reports();
    if (failures == 0)
        printf("crash_report: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
