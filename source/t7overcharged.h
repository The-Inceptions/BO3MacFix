#include <stdbool.h>
#include "havok.h"

// Whether a library a mod is loading is T7Overcharged.
bool t7o_is_overcharged_dll(const char *path);
// Pushes our implementation of T7Overcharged's "init" entrypoint for the DLL at path.
void t7o_push_init(lua_State *s, const char *dll_path);
// AAEOnline: lets the mod's Axios fetch its version notes and the weather (GET only, a few public hosts)
void t7o_set_online(int enabled);
