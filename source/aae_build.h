// The game build All-around Enhancement (AAE, workshop mod 2631943123) insists on. Its Lua compares
// Engine.GetBuildIntField(BUILD_INTFIELD_CHANGELIST) and Engine.GetBuildStringField(BUILD_STRINGFIELD_BUILD_TIME)
// with the latest Windows build's, and on anything else opens its downgrade guide and quits. BO3MacFix answers those
// with what AAE's own core_mod.ff expects, read on a background thread and remembered in BO3MacFix/aae_build.cache,
// so an AAE update that follows a new PC patch needs no new BO3MacFix. AAEChangelist and AAEBuildTime in
// settings.ini override it. Plain C, tested by tests/test_aae_build.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// what AAE 3.9.5 expects (the PC build of December 2025), for when its fastfile can't tell
#define AAE_BUILTIN_CHANGELIST 13892626
#define AAE_BUILTIN_BUILD_TIME "Wed Dec 10 17:10:46 2025"

// which values a lookup found
#define AAE_FOUND_CHANGELIST 1
#define AAE_FOUND_BUILD_TIME 2
#define AAE_FOUND_BOTH (AAE_FOUND_CHANGELIST | AAE_FOUND_BUILD_TIME)

typedef struct {
    int changelist;      // 0 if not found
    char build_time[48]; // "" if not found
} aae_build_values;

// whether s (n bytes) is a build time like "Wed Dec 10 17:10:46 2025" (the day of the month may be space-padded)
bool aae_build_time_valid(const char *s, size_t n);

// looks for the values in decompressed fastfile data: in a compiled Lua function's constants, the first changelist-
// sized whole number after the string "BUILD_INTFIELD_CHANGELIST" and the first build time after
// "BUILD_STRINGFIELD_BUILD_TIME". Fills in only what out doesn't have yet, returns what it has (AAE_FOUND_*).
int aae_build_scan(const uint8_t *data, size_t len, aae_build_values *out);

// reads the fastfile at path, decompressing its blocks until both values are found. Returns AAE_FOUND_* (0 if
// neither is there), or -1 if the file can't be opened or isn't a fastfile.
int aae_build_read_fastfile(const char *path, aae_build_values *out);

// the cache: what was found in a fastfile of this size and modification time. Load returns AAE_FOUND_* (0 for a
// fastfile that had neither), or -1 if the cache is missing, unreadable or for another file.
int aae_build_cache_load(const char *path, long long size, long long mtime, aae_build_values *out);
bool aae_build_cache_save(const char *path, long long size, long long mtime, const aae_build_values *values);

// starts looking up what AAE's fastfile expects, on a background thread (the cache makes it quick after the first
// time). changelist_override > 0 and a non-empty build_time_override replace that value. Relative paths are taken
// from the current directory now. Logs one line with the outcome.
void aae_build_start(const char *fastfile_path, const char *cache_path, int changelist_override,
                     const char *build_time_override);

// what Lua's Engine.GetBuildIntField(BUILD_INTFIELD_CHANGELIST) and GetBuildStringField(BUILD_STRINGFIELD_BUILD_TIME)
// return: the override, else what AAE's fastfile expects, else the built-in value (also while the lookup is still
// running). Each logs its first answer.
int aae_build_changelist(void);
const char *aae_build_time(void);

// waits up to timeout_ms for the lookup aae_build_start started, returns whether it's done (for the tests)
bool aae_build_wait(int timeout_ms);
