// Running the navmesh converter for a custom map (HKAI_InitMapData in bo3macfix.c), without a shell in between. See
// navmesh_convert.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// whether a map's name is one the converter may be run for: 1 to 63 letters, digits, '_' and '-'
bool navmesh_map_name_ok(const char *map);

// The environment Wine gets: from's entries without any DYLD_ variable, as /bin/sh (one of macOS's protected programs)
// passed it on. A new array of the same texts (free only the array), NULL for no memory.
char **navmesh_environment(char *const *from);

// Runs wine with the converter, "-mac_navmesh", the clump and the map's output path as its arguments, exactly as
// given (no shell reads them; a wine without a folder is looked for in PATH), in the process's folder and its
// environment without DYLD_ variables. Its output goes into output (at most output_size - 1 bytes; the rest is read
// and dropped, so that it never waits on a full pipe), read until wine has ended and then what is left in the pipe;
// its exit status into *status. False when it couldn't be started.
bool navmesh_convert_run(const char *wine, const char *converter, const char *clump, const char *map_output,
                         char *output, size_t output_size, int *status);
