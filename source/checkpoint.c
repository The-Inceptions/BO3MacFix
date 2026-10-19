// See checkpoint.h.
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "checkpoint.h"

#define File_Size 65536
#define Max_Entries 32
#define Key_Size 32
#define Value_Size 256

// what checkpoint_save takes from the settings page; the rest of checkpoint.ini is ours
static const char *const saved_keys[] = { "Map", "Round", "Points", "Perks", "Weapon", "Character", "Stats" };

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
        if (store->count == Max_Entries)
            return false;
        entry = &store->entries[store->count++];
        snprintf(entry->key, sizeof(entry->key), "%s", key);
    }
    snprintf(entry->value, sizeof(entry->value), "%s", value);
    return true;
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
    return read_file(path, text, sizeof(text)) && parse_store(text, store);
}

static bool save_store(const char *path, store_t *store) {
    static char text[File_Size];
    size_t used = (size_t)snprintf(text, sizeof(text), "; BO3MacFix's zombies checkpoint, from its settings page\n");
    for (int i = 0; i < store->count && used < sizeof(text); i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s = %s\n", store->entries[i].key,
                                 store->entries[i].value);
    return used < sizeof(text) && write_file(path, text);
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

// a value AAE can have saved for one of its number options: empty (unset) or a number like 7, -1 or 2.5
static bool valid_option_value(const char *value) {
    size_t len = strlen(value);
    if (len > 16)
        return false;
    size_t i = value[0] == '-' ? 1 : 0, digits = 0, dots = 0;
    for (; i < len; i++) {
        if (isdigit((unsigned char)value[i]))
            digits++;
        else if (value[i] == '.' && dots++ == 0)
            continue;
        else
            return false;
    }
    return len == 0 || digits > 0;
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
    int line_len = value != NULL ? snprintf(line_text, sizeof(line_text), "modvar %s %s", dvar, value) : 0;
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

// ---- the checkpoint

bool checkpoint_save(const checkpoint_paths_t *paths, const char *lines, long now) {
    static store_t given, store;
    if (!parse_store(lines, &given))
        return false;
    // a checkpoint.ini that can't be read is replaced
    if (!load_store(paths->checkpoint, &store))
        store.count = 0;
    long round, points;
    if (!parse_number(store_get(&given, "Round"), &round) || round < 1 ||
        !parse_number(store_get(&given, "Points"), &points) || points < 0)
        return false;
    for (size_t i = 0; i < sizeof(saved_keys) / sizeof(saved_keys[0]); i++)
        if (!store_set(&store, saved_keys[i], store_get(&given, saved_keys[i])))
            return false;
    char saved[32];
    snprintf(saved, sizeof(saved), "%ld", now);
    return store_set(&store, "Saved", saved) && save_store(paths->checkpoint, &store);
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

checkpoint_resume_t checkpoint_resume_state(const checkpoint_paths_t *paths) {
    static store_t store;
    if (!load_store(paths->checkpoint, &store))
        return Checkpoint_No_Resume;
    const char *applied = store_get(&store, "Applied"), *in_use = store_get(&store, "InUse");
    if (applied == NULL || strcmp(applied, "1") != 0)
        return Checkpoint_No_Resume;
    return in_use != NULL && strcmp(in_use, "1") == 0 ? Checkpoint_Resume_In_Use : Checkpoint_Resume_Set_Up;
}

bool checkpoint_mark_in_use(const checkpoint_paths_t *paths) {
    static store_t store;
    if (checkpoint_resume_state(paths) != Checkpoint_Resume_Set_Up)
        return false;
    return load_store(paths->checkpoint, &store) && store_set(&store, "InUse", "1") &&
           save_store(paths->checkpoint, &store);
}

static bool mutations_path(const checkpoint_paths_t *paths, const char *preset, char *out, size_t out_size) {
    int len = snprintf(out, out_size, "%sAAECustomMutations%s.cfg", paths->players, preset);
    return len > 0 && (size_t)len < out_size;
}

// the console command that sets dvar to value, "" for unset
static void add_command(char *commands, size_t size, const char *dvar, const char *value) {
    size_t used = strlen(commands);
    snprintf(commands + used, size > used ? size - used : 0, "modvar %s %s\n", dvar, value[0] ? value : "\"\"");
}

bool checkpoint_apply(const checkpoint_paths_t *paths, const char *preset, const char *previous_round,
                      const char *previous_points, char *commands, size_t commands_size) {
    static store_t store;
    static char before[File_Size], after[File_Size], temp[File_Size];
    if (commands_size == 0 || !load_store(paths->checkpoint, &store))
        return false;
    commands[0] = '\0';
    long round, points;
    if (!parse_number(store_get(&store, "Round"), &round) || !parse_number(store_get(&store, "Points"), &points))
        return false;
    char round_text[16], points_text[16];
    const char *applied = store_get(&store, "Applied");
    if (applied != NULL && strcmp(applied, "1") == 0) {
        // set up already: the values from the first time stay, previous ones included (the game's options are the
        // checkpoint's now)
        preset = store_get(&store, "AppliedPreset");
        const char *r = store_get(&store, "AppliedRound"), *p = store_get(&store, "AppliedPoints");
        if (preset == NULL || r == NULL || p == NULL)
            return false;
        snprintf(round_text, sizeof(round_text), "%s", r);
        snprintf(points_text, sizeof(points_text), "%s", p);
    } else {
        if (!valid_option_value(previous_round) || !valid_option_value(previous_points))
            return false;
        round = round < 1 ? 1 : round > Checkpoint_Max_Round ? Checkpoint_Max_Round : round;
        points = points < 0 ? 0 : points > Checkpoint_Max_Points ? Checkpoint_Max_Points : points;
        snprintf(round_text, sizeof(round_text), "%ld", round);
        snprintf(points_text, sizeof(points_text), "%ld", points);
    }
    char path[PATH_MAX];
    if (!valid_preset(preset) || !mutations_path(paths, preset, path, sizeof(path)) ||
        !read_file(path, before, sizeof(before)) ||
        !aae_cfg_set(before, Checkpoint_Round_Dvar, round_text, temp, sizeof(temp)) ||
        !aae_cfg_set(temp, Checkpoint_Points_Dvar, points_text, after, sizeof(after)))
        return false;
    if (strcmp(before, after) != 0 && !write_file(path, after))
        return false;
    if (applied == NULL || strcmp(applied, "1") != 0) {
        char preset_copy[Value_Size];
        snprintf(preset_copy, sizeof(preset_copy), "%s", preset);
        if (!store_set(&store, "Applied", "1") || !store_set(&store, "AppliedPreset", preset_copy) ||
            !store_set(&store, "AppliedRound", round_text) || !store_set(&store, "AppliedPoints", points_text) ||
            !store_set(&store, "PreviousRound", previous_round) ||
            !store_set(&store, "PreviousPoints", previous_points) || !save_store(paths->checkpoint, &store)) {
            // without a record of the previous values the mutations file mustn't keep the checkpoint's
            if (strcmp(before, after) != 0)
                write_file(path, before);
            return false;
        }
    }
    add_command(commands, commands_size, Checkpoint_Round_Dvar, round_text);
    add_command(commands, commands_size, Checkpoint_Points_Dvar, points_text);
    return true;
}

bool checkpoint_clear(const checkpoint_paths_t *paths, const char *current_round, const char *current_points,
                      char *commands, size_t commands_size) {
    static store_t store;
    static char before[File_Size], after[File_Size], temp[File_Size];
    if (commands_size == 0 || !load_store(paths->checkpoint, &store))
        return false;
    commands[0] = '\0';
    const char *applied = store_get(&store, "Applied");
    if (applied == NULL || strcmp(applied, "1") != 0)
        return false;
    const char *preset = store_get(&store, "AppliedPreset");
    const char *dvars[2] = { Checkpoint_Round_Dvar, Checkpoint_Points_Dvar };
    const char *applied_values[2] = { store_get(&store, "AppliedRound"), store_get(&store, "AppliedPoints") };
    const char *previous[2] = { store_get(&store, "PreviousRound"), store_get(&store, "PreviousPoints") };
    char path[PATH_MAX];
    if (preset != NULL && valid_preset(preset) && mutations_path(paths, preset, path, sizeof(path))) {
        if (!read_file(path, before, sizeof(before)))
            return false;
        snprintf(after, sizeof(after), "%s", before);
        for (int i = 0; i < 2; i++) {
            char current[Value_Size];
            // a value changed since (in AAE's menu, say) is the player's, so it stays
            if (applied_values[i] == NULL || !aae_cfg_value(after, dvars[i], current, sizeof(current)) ||
                strcmp(current, applied_values[i]) != 0)
                continue;
            const char *value = previous[i] != NULL && previous[i][0] != '\0' ? previous[i] : NULL;
            if (!aae_cfg_set(after, dvars[i], value, temp, sizeof(temp)))
                return false;
            snprintf(after, sizeof(after), "%s", temp);
        }
        if (strcmp(before, after) != 0 && !write_file(path, after))
            return false;
    }
    // likewise in the game, where AAE's menu may have changed them since
    const char *current[2] = { current_round, current_points };
    for (int i = 0; i < 2; i++)
        if (current[i] != NULL && applied_values[i] != NULL && strcmp(current[i], applied_values[i]) == 0)
            add_command(commands, commands_size, dvars[i], previous[i] != NULL ? previous[i] : "");
    const char *resume_keys[] = { "Applied", "AppliedPreset", "AppliedRound", "AppliedPoints", "PreviousRound",
                                  "PreviousPoints", "InUse" };
    for (size_t i = 0; i < sizeof(resume_keys) / sizeof(resume_keys[0]); i++)
        store_set(&store, resume_keys[i], NULL);
    return save_store(paths->checkpoint, &store);
}
