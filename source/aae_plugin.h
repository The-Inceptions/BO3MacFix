// Whether the mod that's loaded is All-around Enhancement with its Windows plugin (AAEUnified.dll), whose work
// BO3MacFix does in its place: what the plugin changes in the game only applies while that mod is loaded.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_plugin_init(uint64_t game_base_address);
// looks now: the loaded mod is the one T7Overcharged was last asked for from, and its folder has the plugin
bool aae_plugin_loaded(void);
// the same, looked up at most once a second: for places the game comes through many times a frame
bool aae_plugin_loaded_lately(void);
