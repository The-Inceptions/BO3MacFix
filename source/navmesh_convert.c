// Running the navmesh converter for a custom map, see navmesh_convert.h.
//
// A map without the Mac's own navmesh gets one converted from its Windows data by cod2map64.exe under Wine. That
// command used to be one line for popen(), that is for /bin/sh, with the map's name in double quotes. A map's name is
// its zone's, which the map's publisher picks, and a name made on Windows may hold "$(", backquotes and ";", which sh
// runs even inside double quotes: a workshop map could run any command of its publisher's on the player's Mac. A game
// folder with a quote or a "$" in its path broke the command too. Now the arguments go to Wine as a list, as they
// are, and a map whose name isn't plain isn't converted at all (it also names files and folders).
//
// Without sh in between, the rest is kept as sh had it: Wine gets the game's environment without any DYLD_ variable
// (sh is one of macOS's protected programs, which start without them, and so did everything it started), and the
// converter's output is read until the converter has ended, then what is left in the pipe, as popen's pclose waited
// for the process it started and not for others that hold the pipe (Wine's own background processes can, for seconds).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "navmesh_convert.h"

extern char **environ;

#define Map_Name_Most 63
#define Poll_Ms 50 // how often the reading looks whether the converter has ended

bool navmesh_map_name_ok(const char *map) {
    if (map == NULL)
        return false;
    size_t length = strnlen(map, Map_Name_Most + 1);
    if (length == 0 || length > Map_Name_Most)
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = map[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

char **navmesh_environment(char *const *from) {
    size_t count = 0;
    while (from != NULL && from[count] != NULL)
        count++;
    char **kept = calloc(count + 1, sizeof(*kept));
    if (kept == NULL)
        return NULL;
    size_t used = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(from[i], "DYLD_", 5) != 0)
            kept[used++] = from[i];
    return kept;
}

// what the converter printed, at most output_size - 1 bytes of it, the rest read and dropped
static void keep_output(char *output, size_t output_size, size_t *used, const char *chunk, size_t got) {
    size_t keep = output_size > 0 && *used < output_size - 1 ? output_size - 1 - *used : 0;
    if (keep > got)
        keep = got;
    if (keep > 0) {
        memcpy(output + *used, chunk, keep);
        *used += keep;
    }
}

// Reads the pipe until the process has ended (then whatever is left in it, without waiting for others that hold its
// other end) or until its end is closed. True when the process was seen to end, its wait status in *wait_status.
static bool read_until_exit(int fd, pid_t pid, char *output, size_t output_size, size_t *used, int *wait_status) {
    bool ended = false, closed = false;
    while (!ended && !closed) {
        struct pollfd wanted = { .fd = fd, .events = POLLIN };
        int ready = poll(&wanted, 1, Poll_Ms);
        if (ready > 0) {
            char chunk[4096];
            ssize_t got = read(fd, chunk, sizeof(chunk));
            if (got > 0)
                keep_output(output, output_size, used, chunk, (size_t)got);
            else if (got == 0 || (errno != EINTR && errno != EAGAIN))
                closed = true;
        } else if (ready < 0 && errno != EINTR) {
            closed = true;
        }
        pid_t waited = waitpid(pid, wait_status, WNOHANG);
        ended = waited == pid || (waited < 0 && errno == ECHILD);
        if (waited < 0 && errno == ECHILD)
            *wait_status = -1;
    }
    if (ended && !closed) {
        // what it wrote before it ended is in the pipe: taken without waiting for the pipe's other holders
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
            for (;;) {
                char chunk[4096];
                ssize_t got = read(fd, chunk, sizeof(chunk));
                if (got > 0)
                    keep_output(output, output_size, used, chunk, (size_t)got);
                else if (got < 0 && errno == EINTR)
                    continue;
                else
                    break;
            }
        }
    }
    return ended;
}

bool navmesh_convert_run(const char *wine, const char *converter, const char *clump, const char *map_output,
                         char *output, size_t output_size, int *status) {
    if (output_size > 0)
        output[0] = '\0';
    *status = -1;
    char **environment = navmesh_environment(environ);
    if (environment == NULL)
        return false;
    int pipe_ends[2];
    if (pipe(pipe_ends) != 0) {
        free(environment);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
    char *const argv[] = { (char *)wine, (char *)converter, "-mac_navmesh", (char *)clump, (char *)map_output, NULL };
    pid_t pid;
    // (a name without a folder is looked for in PATH, as sh did)
    int spawned = posix_spawnp(&pid, wine, &actions, NULL, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    free(environment);
    close(pipe_ends[1]);
    if (spawned != 0) {
        close(pipe_ends[0]);
        return false;
    }
    size_t used = 0;
    int wait_status = -1;
    bool ended = read_until_exit(pipe_ends[0], pid, output, output_size, &used, &wait_status);
    if (output_size > 0)
        output[used] = '\0';
    close(pipe_ends[0]);
    if (!ended) {
        // the pipe's end came first: the process is ended or ending
        while (waitpid(pid, &wait_status, 0) < 0) {
            if (errno != EINTR)
                return true;
        }
    }
    if (wait_status == -1)
        return true; // reaped by somebody else: no status to give
    *status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128 + WTERMSIG(wait_status);
    return true;
}
