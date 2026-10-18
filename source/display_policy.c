// The decisions behind following display changes (see display_policy.h). Plain C with no macOS calls, so
// tests/test_display_policy.c can run every case without a game or a display.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "display_policy.h"

#define Store_Max ((int)(sizeof(((DisplayStore *)0)->displays) / sizeof(RememberedDisplay)))

int display_policy_find(const DisplayInfo *displays, int count, uint32_t id) {
    for (int i = 0; id != 0 && i < count; i++)
        if (displays[i].id == id)
            return i;
    return -1;
}

int display_policy_find_key(const DisplayInfo *displays, int count, const char *key) {
    for (int i = 0; key != NULL && key[0] != '\0' && i < count; i++)
        if (strcmp(displays[i].key, key) == 0)
            return i;
    return -1;
}

void display_policy_keys(DisplayInfo *displays, int count) {
    for (int i = 0; i < count; i++) {
        strlcpy(displays[i].key, displays[i].uuid, sizeof(displays[i].key));
        for (int j = 0; displays[i].uuid[0] != '\0' && j < i; j++) {
            if (strcmp(displays[j].uuid, displays[i].uuid) == 0) {
                snprintf(displays[i].key, sizeof(displays[i].key), "%s#%u", displays[i].uuid, displays[i].unit);
                break;
            }
        }
    }
}

static const RememberedDisplay *find_remembered(const DisplayStore *store, const char *key) {
    for (int i = 0; key[0] != '\0' && i < store->count; i++)
        if (strcmp(store->displays[i].key, key) == 0)
            return &store->displays[i];
    return NULL;
}

// the same monitor: its vendor, model and serial (identical cheap monitors can share a serial, but never have none)
static int same_serial(const DisplayInfo *display, const RememberedDisplay *remembered) {
    return remembered->serial != 0 && display->serial == remembered->serial && display->vendor == remembered->vendor &&
           display->model == remembered->model;
}

// the same kind of monitor: vendor, model, name and native size
static int same_kind(const DisplayInfo *display, const RememberedDisplay *remembered) {
    return display->vendor == remembered->vendor && display->model == remembered->model &&
           strcmp(display->name, remembered->name) == 0 && display->native_w == remembered->native_w &&
           display->native_h == remembered->native_h;
}

int display_policy_find_remembered(const DisplayInfo *displays, int count, const DisplayStore *store, const char *key) {
    int i = display_policy_find_key(displays, count, key);
    const RememberedDisplay *remembered = i < 0 ? find_remembered(store, key) : NULL;
    if (i >= 0 || remembered == NULL)
        return i;
    // on another port, with another UUID: the same serial, else the only display of its kind that isn't known as
    // another one
    int kind = -1, kinds = 0;
    for (int j = 0; j < count; j++) {
        if (same_serial(&displays[j], remembered))
            return j;
        if (same_kind(&displays[j], remembered) && find_remembered(store, displays[j].key) == NULL) {
            kind = j;
            kinds++;
        }
    }
    return kinds == 1 ? kind : -1;
}

static int find_main(const DisplayInfo *displays, int count) {
    for (int i = 0; i < count; i++)
        if (displays[i].main)
            return i;
    return count > 0 ? 0 : -1;
}

static int find_builtin(const DisplayInfo *displays, int count) {
    for (int i = 0; i < count; i++)
        if (displays[i].builtin)
            return i;
    return -1;
}

int display_policy_target(const DisplayInfo *displays, int count, int game_display, const DisplayStore *store) {
    if (game_display == Game_Display_Builtin) {
        int builtin = find_builtin(displays, count);
        return builtin >= 0 ? builtin : find_main(displays, count);
    }
    if (game_display == Game_Display_Chosen) {
        int chosen = display_policy_find_remembered(displays, count, store, store->chosen);
        if (chosen >= 0)
            return chosen;
        game_display = Game_Display_External;
    }
    if (game_display == Game_Display_External) {
        int last = display_policy_find_remembered(displays, count, store, store->last_external);
        if (last >= 0 && !displays[last].builtin)
            return last;
        int best = -1;
        for (int i = 0; i < count; i++) {
            if (displays[i].builtin)
                continue;
            if (best < 0 || displays[i].refresh > displays[best].refresh ||
                (displays[i].refresh == displays[best].refresh &&
                 (long)displays[i].native_w * displays[i].native_h > (long)displays[best].native_w * displays[best].native_h))
                best = i;
        }
        if (best >= 0)
            return best;
        int builtin = find_builtin(displays, count);
        return builtin >= 0 ? builtin : find_main(displays, count);
    }
    return find_main(displays, count);
}

void display_slots_update(DisplaySlots *table, const DisplayInfo *displays, int count, int *slots) {
    for (int i = 0; i < count; i++) {
        int slot = -1;
        for (int s = 0; slot < 0 && s < table->count; s++)
            if (strcmp(table->displays[s].key, displays[i].key) == 0)
                slot = s;
        if (slot < 0 && table->count < Pick_Slots)
            slot = table->count++;
        // all taken: the first slot whose display isn't connected
        for (int s = 0; slot < 0 && s < table->count; s++)
            if (display_policy_find_key(displays, count, table->displays[s].key) < 0)
                slot = s;
        if (slot >= 0)
            table->displays[slot] = displays[i];
        slots[i] = slot;
    }
}

const DisplayInfo *display_slots_pick(const DisplaySlots *table, int value) {
    int slot = value - Game_Display_Pick;
    return slot >= 0 && slot < table->count && table->displays[slot].key[0] != '\0' ? &table->displays[slot] : NULL;
}

static int pick_value(const int *slots, int i) {
    return Game_Display_Pick + (slots != NULL ? slots[i] : i);
}

int display_policy_choices(const DisplayInfo *displays, int count, const int *slots, int *values, char labels[][64], int max) {
    static const char *const rules[] = { "Main display", "Built-in", "External" };
    int n = 0;
    for (int i = 0; i < 3 && n < max; i++, n++) {
        values[n] = i;
        strlcpy(labels[n], rules[i], 64);
    }
    for (int i = 0; i < count && n < max; i++) {
        if (slots != NULL && slots[i] < 0)
            continue;
        // a second display with the same name is "<name> (2)"
        int same = 1;
        for (int j = 0; j < i; j++)
            same += strcmp(displays[j].name, displays[i].name) == 0;
        values[n] = pick_value(slots, i);
        if (same > 1)
            snprintf(labels[n], 64, "%.58s (%d)", displays[i].name, same);
        else
            strlcpy(labels[n], displays[i].name, 64);
        n++;
    }
    return n;
}

int display_policy_current(int game_display, const DisplayInfo *displays, int count, const int *slots, const DisplayStore *store) {
    if (game_display != Game_Display_Chosen)
        return game_display;
    int chosen = display_policy_find_remembered(displays, count, store, store->chosen);
    return chosen >= 0 && (slots == NULL || slots[chosen] >= 0) ? pick_value(slots, chosen) : Game_Display_Chosen;
}

int display_policy_persisted(const char *name, int value) {
    return strcmp(name, "GameDisplay") == 0 && value >= Game_Display_Pick ? Game_Display_Chosen : value;
}

int display_policy_refresh(int screen_fps, int mode_refresh) {
    if (screen_fps >= Display_Refresh_Low && screen_fps <= Display_Refresh_High)
        return screen_fps;
    if (mode_refresh >= Display_Refresh_Low && mode_refresh <= Display_Refresh_High)
        return mode_refresh;
    return 60;
}

int display_policy_changed(const DisplayInfo *before, const DisplayInfo *after) {
    if (before->x != after->x || before->y != after->y || before->w != after->w || before->h != after->h ||
        before->pixel_w != after->pixel_w || before->pixel_h != after->pixel_h || before->scale != after->scale)
        return 1;
    return before->refresh != after->refresh && after->refresh >= Display_Refresh_Low && after->refresh <= Game_Refresh_Max;
}

void display_policy_shape(const DisplayInfo *display, RememberedDisplay *out) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->key, display->key, sizeof(out->key));
    strlcpy(out->name, display->name, sizeof(out->name));
    out->vendor = display->vendor;
    out->model = display->model;
    out->serial = display->serial;
    out->native_w = display->native_w;
    out->native_h = display->native_h;
    out->max_refresh = display->refresh;
}

static int round8(double value) {
    return (int)lround(value / 8.0) * 8;
}

void display_policy_decide(const RememberedDisplay *from, const GameValues *now, const DisplayInfo *to,
                           const RememberedDisplay *to_memory, int match_refresh, DisplayDecision *out) {
    memset(out, 0, sizeof(*out));
    out->display = to->id;
    out->max_fps = -1;
    out->moved = from->key[0] == '\0' || strcmp(from->key, to->key) != 0;
    if (out->moved) {
        if (to_memory != NULL && to_memory->width > 0 && to_memory->height > 0) {
            out->width = to_memory->width;
            out->height = to_memory->height;
            snprintf(out->why, sizeof(out->why), "remembered for %s", to->name);
        } else if (now->width > 0 && now->height > 0 && to->native_w > 0 && to->native_h > 0) {
            // as many pixels as now (the same GPU cost) at the new display's shape, but no more than its panel has:
            // 2624x1696 on the MacBook is about 2816x1584 on a 4K panel, and 2336x1510 is 1920x1080 on a 1080p one
            double pixels = (double)now->width * now->height, native = (double)to->native_w * to->native_h;
            if (pixels >= native) {
                out->width = to->native_w;
                out->height = to->native_h;
                snprintf(out->why, sizeof(out->why), "the native resolution of %s", to->name);
            } else {
                double aspect = (double)to->native_w / to->native_h, height = sqrt(pixels / aspect);
                out->width = round8(height * aspect);
                out->height = round8(height);
                if (out->width > to->native_w)
                    out->width = to->native_w;
                if (out->height > to->native_h)
                    out->height = to->native_h;
                snprintf(out->why, sizeof(out->why), "the same cost as on %s", from->name[0] ? from->name : "the last display");
            }
        } else {
            snprintf(out->why, sizeof(out->why), "moved to %s", to->name);
        }
        if (match_refresh)
            out->refresh = to_memory != NULL && to_memory->refresh > 0 ? to_memory->refresh : to->refresh;
    } else {
        // the same display changed: the refresh rate follows only if the game was at the display's old maximum
        if (match_refresh && to->refresh > 0 && to->refresh != from->max_refresh && now->refresh == from->max_refresh)
            out->refresh = to->refresh;
        snprintf(out->why, sizeof(out->why), "%s changed", to->name);
    }
    // a frame cap that equalled the old display's refresh rate follows the new display's (120 on the MacBook becomes
    // 240 on a 240 Hz monitor, and back); any other cap, or none, is the user's choice
    if (match_refresh && now->max_fps > 0 && now->max_fps == from->max_refresh && to->refresh > 0 &&
        to->refresh != from->max_refresh)
        out->max_fps = to->refresh;
    if (out->refresh > Game_Refresh_Max)
        out->refresh = Game_Refresh_Max;
    if (out->max_fps > Game_MaxFPS_Max)
        out->max_fps = Game_MaxFPS_Max;
}

int display_policy_launch(const DisplayStore *store, const DisplayInfo *displays, int count, const DisplayInfo *to,
                          const GameValues *now, int use_memory, int match_refresh, DisplayDecision *out) {
    RememberedDisplay from;
    const RememberedDisplay *last = find_remembered(store, store->last_display);
    if (last != NULL) {
        from = *last;
    } else {
        // nothing says which display config.ini's values are from: take the one whose shape they have, the built-in
        // first (2624x1696 is the MacBook's shape, not a 16:9 monitor's)
        int source = -1;
        for (int i = 0; now->height > 0 && i < count; i++) {
            if (displays[i].native_h <= 0)
                continue;
            double aspect = (double)displays[i].native_w / displays[i].native_h, game = (double)now->width / now->height;
            if (fabs(aspect - game) / aspect < 0.01 && (source < 0 || displays[i].builtin))
                source = i;
        }
        if (source < 0)
            return 0;
        display_policy_shape(&displays[source], &from);
    }
    if (strcmp(from.key, to->key) == 0)
        return 0;
    display_policy_decide(&from, now, to, use_memory ? find_remembered(store, to->key) : NULL, match_refresh, out);
    // what's already right stays a "keep"
    if (out->width == now->width && out->height == now->height)
        out->width = out->height = 0;
    if (out->refresh == now->refresh)
        out->refresh = 0;
    if (out->max_fps == now->max_fps)
        out->max_fps = -1;
    return out->width > 0 || out->refresh > 0 || out->max_fps >= 0;
}

int display_policy_parse_game(const char *text, GameValues *out) {
    memset(out, 0, sizeof(*out));
    const char *game = strstr(text, "game="), *refresh = strstr(text, "refresh="), *max_fps = strstr(text, "maxfps=");
    if (refresh != NULL)
        out->refresh = (int)lround(strtod(refresh + 8, NULL));
    if (max_fps != NULL)
        out->max_fps = (int)lround(strtod(max_fps + 7, NULL));
    return game != NULL && sscanf(game + 5, "%dx%d", &out->width, &out->height) == 2 && out->width > 0 && out->height > 0;
}

RememberedDisplay *display_store_find(DisplayStore *store, const char *key) {
    return (RememberedDisplay *)find_remembered(store, key);
}

// a full store forgets the display used longest ago that isn't the last one, the last external or the chosen one
static int forget_one(DisplayStore *store) {
    for (int i = 0; i < store->count; i++) {
        const char *key = store->displays[i].key;
        if (strcmp(key, store->last_display) == 0 || strcmp(key, store->last_external) == 0 ||
            strcmp(key, store->chosen) == 0)
            continue;
        memmove(&store->displays[i], &store->displays[i + 1], (size_t)(store->count - i - 1) * sizeof(RememberedDisplay));
        store->count--;
        return 1;
    }
    return 0;
}

RememberedDisplay *display_store_add(DisplayStore *store, const DisplayInfo *display) {
    if (display->key[0] == '\0')
        return NULL;
    RememberedDisplay entry;
    RememberedDisplay *found = display_store_find(store, display->key);
    if (found != NULL) {
        // the most recently used display goes last, so the front is what a full store forgets first
        entry = *found;
        int i = (int)(found - store->displays);
        memmove(&store->displays[i], &store->displays[i + 1], (size_t)(store->count - i - 1) * sizeof(RememberedDisplay));
        store->count--;
    } else {
        if (store->count >= Store_Max && !forget_one(store))
            return NULL;
        memset(&entry, 0, sizeof(entry));
        strlcpy(entry.key, display->key, sizeof(entry.key));
    }
    strlcpy(entry.name, display->name, sizeof(entry.name));
    entry.vendor = display->vendor;
    entry.model = display->model;
    entry.serial = display->serial;
    if (display->native_w > 0) {
        entry.native_w = display->native_w;
        entry.native_h = display->native_h;
    }
    if (display->refresh > 0)
        entry.max_refresh = display->refresh;
    store->displays[store->count] = entry;
    return &store->displays[store->count++];
}

int display_store_adopt(DisplayStore *store, const DisplayInfo *displays, int count, int i, char *from, size_t n) {
    const DisplayInfo *display = &displays[i];
    if (display->key[0] == '\0' || find_remembered(store, display->key) != NULL)
        return 0;
    const RememberedDisplay *match = NULL;
    for (int r = 0; match == NULL && r < store->count; r++)
        if (same_serial(display, &store->displays[r]))
            match = &store->displays[r];
    if (match == NULL) {
        // one of its kind, and only one: a remembered display that's connected right now is another display
        int kinds = 0;
        for (int r = 0; r < store->count; r++) {
            const RememberedDisplay *entry = &store->displays[r];
            if (display_policy_find_key(displays, count, entry->key) < 0 && same_kind(display, entry)) {
                match = entry;
                kinds++;
            }
        }
        if (kinds != 1)
            return 0;
    }
    RememberedDisplay old = *match;
    RememberedDisplay *entry = display_store_add(store, display);
    if (entry == NULL)
        return 0;
    entry->width = old.width;
    entry->height = old.height;
    entry->refresh = old.refresh;
    // the same monitor under its new key: what pointed at the old one points at this one, unless that's connected too
    if (display_policy_find_key(displays, count, old.key) < 0) {
        if (strcmp(store->last_display, old.key) == 0)
            strlcpy(store->last_display, display->key, sizeof(store->last_display));
        if (strcmp(store->last_external, old.key) == 0)
            strlcpy(store->last_external, display->key, sizeof(store->last_external));
        if (strcmp(store->chosen, old.key) == 0)
            strlcpy(store->chosen, display->key, sizeof(store->chosen));
    }
    strlcpy(from, old.key, n);
    return 1;
}

static char *trim(char *text) {
    text += strspn(text, " \t");
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ' || text[len - 1] == '\t'))
        text[--len] = '\0';
    return text;
}

int display_store_load(DisplayStore *store, const char *path) {
    memset(store, 0, sizeof(*store));
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return 0;
    RememberedDisplay *entry = NULL;
    int in_section = 0; // the keys after a section header belong to it, even when it isn't kept
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = trim(line);
        if (*p == '\0' || *p == '/' || *p == ';' || *p == '#')
            continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            entry = NULL;
            in_section = 1;
            if (end == NULL || end == p + 1 || store->count >= Store_Max)
                continue;
            *end = '\0';
            entry = &store->displays[store->count++];
            memset(entry, 0, sizeof(*entry));
            strlcpy(entry->key, p + 1, sizeof(entry->key));
            continue;
        }
        char *equals = strchr(p, '=');
        if (equals == NULL)
            continue;
        *equals = '\0';
        char *key = trim(p), *value = trim(equals + 1);
        if (entry == NULL && in_section)
            continue;
        if (entry == NULL) {
            if (strcmp(key, "LastDisplay") == 0)
                strlcpy(store->last_display, value, sizeof(store->last_display));
            else if (strcmp(key, "LastExternal") == 0)
                strlcpy(store->last_external, value, sizeof(store->last_external));
            else if (strcmp(key, "Chosen") == 0)
                strlcpy(store->chosen, value, sizeof(store->chosen));
            else if (strcmp(key, "ChosenName") == 0)
                strlcpy(store->chosen_name, value, sizeof(store->chosen_name));
        } else if (strcmp(key, "Name") == 0) {
            strlcpy(entry->name, value, sizeof(entry->name));
        } else if (strcmp(key, "Vendor") == 0) {
            entry->vendor = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "Model") == 0) {
            entry->model = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "Serial") == 0) {
            entry->serial = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "Native") == 0) {
            sscanf(value, "%dx%d", &entry->native_w, &entry->native_h);
        } else if (strcmp(key, "MaxRefresh") == 0) {
            entry->max_refresh = atoi(value);
        } else if (strcmp(key, "WindowSize") == 0) {
            sscanf(value, "%dx%d", &entry->width, &entry->height);
        } else if (strcmp(key, "RefreshRate") == 0) {
            entry->refresh = atoi(value);
        }
    }
    fclose(fp);
    return 1;
}

int display_store_save(const DisplayStore *store, const char *path) {
    // the rename below would replace a displays.ini the user made read-only (rename only needs the folder writable)
    if (access(path, F_OK) == 0 && access(path, W_OK) != 0)
        return -1;
    char temp[1024];
    snprintf(temp, sizeof(temp), "%s.tmp%d", path, (int)getpid());
    FILE *fp = fopen(temp, "w");
    if (fp == NULL)
        return 0;
    fprintf(fp, "// BO3MacFix keeps the game's resolution and refresh rate on each display here (DisplayMemory in settings.ini)\n");
    if (store->last_display[0] != '\0')
        fprintf(fp, "LastDisplay = %s\n", store->last_display);
    if (store->last_external[0] != '\0')
        fprintf(fp, "LastExternal = %s\n", store->last_external);
    if (store->chosen[0] != '\0')
        fprintf(fp, "Chosen = %s\nChosenName = %s\n", store->chosen, store->chosen_name);
    for (int i = 0; i < store->count; i++) {
        const RememberedDisplay *entry = &store->displays[i];
        fprintf(fp, "\n[%s]\nName = %s\nVendor = %u\nModel = %u\nSerial = %u\nNative = %dx%d\nMaxRefresh = %d\n", entry->key,
                entry->name, entry->vendor, entry->model, entry->serial, entry->native_w, entry->native_h, entry->max_refresh);
        if (entry->width > 0 && entry->height > 0)
            fprintf(fp, "WindowSize = %dx%d\n", entry->width, entry->height);
        if (entry->refresh > 0)
            fprintf(fp, "RefreshRate = %d\n", entry->refresh);
    }
    int ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    ok = fclose(fp) == 0 && ok;
    if (!ok || rename(temp, path) != 0) {
        unlink(temp);
        return 0;
    }
    return 1;
}

void display_debounce_poke(DisplayDebounce *d, double now, int begin) {
    if (!d->pending)
        d->first = now;
    d->pending = 1;
    d->last = now;
    d->in_config = begin;
}

double display_debounce_wait(const DisplayDebounce *d, double now, double settle, double cap) {
    if (!d->pending)
        return -1;
    double until_cap = d->first + cap - now;
    if (until_cap <= 0)
        return 0;
    if (d->in_config)
        return until_cap;
    double until_settled = d->last + settle - now;
    if (until_settled <= 0)
        return 0;
    return until_settled < until_cap ? until_settled : until_cap;
}

void display_debounce_clear(DisplayDebounce *d) {
    memset(d, 0, sizeof(*d));
}

void cursor_edge_reset(CursorEdge *edge) {
    memset(edge, 0, sizeof(*edge));
}

CursorAction cursor_edge_sample(CursorEdge *edge, double now, int outside) {
    if (!outside) {
        // back on the game's display: a let-through is over, and a pause ends the attempt
        if (edge->released)
            cursor_edge_reset(edge);
        else if (edge->in_attempt && now - edge->last_outside >= Edge_Attempt_Gap) {
            edge->in_attempt = 0;
            edge->attempt_end = edge->last_outside;
        }
        return Cursor_Stay;
    }
    if (edge->released)
        return Cursor_Let_Through;
    if (edge->in_attempt && now - edge->last_outside < Edge_Attempt_Gap) {
        // still pushing against the edge
        edge->pushed += fmin(now - edge->last_outside, Edge_Step_Max);
    } else {
        // a new attempt; one soon after the last is on purpose
        if (edge->in_attempt)
            edge->attempt_end = edge->last_outside;
        edge->released = edge->attempt_end > 0 && now - edge->attempt_end < Edge_Again_Window;
        edge->in_attempt = 1;
        edge->pushed = 0;
    }
    edge->last_outside = now;
    if (edge->pushed >= Edge_Push_Total)
        edge->released = 1;
    return edge->released ? Cursor_Let_Through : Cursor_Pull_Back;
}
