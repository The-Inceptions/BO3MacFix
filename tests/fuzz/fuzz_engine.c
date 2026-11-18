// A small coverage-guided fuzzer for the harnesses in tests/fuzz (make fuzz), since the Mac's own clang comes without
// libFuzzer. Each harness defines LLVMFuzzerTestOneInput(data, size), as for libFuzzer, so the same file also builds
// with a full LLVM's -fsanitize=fuzzer.
//
// The harness and the code it feeds are built with -fsanitize-coverage=trace-pc-guard, which calls the two functions
// below at every edge of the program; this file is built without it. An input that reaches an edge none before it
// reached joins the corpus, and new inputs are made by changing the corpus's (bits, bytes, numbers that are often
// wrong, pieces of other inputs, words of the harness's dictionary). A fault stops the run through the address or
// undefined behaviour sanitizer, which first lets this write the input that did it to <out>/crash-<hash>.
//
// Started by hand, it gets the same as from make fuzz with
//     env DYLD_INSERT_LIBRARIES=$PWD/tests/quietcrash.dylib ASAN_OPTIONS=abort_on_error=0 \
//         UBSAN_OPTIONS=abort_on_error=0:halt_on_error=1 tests/fuzz/out/bin/fuzz_<name> ...
// so that nothing it finds leaves a crash report in ~/Library/Logs/DiagnosticReports.
//
// fuzz_<name> [-runs=N] [-seconds=S] [-max_len=N] [-seed=N] [-out=DIR] [CORPUS_DIR | FILE]...
//   With files given and neither -runs nor -seconds, each file is run once (to repeat a crash). Otherwise the
//   directories' files (and the harness's own seeds) start the corpus and it runs until N inputs or S seconds; new
//   corpus entries are written to <out>/corpus.
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
// a harness may give seeds and words of its format: its own definitions take the place of these empty ones
__attribute__((weak)) void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    (void)add;
}
__attribute__((weak)) void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    (void)add;
}

// ---- coverage

static uint8_t *edges_hit;  // per edge: reached by this input
static uint8_t *edges_seen; // reached by any input so far
static uint32_t edge_count;

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
    if (start == stop || *start != 0)
        return;
    for (uint32_t *guard = start; guard < stop; guard++)
        *guard = ++edge_count;
    edges_hit = realloc(edges_hit, edge_count + 1);
    edges_seen = realloc(edges_seen, edge_count + 1);
    if (edges_hit == NULL || edges_seen == NULL)
        abort();
    memset(edges_hit, 0, edge_count + 1);
    memset(edges_seen, 0, edge_count + 1);
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    if (edges_hit != NULL)
        edges_hit[*guard] = 1;
}

// how many edges this input reached that none before it did; they count as seen from now on
static uint32_t new_edges(void) {
    uint32_t found = 0;
    for (uint32_t i = 1; i <= edge_count; i++) {
        if (edges_hit[i] && !edges_seen[i]) {
            edges_seen[i] = 1;
            found++;
        }
        edges_hit[i] = 0;
    }
    return found;
}

static uint32_t edges_total(void) {
    uint32_t total = 0;
    for (uint32_t i = 1; i <= edge_count; i++)
        total += edges_seen[i];
    return total;
}

// ---- inputs

typedef struct {
    uint8_t *data;
    size_t size;
} input_t;

static input_t *corpus;
static size_t corpus_count, corpus_room;
static input_t *words;
static size_t word_count, word_room;
static size_t max_len = 4096;
static char out_dir[1024] = "tests/fuzz/out";

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    // xorshift64*
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545f4914f6cdd1dull;
}
static size_t below(size_t n) {
    return n == 0 ? 0 : (size_t)(rnd() % n);
}

static void push(input_t **list, size_t *count, size_t *room, const void *data, size_t size) {
    if (*count == *room) {
        *room = *room ? *room * 2 : 64;
        *list = realloc(*list, *room * sizeof(**list));
        if (*list == NULL)
            abort();
    }
    uint8_t *copy = malloc(size ? size : 1);
    if (copy == NULL)
        abort();
    memcpy(copy, data, size);
    (*list)[(*count)++] = (input_t){ copy, size };
}

static void add_seed(const void *data, size_t size) {
    push(&corpus, &corpus_count, &corpus_room, data, size < max_len ? size : max_len);
}
static void add_word(const void *data, size_t size) {
    if (size > 0)
        push(&words, &word_count, &word_room, data, size);
}

static uint64_t fnv(const uint8_t *data, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

static void write_input(const char *dir, const char *prefix, const uint8_t *data, size_t size, char *path,
                        size_t path_size) {
    mkdir(out_dir, 0755);
    mkdir(dir, 0755);
    snprintf(path, path_size, "%s/%s%016llx", dir, prefix, (unsigned long long)fnv(data, size));
    FILE *file = fopen(path, "wb");
    if (file != NULL) {
        fwrite(data, 1, size, file);
        fclose(file);
    }
}

// ---- the input being run, written out when it brings the run down

static const uint8_t *current;
static size_t current_size;

static void save_current(const char *prefix) {
    if (current == NULL)
        return;
    char path[1200];
    write_input(out_dir, prefix, current, current_size, path, sizeof(path));
    fprintf(stderr, "fuzz: the input is in %s (%zu bytes)\n", path, current_size);
}

void __sanitizer_set_death_callback(void (*callback)(void));
static void on_death(void) {
    save_current("crash-");
}

// A fatal signal (a wild pointer, or a harness's abort() for a broken promise) doesn't pass the sanitizers' death
// callback: the input is saved here, with the stack, and the signal goes on to the handler that was there before
// (tests/quietcrash.dylib's, which make fuzz puts in: it ends the run with status 128 + the signal and no crash report)
// or, without one, the run ends that way here. macOS writes a crash report for a fatal signal only when nothing
// handles it. This handler takes the place of the address sanitizer's own for those signals (quietcrash's would take
// it otherwise): a find by a wild pointer gives the stack's raw frames here, from backtrace_symbols_fd, and not the
// sanitizer's "SEGV on unknown address" report; the input is saved all the same (a harness built with libFuzzer
// instead of this engine gets the sanitizer's report). Faults the sanitizers find themselves (a read past a buffer,
// undefined behaviour) are reported by them as usual.
static struct sigaction before_ours[32];
static void on_fatal_signal(int signal_number) {
    void *frames[64];
    int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    save_current("crash-");
    current = NULL;
    const struct sigaction *was = &before_ours[signal_number];
    if (!(was->sa_flags & SA_SIGINFO) && was->sa_handler != SIG_DFL && was->sa_handler != SIG_IGN)
        was->sa_handler(signal_number);
    _exit(128 + signal_number);
}
static void catch_fatal_signals(void) {
    static char stack[65536]; // its own, so that a stack overflow is caught too
    stack_t alternate = { .ss_sp = stack, .ss_size = sizeof(stack), .ss_flags = 0 };
    sigaltstack(&alternate, NULL);
    static const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS };
    struct sigaction ours;
    memset(&ours, 0, sizeof(ours));
    ours.sa_handler = on_fatal_signal;
    ours.sa_flags = SA_ONSTACK;
    sigemptyset(&ours.sa_mask);
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        sigaction(fatal[i], &ours, &before_ours[fatal[i]]);
}
static unsigned timeout_seconds = 10;
static void on_alarm(int signal) {
    (void)signal;
    fprintf(stderr, "fuzz: an input ran for more than %u s\n", timeout_seconds);
    save_current("timeout-");
    _exit(70);
}

static void run(const uint8_t *data, size_t size) {
    // a copy of exactly the input's size, so a read past its end is a fault the sanitizer sees
    uint8_t *copy = malloc(size ? size : 1);
    if (copy == NULL)
        abort();
    memcpy(copy, data, size);
    current = copy;
    current_size = size;
    alarm(timeout_seconds);
    LLVMFuzzerTestOneInput(copy, size);
    alarm(0);
    current = NULL;
    free(copy);
}

// ---- making new inputs

static const uint64_t interesting[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 255, 256, 512,
                                        1000, 1024, 4096, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000,
                                        0xfffffffe, 0xffffffff, 0x7fffffffffffffffull, 0x8000000000000000ull,
                                        0xffffffffffffffffull };

static size_t mutate(uint8_t *data, size_t size, size_t room) {
    int steps = 1 + (int)below(4);
    for (int step = 0; step < steps; step++) {
        switch (below(12)) {
        case 0: // a bit
            if (size > 0)
                data[below(size)] ^= (uint8_t)(1u << below(8));
            break;
        case 1: // a byte
            if (size > 0)
                data[below(size)] = (uint8_t)rnd();
            break;
        case 2: // a byte up or down
            if (size > 0)
                data[below(size)] += (uint8_t)(below(2) ? 1 : -1);
            break;
        case 3: { // a number that is often wrong, 1, 2, 4 or 8 bytes, little endian
            size_t width = (size_t)1 << below(4);
            if (size >= width) {
                uint64_t value = interesting[below(sizeof(interesting) / sizeof(interesting[0]))];
                if (below(2))
                    value = 0 - value;
                memcpy(data + below(size - width + 1), &value, width);
            }
            break;
        }
        case 4: { // bytes in
            size_t count = 1 + below(16);
            if (size + count <= room) {
                size_t at = below(size + 1);
                memmove(data + at + count, data + at, size - at);
                for (size_t i = 0; i < count; i++)
                    data[at + i] = (uint8_t)rnd();
                size += count;
            }
            break;
        }
        case 5: { // bytes out
            if (size > 1) {
                size_t at = below(size), count = 1 + below(size - at < 16 ? size - at : 16);
                memmove(data + at, data + at + count, size - at - count);
                size -= count;
            }
            break;
        }
        case 6: { // a piece copied elsewhere in the input
            if (size > 1) {
                size_t from = below(size), to = below(size), count = 1 + below(size - (from > to ? from : to));
                memmove(data + to, data + from, count);
            }
            break;
        }
        case 7: { // a piece repeated
            if (size > 0) {
                size_t from = below(size), count = 1 + below(size - from < 64 ? size - from : 64);
                if (size + count <= room) {
                    size_t at = below(size + 1);
                    memmove(data + at + count, data + at, size - at);
                    memmove(data + at, data + (from < at ? from : from + count), count);
                    size += count;
                }
            }
            break;
        }
        case 8: { // a dictionary word put over the input
            if (word_count > 0) {
                const input_t *word = &words[below(word_count)];
                if (word->size <= size)
                    memcpy(data + below(size - word->size + 1), word->data, word->size);
            }
            break;
        }
        case 9: { // a dictionary word put in
            if (word_count > 0) {
                const input_t *word = &words[below(word_count)];
                if (size + word->size <= room) {
                    size_t at = below(size + 1);
                    memmove(data + at + word->size, data + at, size - at);
                    memcpy(data + at, word->data, word->size);
                    size += word->size;
                }
            }
            break;
        }
        case 10: { // the rest from another corpus entry
            if (corpus_count > 0) {
                const input_t *other = &corpus[below(corpus_count)];
                size_t at = below(size + 1), from = below(other->size + 1), count = other->size - from;
                if (at + count > room)
                    count = room - at;
                memcpy(data + at, other->data + from, count);
                size = at + count;
            }
            break;
        }
        case 11: // cut short
            if (size > 0)
                size = below(size + 1);
            break;
        }
    }
    return size;
}

// ---- the run

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool read_whole(const char *path, uint8_t **data, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;
    size_t room = 4096, used = 0;
    uint8_t *buffer = malloc(room);
    for (size_t got; buffer != NULL && (got = fread(buffer + used, 1, room - used, file)) > 0;) {
        used += got;
        if (used == room) {
            uint8_t *bigger = realloc(buffer, room *= 2);
            if (bigger == NULL) {
                free(buffer);
                buffer = NULL;
            }
            buffer = bigger;
        }
    }
    fclose(file);
    if (buffer == NULL)
        return false;
    *data = buffer;
    *size = used;
    return true;
}

static void load_directory(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL)
        return;
    for (struct dirent *entry; (entry = readdir(dir)) != NULL;) {
        if (entry->d_name[0] == '.')
            continue;
        char file[2048];
        snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
        uint8_t *data;
        size_t size;
        if (!is_directory(file) && read_whole(file, &data, &size)) {
            add_seed(data, size);
            free(data);
        }
    }
    closedir(dir);
}

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    long long runs = -1;
    double seconds = -1;
    const char *files[256];
    int file_count = 0;
    bool any_directory = false;
    rng_state ^= (uint64_t)time(NULL) * 0x100000001b3ull ^ (uint64_t)getpid();
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strncmp(arg, "-runs=", 6) == 0)
            runs = atoll(arg + 6);
        else if (strncmp(arg, "-seconds=", 9) == 0 || strncmp(arg, "-max_total_time=", 16) == 0)
            seconds = atof(strchr(arg, '=') + 1);
        else if (strncmp(arg, "-max_len=", 9) == 0)
            max_len = (size_t)atoll(arg + 9);
        else if (strncmp(arg, "-seed=", 6) == 0)
            rng_state = (uint64_t)atoll(arg + 6) * 0x9e3779b97f4a7c15ull + 1;
        else if (strncmp(arg, "-out=", 5) == 0)
            snprintf(out_dir, sizeof(out_dir), "%s", arg + 5);
        else if (strncmp(arg, "-timeout=", 9) == 0)
            timeout_seconds = (unsigned)atoi(arg + 9);
        else if (arg[0] == '-')
            fprintf(stderr, "fuzz: %s isn't an option of this fuzzer, left out\n", arg);
        else if (file_count < 256) {
            files[file_count++] = arg;
            any_directory = any_directory || is_directory(arg);
        }
    }
    __sanitizer_set_death_callback(on_death);
    catch_fatal_signals();
    signal(SIGALRM, on_alarm);

    // the inputs given, each run once
    if (file_count > 0 && !any_directory && runs < 0 && seconds < 0) {
        for (int i = 0; i < file_count; i++) {
            uint8_t *data;
            size_t size;
            if (!read_whole(files[i], &data, &size)) {
                fprintf(stderr, "fuzz: can't read %s\n", files[i]);
                return 1;
            }
            fprintf(stderr, "fuzz: running %s (%zu bytes)\n", files[i], size);
            run(data, size);
            free(data);
        }
        fprintf(stderr, "fuzz: %d inputs ran without a fault\n", file_count);
        return 0;
    }

    fuzz_dictionary(add_word);
    fuzz_seeds(add_seed);
    for (int i = 0; i < file_count; i++)
        if (is_directory(files[i]))
            load_directory(files[i]);
    if (corpus_count == 0)
        add_seed("", 0);
    // the seeds themselves first
    size_t seeded = corpus_count;
    for (size_t i = 0; i < seeded; i++) {
        run(corpus[i].data, corpus[i].size);
        new_edges();
    }
    if (runs < 0 && seconds < 0)
        seconds = 60;
    char corpus_dir[1100];
    snprintf(corpus_dir, sizeof(corpus_dir), "%s/corpus", out_dir);
    uint8_t *buffer = malloc(max_len ? max_len : 1);
    if (buffer == NULL)
        return 1;
    double start = now_seconds(), last_report = start;
    long long done = 0;
    for (; runs < 0 || done < runs; done++) {
        if ((done & 255) == 0) {
            double now = now_seconds();
            if (seconds >= 0 && now - start >= seconds)
                break;
            if (now - last_report >= 10) {
                fprintf(stderr, "fuzz: %lld inputs, %zu in the corpus, %u edges, %.0f s\n", done, corpus_count,
                        edges_total(), now - start);
                last_report = now;
            }
        }
        const input_t *base = &corpus[below(corpus_count)];
        size_t size = base->size < max_len ? base->size : max_len;
        memcpy(buffer, base->data, size);
        size = mutate(buffer, size, max_len);
        run(buffer, size);
        if (new_edges() > 0) {
            char path[1200];
            add_seed(buffer, size);
            write_input(corpus_dir, "", buffer, size, path, sizeof(path));
        }
    }
    fprintf(stderr, "fuzz: done, %lld inputs in %.0f s without a fault, %zu in the corpus, %u edges\n", done,
            now_seconds() - start, corpus_count, edges_total());
    return 0;
}
