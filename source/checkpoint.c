// See checkpoint.h.
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "checkpoint.h"
#include "zm_loadout.h"

#define File_Size 65536
#define Max_Entries Checkpoint_Max_Entries
#define Key_Size 32
#define Value_Size 1024 // a Guns line: a few guns with their attachments, options and ammo types
_Static_assert(Checkpoint_Read_Size >= Max_Entries * (Key_Size + Value_Size) + 1, "checkpoint_read's lines fit");

// what checkpoint_save takes from the settings page; the rest of checkpoint.ini is ours
static const char *const saved_keys[] = { "Map",       "Round", "Points",    "Perks", "Weapons", "Equipment",
                                          "Held",      "Character", "Stats", "Guns",  "GunInHand", "PerkList",
                                          "PapLevels", "Items", "GunAmmo", "HeroPower", "ShieldHealth" };
// the ones that are comma-separated names: perks, and weapons that go into give and take commands
static const char *const list_keys[] = { "Perks", "Weapons", "Equipment", "PerkList" };

// the AAE options a resume sets, with the key their values have in checkpoint.ini after Applied and Previous
typedef struct {
    const char *key;
    const char *dvar;
} option_t;

static const option_t options[] = {
    { "Round", Checkpoint_Round_Dvar },
    { "Points", Checkpoint_Points_Dvar },
    { "Perkaholic", Checkpoint_Perkaholic_Dvar },
    { "QuickRevive", Checkpoint_Quick_Revive_Dvar },
};
#define Option_Count (sizeof(options) / sizeof(options[0]))

// ---- files

// the file at path into buf; a missing file reads as empty. False if it's there but can't be read, or is too big.
static bool read_file(const char *path, char *buf, size_t size) {
    buf[0] = '\0';
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return errno == ENOENT;
    size_t len = fread(buf, 1, size - 1, file);
    bool whole = !ferror(file) && fgetc(file) == EOF;
    fclose(file);
    buf[len] = '\0';
    return whole && strlen(buf) == len;
}

// writes text to a temporary file that then replaces path, so path is never half written
static bool write_file(const char *path, const char *text) {
    char temp[PATH_MAX + 32];
    if (snprintf(temp, sizeof(temp), "%s.tmp%d", path, (int)getpid()) >= (int)sizeof(temp))
        return false;
    FILE *file = fopen(temp, "wb");
    if (file == NULL)
        return false;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, file) == len;
    ok = fclose(file) == 0 && ok;
    if (ok && rename(temp, path) == 0)
        return true;
    unlink(temp);
    return false;
}

// ---- checkpoint.ini: "Key = value" lines

typedef struct {
    char key[Key_Size];
    char value[Value_Size];
} entry_t;

typedef struct {
    entry_t entries[Max_Entries];
    int count;
    bool full; // the last store_set was refused for want of room
} store_t;

static bool valid_key(const char *key, size_t len) {
    if (len == 0 || len >= Key_Size)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalpha((unsigned char)key[i]))
            return false;
    return true;
}

// printable text (UTF-8 is fine), as a player or weapon name could be
static bool valid_value(const char *value, size_t len) {
    if (len >= Value_Size)
        return false;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)value[i] < 0x20 || value[i] == 0x7f)
            return false;
    return true;
}

static entry_t *store_find(store_t *store, const char *key) {
    for (int i = 0; i < store->count; i++)
        if (strcmp(store->entries[i].key, key) == 0)
            return &store->entries[i];
    return NULL;
}

static const char *store_get(store_t *store, const char *key) {
    entry_t *entry = store_find(store, key);
    return entry != NULL ? entry->value : NULL;
}

// sets key to value, or takes it out if value is NULL
static bool store_set(store_t *store, const char *key, const char *value) {
    entry_t *entry = store_find(store, key);
    store->full = false;
    if (value == NULL) {
        if (entry != NULL) {
            *entry = store->entries[store->count - 1];
            store->count--;
        }
        return true;
    }
    if (!valid_key(key, strlen(key)) || !valid_value(value, strlen(value)))
        return false;
    if (entry == NULL) {
        if (store->count == Max_Entries) {
            store->full = true;
            return false;
        }
        entry = &store->entries[store->count++];
        snprintf(entry->key, sizeof(entry->key), "%s", key);
    }
    snprintf(entry->value, sizeof(entry->value), "%s", value);
    return true;
}

// store_set for a change to checkpoint.ini, saying so when it has no room for key (a reader that finds a file with
// more entries than it holds just doesn't read it, as with any it can't, so a poll doesn't log it again and again)
static bool store_change(store_t *store, const char *key, const char *value) {
    if (store_set(store, key, value))
        return true;
    if (store->full)
        printf("BO3MacFix: checkpoint: checkpoint.ini has no room for %s (it holds %d entries), so it isn't changed\n",
               key, Max_Entries);
    return false;
}

static void trim(const char **start, const char **end) {
    while (*start < *end && (**start == ' ' || **start == '\t'))
        (*start)++;
    while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r'))
        (*end)--;
}

// "Key = value" (or "Key=value") lines into store; blank lines and ones starting with ; or # are skipped. False if a
// line is malformed.
static bool parse_store(const char *text, store_t *store) {
    store->count = 0;
    store->full = false;
    for (const char *line = text; *line != '\0';) {
        const char *end = strchr(line, '\n');
        const char *next = end != NULL ? end + 1 : line + strlen(line);
        if (end == NULL)
            end = next;
        const char *start = line, *stop = end;
        trim(&start, &stop);
        if (start < stop && *start != ';' && *start != '#') {
            const char *equals = memchr(start, '=', (size_t)(stop - start));
            if (equals == NULL)
                return false;
            const char *key_end = equals, *value = equals + 1;
            const char *key_start = start, *value_end = stop;
            trim(&key_start, &key_end);
            trim(&value, &value_end);
            size_t key_len = (size_t)(key_end - key_start), value_len = (size_t)(value_end - value);
            if (!valid_key(key_start, key_len) || !valid_value(value, value_len))
                return false;
            char key[Key_Size], val[Value_Size];
            memcpy(key, key_start, key_len);
            key[key_len] = '\0';
            memcpy(val, value, value_len);
            val[value_len] = '\0';
            if (!store_set(store, key, val))
                return false;
        }
        line = next;
    }
    return true;
}

static bool load_store(const char *path, store_t *store) {
    static char text[File_Size];
    store->count = 0;
    store->full = false;
    return read_file(path, text, sizeof(text)) && parse_store(text, store);
}

static bool save_store(const char *path, store_t *store) {
    static char text[File_Size];
    size_t used = (size_t)snprintf(text, sizeof(text), "; BO3MacFix's zombies checkpoint, from its settings page\n");
    for (int i = 0; i < store->count && used < sizeof(text); i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s = %s\n", store->entries[i].key,
                                 store->entries[i].value);
    if (used >= sizeof(text)) {
        printf("BO3MacFix: checkpoint: checkpoint.ini would be over %d KB, so it isn't changed\n", File_Size / 1024);
        return false;
    }
    return write_file(path, text);
}

// a whole number in text, nothing else
static bool parse_number(const char *text, long *out) {
    if (text == NULL || *text == '\0')
        return false;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || isspace((unsigned char)*text))
        return false;
    *out = value;
    return true;
}

static bool valid_preset(const char *preset) {
    size_t len = strlen(preset);
    if (len == 0 || len > 64)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)preset[i]) && preset[i] != '_' && preset[i] != '-' && preset[i] != ' ')
            return false;
    return true;
}

// ---- AAE's custom mutations files: "modvar <dvar> <value>" lines

// where the value starts if the line from line to end sets dvar, else NULL
static const char *cfg_match(const char *line, const char *end, const char *dvar) {
    while (line < end && (*line == ' ' || *line == '\t'))
        line++;
    size_t command = strlen("modvar"), name = strlen(dvar);
    if ((size_t)(end - line) <= command || strncasecmp(line, "modvar", command) != 0 ||
        (line[command] != ' ' && line[command] != '\t'))
        return NULL;
    line += command;
    while (line < end && (*line == ' ' || *line == '\t'))
        line++;
    if ((size_t)(end - line) < name || strncasecmp(line, dvar, name) != 0)
        return NULL;
    line += name;
    if (line < end && *line != ' ' && *line != '\t' && *line != '\r')
        return NULL;
    while (line < end && (*line == ' ' || *line == '\t'))
        line++;
    return line;
}

bool aae_cfg_value(const char *text, const char *dvar, char *out, size_t out_size) {
    bool found = false;
    for (const char *line = text; *line != '\0';) {
        const char *end = strchr(line, '\n');
        const char *next = end != NULL ? end + 1 : line + strlen(line);
        if (end == NULL)
            end = next;
        const char *value = cfg_match(line, end, dvar);
        if (value != NULL) {
            const char *value_end = end;
            trim(&value, &value_end);
            if (value_end - value >= 2 && *value == '"' && value_end[-1] == '"') {
                value++;
                value_end--;
            }
            size_t len = (size_t)(value_end - value);
            if (len >= out_size)
                return false;
            memcpy(out, value, len);
            out[len] = '\0';
            found = true;
        }
        line = next;
    }
    return found;
}

static bool append(char *out, size_t out_size, size_t *used, const char *text, size_t len) {
    if (*used + len >= out_size)
        return false;
    memcpy(out + *used, text, len);
    *used += len;
    out[*used] = '\0';
    return true;
}

bool aae_cfg_set(const char *text, const char *dvar, const char *value, char *out, size_t out_size) {
    if (out_size == 0)
        return false;
    out[0] = '\0';
    const char *eol = strstr(text, "\r\n") != NULL ? "\r\n" : "\n";
    char line_text[Value_Size + 64];
    // a value with spaces in it (none of AAE's) keeps them in quotes
    const char *quote = value != NULL && strpbrk(value, " \t") != NULL ? "\"" : "";
    int line_len = value != NULL
                       ? snprintf(line_text, sizeof(line_text), "modvar %s %s%s%s", dvar, quote, value, quote)
                       : 0;
    if (line_len < 0 || line_len >= (int)sizeof(line_text))
        return false;
    size_t used = 0;
    bool written = false;
    for (const char *line = text; *line != '\0';) {
        const char *end = strchr(line, '\n');
        const char *next = end != NULL ? end + 1 : line + strlen(line);
        if (end == NULL)
            end = next;
        if (cfg_match(line, end, dvar) == NULL) {
            if (!append(out, out_size, &used, line, (size_t)(next - line)))
                return false;
        } else if (value != NULL && !written) {
            // the line keeps its own line ending
            const char *ending = end > line && end[-1] == '\r' ? end - 1 : end;
            if (!append(out, out_size, &used, line_text, (size_t)line_len) ||
                !append(out, out_size, &used, ending, (size_t)(next - ending)))
                return false;
            written = true;
        }
        line = next;
    }
    if (value != NULL && !written) {
        if (used > 0 && out[used - 1] != '\n' && !append(out, out_size, &used, eol, strlen(eol)))
            return false;
        if (!append(out, out_size, &used, line_text, (size_t)line_len) ||
            !append(out, out_size, &used, eol, strlen(eol)))
            return false;
    }
    return true;
}

// ---- names: weapons and perks, comma-separated in a list

// a weapon or perk name as the game has them (ray_gun_upgraded, ar_standard+reflex, juggernaut), which can go into a
// console command
static bool valid_name(const char *name, size_t len) {
    if (len == 0 || len > 64)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '+')
            return false;
    return true;
}

// the next name in a comma-separated list: its length, and where the one after it starts (NULL after the last)
static size_t next_name(const char *item, const char **rest) {
    const char *comma = strchr(item, ',');
    *rest = comma != NULL ? comma + 1 : NULL;
    return comma != NULL ? (size_t)(comma - item) : strlen(item);
}

// a list of names with nothing else in it; empty or missing is fine
static bool valid_list(const char *list) {
    if (list == NULL || *list == '\0')
        return true;
    for (const char *item = list; item != NULL;) {
        const char *rest;
        if (!valid_name(item, next_name(item, &rest)))
            return false;
        item = rest;
    }
    return true;
}

// whether the list has name in it, before stop if stop isn't NULL
static bool list_has(const char *list, const char *name, size_t name_len, const char *stop) {
    for (const char *item = list; item != NULL && *item != '\0' && (stop == NULL || item < stop);) {
        const char *rest;
        size_t len = next_name(item, &rest);
        if (len == name_len && strncmp(item, name, len) == 0)
            return true;
        item = rest;
    }
    return false;
}

// ---- the checkpoint

bool checkpoint_save(const checkpoint_paths_t *paths, const char *lines, long now) {
    static store_t given, store;
    if (!parse_store(lines, &given))
        return false;
    // a checkpoint.ini that can't be read is replaced, but not one with more entries than it holds (from a newer
    // BO3MacFix): that would lose a resume set up in it
    if (!load_store(paths->checkpoint, &store)) {
        if (store.full) {
            printf("BO3MacFix: checkpoint: checkpoint.ini has more entries than the %d it holds, so it isn't "
                   "changed\n", Max_Entries);
            return false;
        }
        store.count = 0;
    }
    long round, points;
    if (!parse_number(store_get(&given, "Round"), &round) || round < 1 ||
        !parse_number(store_get(&given, "Points"), &points) || points < 0)
        return false;
    for (size_t i = 0; i < sizeof(list_keys) / sizeof(list_keys[0]); i++)
        if (!valid_list(store_get(&given, list_keys[i])))
            return false;
    // the full loadout (zm_loadout.h) has to read back as one
    static loadout_t loadout;
    const char *in_hand = store_get(&given, "GunInHand");
    if ((in_hand != NULL && in_hand[0] != '\0' && !valid_name(in_hand, strlen(in_hand))) ||
        !loadout_parse(store_get(&given, "Guns"), in_hand, store_get(&given, "PerkList"), &loadout) ||
        !loadout_parse_pap_levels(store_get(&given, "PapLevels"), &loadout) ||
        !loadout_parse_items(store_get(&given, "Items"), &loadout) ||
        !loadout_parse_gun_ammo(store_get(&given, "GunAmmo"), &loadout) ||
        !loadout_parse_hero_power(store_get(&given, "HeroPower"), &loadout) ||
        !loadout_parse_shield_health(store_get(&given, "ShieldHealth"), &loadout))
        return false;
    // a key this save doesn't have goes, so nothing from the last game stays with this one; first, so it makes room
    // for this save's keys
    for (size_t i = 0; i < sizeof(saved_keys) / sizeof(saved_keys[0]); i++)
        if (store_get(&given, saved_keys[i]) == NULL)
            store_set(&store, saved_keys[i], NULL);
    for (size_t i = 0; i < sizeof(saved_keys) / sizeof(saved_keys[0]); i++) {
        const char *value = store_get(&given, saved_keys[i]);
        if (value != NULL && !store_change(&store, saved_keys[i], value))
            return false;
    }
    char saved[32];
    snprintf(saved, sizeof(saved), "%ld", now);
    return store_change(&store, "Saved", saved) && save_store(paths->checkpoint, &store);
}

void checkpoint_read(const checkpoint_paths_t *paths, char *out, size_t out_size) {
    static store_t store;
    size_t used = 0;
    out[0] = '\0';
    if (!load_store(paths->checkpoint, &store))
        return;
    for (int i = 0; i < store.count; i++) {
        char line[Key_Size + Value_Size + 2];
        int len = snprintf(line, sizeof(line), "%s=%s\n", store.entries[i].key, store.entries[i].value);
        if (!append(out, out_size, &used, line, (size_t)len))
            return;
    }
}

void checkpoint_saved_when(const char *saved, long now, char *out, size_t out_size) {
    long when;
    if (out_size == 0)
        return;
    out[0] = '\0';
    if (!parse_number(saved, &when) || when <= 0 || when > now + 60)
        return;
    long ago = now > when ? now - when : 0;
    if (ago < 60) {
        snprintf(out, out_size, "just now");
    } else if (ago < 3600) {
        snprintf(out, out_size, "%ld min ago", ago / 60);
    } else if (ago < 86400) {
        snprintf(out, out_size, "%ld h ago", ago / 3600);
    } else {
        time_t then_time = (time_t)when, now_time = (time_t)now;
        struct tm then_tm, now_tm;
        if (localtime_r(&then_time, &then_tm) == NULL || localtime_r(&now_time, &now_tm) == NULL)
            return;
        // English month names, as the rest of the page is English (not the locale's)
        static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        if (then_tm.tm_year == now_tm.tm_year)
            snprintf(out, out_size, "%s %d", months[then_tm.tm_mon], then_tm.tm_mday);
        else
            snprintf(out, out_size, "%s %d %d", months[then_tm.tm_mon], then_tm.tm_mday, then_tm.tm_year + 1900);
    }
}

checkpoint_resume_t checkpoint_resume_state(const checkpoint_paths_t *paths) {
    static store_t store;
    if (!load_store(paths->checkpoint, &store))
        return Checkpoint_No_Resume;
    const char *applied = store_get(&store, "Applied"), *in_use = store_get(&store, "InUse");
    if (applied == NULL || strcmp(applied, "1") != 0)
        return Checkpoint_No_Resume;
    if (in_use != NULL && strcmp(in_use, "2") == 0)
        return Checkpoint_Resume_Weapons_Given;
    return in_use != NULL && strcmp(in_use, "1") == 0 ? Checkpoint_Resume_In_Use : Checkpoint_Resume_Set_Up;
}

bool checkpoint_mark_in_use(const checkpoint_paths_t *paths) {
    static store_t store;
    if (checkpoint_resume_state(paths) != Checkpoint_Resume_Set_Up)
        return false;
    return load_store(paths->checkpoint, &store) && store_change(&store, "InUse", "1") &&
           save_store(paths->checkpoint, &store);
}

bool checkpoint_mark_weapons_given(const checkpoint_paths_t *paths) {
    static store_t store;
    if (checkpoint_resume_state(paths) != Checkpoint_Resume_In_Use)
        return false;
    return load_store(paths->checkpoint, &store) && store_change(&store, "InUse", "2") &&
           save_store(paths->checkpoint, &store);
}

static bool mutations_path(const checkpoint_paths_t *paths, const char *preset, char *out, size_t out_size) {
    int len = snprintf(out, out_size, "%sAAECustomMutations%s.cfg", paths->players, preset);
    return len > 0 && (size_t)len < out_size;
}

// the console command that sets dvar to value: "" for unset, in quotes with spaces in it
static void add_command(char *commands, size_t size, const char *dvar, const char *value) {
    size_t used = strlen(commands);
    const char *quote = value[0] == '\0' || strpbrk(value, " \t") != NULL ? "\"" : "";
    snprintf(commands + used, size > used ? size - used : 0, "modvar %s %s%s%s\n", dvar, quote, value, quote);
}

// what each option is set to for the checkpoint, NULL where it's left alone: the round and points, Perkaholic for any
// perk but Quick Revive (AAE gives every perk or none), and Quick Revive for that one. A checkpoint with the exact
// perks (PerkList) gets them back from zm_perks::give_perk instead (zm_inventory.c), so the player's own perk options
// stay as they are.
static bool resume_values(store_t *store, char text[2][16], const char *values[Option_Count]) {
    long round, points;
    if (!parse_number(store_get(store, "Round"), &round) || !parse_number(store_get(store, "Points"), &points))
        return false;
    round = round < 1 ? 1 : round > Checkpoint_Max_Round ? Checkpoint_Max_Round : round;
    points = points < 0 ? 0 : points > Checkpoint_Max_Points ? Checkpoint_Max_Points : points;
    snprintf(text[0], sizeof(text[0]), "%ld", round);
    snprintf(text[1], sizeof(text[1]), "%ld", points);
    const char *perks = store_get(store, "PerkList") != NULL ? NULL : store_get(store, "Perks");
    bool quick_revive = false, others = false;
    for (const char *item = perks; item != NULL && *item != '\0';) {
        const char *rest;
        size_t len = next_name(item, &rest);
        if (len == strlen(Checkpoint_Quick_Revive_Perk) && strncmp(item, Checkpoint_Quick_Revive_Perk, len) == 0)
            quick_revive = true;
        else if (len > 0)
            others = true;
        item = rest;
    }
    values[0] = text[0];
    values[1] = text[1];
    values[2] = others ? "1" : NULL;
    values[3] = quick_revive ? "1" : NULL;
    return true;
}

static void option_key(char *out, const char *prefix, const option_t *option) {
    snprintf(out, Key_Size, "%s%s", prefix, option->key);
}

// sets dvar to value (NULL: takes its lines out) in text, in place
static bool cfg_set_in_place(char *text, size_t size, const char *dvar, const char *value) {
    static char temp[File_Size];
    if (!aae_cfg_set(text, dvar, value, temp, sizeof(temp)) || strlen(temp) >= size)
        return false;
    memcpy(text, temp, strlen(temp) + 1);
    return true;
}

bool checkpoint_apply(const checkpoint_paths_t *paths, const char *preset, char *commands, size_t commands_size) {
    static store_t store;
    static char before[File_Size], after[File_Size];
    if (commands_size == 0 || !load_store(paths->checkpoint, &store))
        return false;
    commands[0] = '\0';
    char text[2][16];
    const char *values[Option_Count];
    if (!resume_values(&store, text, values))
        return false;
    const char *applied = store_get(&store, "Applied");
    bool again = applied != NULL && strcmp(applied, "1") == 0;
    // set up already: the same preset's file, and the values from before the first time
    if (again)
        preset = store_get(&store, "AppliedPreset");
    char preset_copy[Value_Size], path[PATH_MAX];
    if (preset == NULL || !valid_preset(preset))
        return false;
    snprintf(preset_copy, sizeof(preset_copy), "%s", preset); // store_set moves entries about
    if (!mutations_path(paths, preset_copy, path, sizeof(path)) || !read_file(path, before, sizeof(before)))
        return false;
    memcpy(after, before, strlen(before) + 1);
    for (size_t i = 0; i < Option_Count; i++) {
        char applied_key[Key_Size], previous_key[Key_Size], current[Value_Size];
        option_key(applied_key, "Applied", &options[i]);
        option_key(previous_key, "Previous", &options[i]);
        bool had = aae_cfg_value(after, options[i].dvar, current, sizeof(current));
        if (!had)
            current[0] = '\0';
        if (values[i] == NULL) {
            // set the first time but not wanted now (a new checkpoint without perks): back to the value from before
            const char *was = store_get(&store, applied_key), *previous = store_get(&store, previous_key);
            if (was == NULL)
                continue;
            if (previous == NULL)
                previous = "";
            if (had && strcmp(current, was) == 0 &&
                !cfg_set_in_place(after, sizeof(after), options[i].dvar, previous[0] != '\0' ? previous : NULL))
                return false;
            add_command(commands, commands_size, options[i].dvar, previous);
            store_set(&store, applied_key, NULL);
            store_set(&store, previous_key, NULL);
            continue;
        }
        if (store_get(&store, previous_key) == NULL) {
            // an option that already has the checkpoint's value is the player's own, so it's left alone
            if (strcmp(current, values[i]) == 0)
                continue;
            if (!store_change(&store, previous_key, current))
                return false;
        }
        if (!store_change(&store, applied_key, values[i]) ||
            !cfg_set_in_place(after, sizeof(after), options[i].dvar, values[i]))
            return false;
        add_command(commands, commands_size, options[i].dvar, values[i]);
    }
    if (!store_change(&store, "Applied", "1") || !store_change(&store, "AppliedPreset", preset_copy))
        return false;
    bool changed = strcmp(before, after) != 0;
    if (changed && !write_file(path, after))
        return false;
    if (!save_store(paths->checkpoint, &store)) {
        // without a record of the values from before, the mutations file mustn't keep the checkpoint's
        if (changed)
            write_file(path, before);
        return false;
    }
    return true;
}

// the value a "dvar=value" line in lines gives dvar
static bool current_value(const char *lines, const char *dvar, char *out, size_t out_size) {
    size_t name = strlen(dvar);
    for (const char *line = lines; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
        if (len > name && strncmp(line, dvar, name) == 0 && line[name] == '=') {
            size_t value_len = len - name - 1;
            if (value_len > 0 && line[name + value_len] == '\r')
                value_len--;
            if (value_len >= out_size)
                return false;
            memcpy(out, line + name + 1, value_len);
            out[value_len] = '\0';
            return true;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return false;
}

bool checkpoint_clear(const checkpoint_paths_t *paths, const char *current, char *commands, size_t commands_size) {
    static store_t store;
    static char before[File_Size], after[File_Size];
    if (commands_size == 0 || !load_store(paths->checkpoint, &store))
        return false;
    commands[0] = '\0';
    const char *applied = store_get(&store, "Applied");
    if (applied == NULL || strcmp(applied, "1") != 0)
        return false;
    const char *preset = store_get(&store, "AppliedPreset");
    char path[PATH_MAX];
    bool have_file = preset != NULL && valid_preset(preset) && mutations_path(paths, preset, path, sizeof(path));
    if (have_file) {
        if (!read_file(path, before, sizeof(before)))
            return false;
        memcpy(after, before, strlen(before) + 1);
    }
    for (size_t i = 0; i < Option_Count; i++) {
        char applied_key[Key_Size], previous_key[Key_Size], now[Value_Size];
        option_key(applied_key, "Applied", &options[i]);
        option_key(previous_key, "Previous", &options[i]);
        const char *value = store_get(&store, applied_key), *previous = store_get(&store, previous_key);
        if (value == NULL)
            continue;
        if (previous == NULL)
            previous = "";
        // a value changed since (in AAE's menu, say) is the player's, so it stays
        if (have_file && aae_cfg_value(after, options[i].dvar, now, sizeof(now)) && strcmp(now, value) == 0 &&
            !cfg_set_in_place(after, sizeof(after), options[i].dvar, previous[0] != '\0' ? previous : NULL))
            return false;
        // likewise in the game
        if (current != NULL && current_value(current, options[i].dvar, now, sizeof(now)) && strcmp(now, value) == 0)
            add_command(commands, commands_size, options[i].dvar, previous);
    }
    if (have_file && strcmp(before, after) != 0 && !write_file(path, after))
        return false;
    store_set(&store, "Applied", NULL);
    store_set(&store, "AppliedPreset", NULL);
    store_set(&store, "InUse", NULL);
    for (size_t i = 0; i < Option_Count; i++) {
        char key[Key_Size];
        option_key(key, "Applied", &options[i]);
        store_set(&store, key, NULL);
        option_key(key, "Previous", &options[i]);
        store_set(&store, key, NULL);
    }
    return save_store(paths->checkpoint, &store);
}

int checkpoint_weapon_commands(const char *saved, const char *held, char *out, size_t out_size) {
    if (out_size == 0 || !valid_list(saved) || !valid_list(held))
        return -1;
    out[0] = '\0';
    if (saved == NULL || *saved == '\0')
        return 0;
    if (held == NULL)
        held = "";
    size_t used = 0;
    int count = 0;
    // take first, so a full hand has room for what's given
    for (int pass = 0; pass < 2; pass++) {
        const char *list = pass == 0 ? held : saved, *other = pass == 0 ? saved : held;
        for (const char *item = list; item != NULL && *item != '\0';) {
            const char *rest;
            size_t len = next_name(item, &rest);
            if (!list_has(other, item, len, NULL) && !list_has(list, item, len, item)) {
                char line[96];
                int line_len = snprintf(line, sizeof(line), "%s %.*s\n", pass == 0 ? "take" : "give", (int)len, item);
                if (!append(out, out_size, &used, line, (size_t)line_len))
                    return -1;
                count++;
            }
            item = rest;
        }
    }
    return count;
}

bool checkpoint_loadout(const checkpoint_paths_t *paths, loadout_t *loadout) {
    static store_t store;
    loadout_clear(loadout);
    if (!load_store(paths->checkpoint, &store))
        return false;
    const char *guns = store_get(&store, "Guns"), *perks = store_get(&store, "PerkList");
    if (guns == NULL && perks == NULL)
        return false;
    // the repack levels, items and ammo of a newer save; an older one has none, and comes back as it did
    return loadout_parse(guns, store_get(&store, "GunInHand"), perks, loadout) &&
           loadout_parse_pap_levels(store_get(&store, "PapLevels"), loadout) &&
           loadout_parse_items(store_get(&store, "Items"), loadout) &&
           loadout_parse_gun_ammo(store_get(&store, "GunAmmo"), loadout) &&
           loadout_parse_hero_power(store_get(&store, "HeroPower"), loadout) &&
           loadout_parse_shield_health(store_get(&store, "ShieldHealth"), loadout);
}
