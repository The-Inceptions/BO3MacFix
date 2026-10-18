// Reading and writing BO3MacFix's settings.ini, made of "Name = value" lines (see settings_ini.c). Plain C, so
// tests/test_settings_ini.c tests it without the game.
#pragma once
#include <stdbool.h>

// where the value starts if line sets name ("Name = value", spaces and tabs optional), else NULL
const char *settings_ini_match(const char *line, const char *name);

// the number at value, in base 10, or 16 for RRGGBB colours (which may start with # or 0x). Quotes around it and
// anything after it, like a comment, are fine. Returns false if there's no number.
bool settings_ini_number(const char *value, int base, int *out);

// the number set for name in the file at path (by the last line with one), or fallback
int settings_ini_read(const char *path, const char *name, int fallback, int base);

// sets name = value in the file at path, keeping every other line as it is: the first line that sets name gets the
// new value and any later ones go. Written to a temporary file that then replaces it, so the file is never half
// written. Returns false on failure, leaving the file as it was.
bool settings_ini_write(const char *path, const char *name, const char *value);
