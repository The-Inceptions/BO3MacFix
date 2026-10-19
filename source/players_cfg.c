// See players_cfg.h.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "players_cfg.h"

// when a regular file was last modified, false if there's none at path
static bool modified(const char *path, struct timespec *when) {
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    *when = info.st_mtimespec;
    return true;
}

static void forward_slashes(char *path) {
    for (char *c = path; *c != '\0'; c++)
        if (*c == '\\')
            *c = '/';
}

bool players_cfg_in_game_folder(const char *path, const char *game_folder, char *out, size_t out_len) {
    const char *rest = strcasestr(path, Engine_Players_Folder);
    if (rest == NULL)
        return false;
    rest += strlen(Engine_Players_Folder);
    size_t len = strlen(rest);
    if (len <= 4 || strcasecmp(rest + len - 4, ".cfg") != 0 || strstr(rest, "..") != NULL)
        return false;
    int written = snprintf(out, out_len, "%splayers/%s", game_folder, rest);
    if (written < 0 || (size_t)written >= out_len)
        return false;
    forward_slashes(out);
    struct timespec game_copy, engine_copy;
    if (!modified(out, &game_copy))
        return false;
    char engine[4096];
    written = snprintf(engine, sizeof(engine), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(engine))
        return true;
    forward_slashes(engine);
    if (!modified(engine, &engine_copy))
        return true;
    return game_copy.tv_sec > engine_copy.tv_sec ||
           (game_copy.tv_sec == engine_copy.tv_sec && game_copy.tv_nsec >= engine_copy.tv_nsec);
}
