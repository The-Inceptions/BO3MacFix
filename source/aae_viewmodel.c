// All-around Enhancement 4's gun position, and the other moves of the first person gun's bones it makes: what the
// mod's T7Overcharged does with the dvars viewmodel_p<N>_move<M> (AAEGunPosition).
//
// The mod's pause menu has a gun position option (sliders for X, Y, Z and three rotations). It writes
//     viewmodel_p<N>_move0 "tag_ads <Y - 100> <Z - 100> <X - 100> <Y rotation> <Z rotation> <X rotation>"
// and nothing in the game reads such a dvar: the mod's build of T7Overcharged (a Windows DLL the mod loads) does.
// The mod uses the same dvars for more than that option: its low ammo widget writes per weapon moves from its
// weapon table into _move1 and _move2 (a bolt or slide held back on an empty gun, "j_bolt 0 0 -3 0 0 0"; an arm
// moved out of sight), and its client script tilts the gun into _move0 while the player leans. On the Mac none of
// it happened.
//
// T7Overcharged hooks the game's CG_UpdateViewModelDynamicBones, where the game sets the bones of the first person
// weapon it moves by itself (a spinning barrel and the like) on the view model's DObj, every frame. Behind the
// game's own work, for the view model of a local client, on the main thread and not while the front end's UI level
// runs, it:
// - reads the dvars viewmodel_p<N>_move0, _move1, ... (N the view model's local client) until one doesn't exist or
//   is empty. Each holds "bone a b c d e f". The text is split at white space; fewer than seven pieces is a move
//   without a bone. Each number is read on its own, and one that isn't a number leaves the whole dvar out for this
//   frame. "skip", and a text that begins with '!' or '#' (that character isn't read), switch the dvar's move off.
//   The six numbers are kept as two texts printed with "%g", so to six significant digits.
// - keeps one move per dvar name, in the order the names were first seen, for good: a dvar that is later emptied
//   ends the reading before it, so its move and every later one stay as they last were. Odd, but T7Overcharged's,
//   and kept here. The mod never empties one: its "off" texts are moves of a tag by 0.
// - reads viewmodel_move.cfg from its folder (the mod's T7Overcharged folder) once, the same moves as lines.
// - moves the bones, the file's moves first, then the dvars' that are on. A move reads its two texts back with
//   "%f %f %f" and turns the bone's name into the game's string (GScr_AllocString); the bone is found in the DObj
//   (DObjGetBoneIndex) and given the game's local tag (DObjSetLocalTag): translation (c, -a, b), angles (d, e, f).
//   A bone's first local tag in a frame is the one that counts (the DObj keeps a bit for it), so the order matters.
// The same is done here, through the game's own functions, behind the same hook, while the mod with its plugin is
// loaded.
//
// Left out: T7Overcharged first looks for gamedata/viewmodel_move.cfg in the loaded fastfiles and only reads its
// own folder's file without one; the mod has neither (and nothing in the game reads such a rawfile otherwise). Its
// four console commands that reload the file or forget the moves (force_refresh_viewmodel_cmd, clear_cfg_models,
// clear_dvar_models) and its dvar viewmodel_disabled's registration: nothing in the mod uses them; a
// viewmodel_disabled that is set still switches the moves off here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xlocale.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include "aae_plugin.h"
#include "aae_viewmodel.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "t7overcharged.h"

// T7Overcharged's numbers come from a C locale (its own CRT's), so these do
#define C_Locale LC_C_LOCALE

// white space as T7Overcharged's isspace sees it in the C locale
static bool is_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

// ---- what T7Overcharged makes of the texts

// A piece of text as a C string for strtof and strtol: in the buffer when it fits, else in memory of its own.
static char *piece_copy(const char *piece, size_t length, char *buffer, size_t size) {
    char *copy = length < size ? buffer : malloc(length + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, piece, length);
    copy[length] = '\0';
    return copy;
}

// T7Overcharged's number (T7O 0x180079198): MSVC's from_chars over the whole piece, else std::stof (strtof, which
// reads as much of the start as is a number; stof throws for none and for ERANGE, and T7Overcharged's catch makes
// that "not a number"). from_chars gives the same correctly rounded number strtof does wherever it reads the whole
// piece, so this is strtof alone. MSVC's ERANGE means beyond a float or below every float (0 for a number that isn't
// 0); macOS's strtof says ERANGE also for a number below the normal floats, which MSVC reads as it is, so such a
// small one counts here. (A NaN's bits can differ from MSVC's: "nan(123)", "nan(ind)", "nan(snan)".)
bool aae_viewmodel_number(const char *piece, size_t length, float *value) {
    char buffer[256];
    if (length == 0)
        return false;
    char *text = piece_copy(piece, length, buffer, sizeof(buffer)), *end;
    if (text == NULL)
        return false;
    errno = 0;
    float number = strtof_l(text, &end, C_Locale);
    bool read = end != text && (errno != ERANGE || (number != 0.0f && !isinf(number)));
    if (text != buffer)
        free(text);
    if (read)
        *value = number;
    return read;
}

// T7Overcharged's whole number (T7O 0x18007902c): from_chars, else std::stoi (strtol in base 10; MSVC's long has 32
// bits, so ERANGE beyond them).
bool aae_viewmodel_integer(const char *piece, size_t length, int *value) {
    char buffer[64];
    if (length == 0)
        return false;
    char *text = piece_copy(piece, length, buffer, sizeof(buffer)), *end;
    if (text == NULL)
        return false;
    errno = 0;
    long number = strtol_l(text, &end, 10, C_Locale);
    bool read = end != text && errno != ERANGE && number >= INT_MIN && number <= INT_MAX;
    if (text != buffer)
        free(text);
    if (read)
        *value = (int)number;
    return read;
}

// the pieces of a text between white space, at most max of them; the count of all of them
static int split(const char *text, size_t length, const char **pieces, size_t *lengths, int max) {
    int count = 0;
    for (size_t at = 0; at < length;) {
        if (is_space(text[at])) {
            at++;
            continue;
        }
        size_t end = at;
        while (end < length && !is_space(text[end]))
            end++;
        if (count < max)
            pieces[count] = text + at, lengths[count] = end - at;
        count++;
        at = end;
    }
    return count;
}

// a bone and six numbers into a move (pieces: the bone, then the numbers); false when a number isn't one
static bool make_move(const char *const *pieces, const size_t *lengths, aae_viewmodel_move_t *move) {
    float numbers[6];
    for (int i = 0; i < 6; i++)
        if (!aae_viewmodel_number(pieces[1 + i], lengths[1 + i], &numbers[i]))
            return false;
    memset(move, 0, sizeof(*move));
    if (lengths[0] < sizeof(move->bone))
        memcpy(move->bone, pieces[0], lengths[0]);
    snprintf_l(move->position, sizeof(move->position), C_Locale, "%g %g %g", (double)numbers[0], (double)numbers[1],
               (double)numbers[2]);
    snprintf_l(move->angles, sizeof(move->angles), C_Locale, "%g %g %g", (double)numbers[3], (double)numbers[4],
               (double)numbers[5]);
    return true;
}

// T7O 0x18007b9d4 reads "viewmodel_p<N> " followed by the text and wants eight pieces: its own name and these seven
aae_viewmodel_reading_t aae_viewmodel_read_move(const char *text, aae_viewmodel_move_t *move) {
    const char *pieces[7];
    size_t lengths[7];
    memset(move, 0, sizeof(*move));
    if (strcmp(text, "skip") == 0)
        return Move_None;
    if (text[0] == '!' || text[0] == '#')
        text++;
    if (split(text, strlen(text), pieces, lengths, 7) < 7)
        return Move_None;
    return make_move(pieces, lengths, move) ? Move_Read : Move_Unreadable;
}

bool aae_viewmodel_text_active(const char *text) {
    return text[0] != '!' && text[0] != '#' && strcmp(text, "skip") != 0;
}

// T7O 0x180078e0c
bool aae_viewmodel_transform(const char *position, const char *angles, float translation[3], float rotation[3]) {
    float a, b, c;
    if (sscanf_l(position, C_Locale, "%f %f %f", &a, &b, &c) != 3 ||
        sscanf_l(angles, C_Locale, "%f %f %f", &rotation[0], &rotation[1], &rotation[2]) != 3)
        return false;
    translation[0] = c;
    translation[1] = -a;
    translation[2] = b;
    return true;
}

// T7O 0x18007b2b4: entries are matched by the first 64 characters of the name
bool aae_viewmodel_store(aae_viewmodel_entries_t *entries, const char *name, bool active,
                         const aae_viewmodel_move_t *move) {
    aae_viewmodel_entry_t *entry = NULL;
    for (int i = 0; i < entries->count && entry == NULL; i++)
        if (strncmp(entries->entries[i].name, name, sizeof(entry->name)) == 0)
            entry = &entries->entries[i];
    if (entry == NULL) {
        if (entries->count == entries->capacity) {
            int capacity = entries->capacity > 0 ? entries->capacity * 2 : 8;
            aae_viewmodel_entry_t *more = realloc(entries->entries, (size_t)capacity * sizeof(*more));
            if (more == NULL)
                return false;
            entries->entries = more, entries->capacity = capacity;
        }
        entry = &entries->entries[entries->count++];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->name, name, sizeof(entry->name));
    }
    entry->active = active;
    if (active) {
        entry->has_bone = move != NULL;
        if (move != NULL)
            entry->move = *move;
        else
            memset(&entry->move, 0, sizeof(entry->move));
    }
    return true;
}

void aae_viewmodel_entries_clear(aae_viewmodel_entries_t *entries) {
    free(entries->entries);
    memset(entries, 0, sizeof(*entries));
}

#define Client_Prefix "viewmodel_p"

// T7O 0x18007a470: a line is up to a '\n', one '\r' before it dropped, up to a "//", without white space around
void aae_viewmodel_read_move_config(const char *text, size_t length, aae_viewmodel_config_move_t each, void *context) {
    const size_t prefix = strlen(Client_Prefix);
    while (length > 0) {
        const char *newline = memchr(text, '\n', length);
        size_t line_length = newline != NULL ? (size_t)(newline - text) : length;
        const char *line = text;
        text += newline != NULL ? line_length + 1 : length;
        length -= newline != NULL ? line_length + 1 : length;
        if (line_length > 0 && line[line_length - 1] == '\r')
            line_length--;
        for (size_t at = 0; at + 1 < line_length; at++)
            if (line[at] == '/' && line[at + 1] == '/') {
                line_length = at;
                break;
            }
        while (line_length > 0 && is_space(line[0]))
            line++, line_length--;
        while (line_length > 0 && is_space(line[line_length - 1]))
            line_length--;
        // the pieces, with viewmodel_p0 in front of a line whose first piece doesn't begin with viewmodel_p
        const char *pieces[9] = { NULL };
        size_t lengths[9] = { 0 };
        int count = split(line, line_length, pieces + 1, lengths + 1, 8);
        if (count == 0)
            continue;
        if (lengths[1] < prefix || memcmp(pieces[1], Client_Prefix, prefix) != 0) {
            pieces[0] = Client_Prefix "0", lengths[0] = prefix + 1;
            count++;
        } else {
            memmove(pieces, pieces + 1, 8 * sizeof(pieces[0]));
            memmove(lengths, lengths + 1, 8 * sizeof(lengths[0]));
        }
        int local_client;
        aae_viewmodel_move_t move;
        if (count < 8 || !aae_viewmodel_integer(pieces[0] + prefix, lengths[0] - prefix, &local_client) ||
            !make_move(pieces + 1, lengths + 1, &move))
            continue;
        each(context, local_client, &move);
    }
}

// ---- the game's side

#define Local_Clients 4    // the view model's local client, the byte at DObj +0x123, is 0 to 3 for a real one
#define Dvar_Type_Bool 1   // a dvar_t's type (+0x1c): what Dvar_GetBool (0x100fa0224) reads as a byte
#define Dvar_Type_String 8 // and one it reads with atoi, as it does every other kind
#define Part_Bits 12       // the parts mask a local tag is set through: 384 bits, all set
#define Config_Folder "T7Overcharged" // in the mod's folder: T7Overcharged's own (its DLL's name without .ff)
#define Move_Config "viewmodel_move.cfg"

typedef struct DObj DObj;

static int debug;
static void *(*Dvar_FindByHash)(uint32_t hash);
static const char *(*Dvar_GetString)(void *dvar);
static bool (*Sys_IsMainThread)(void);
static bool (*Com_IsRunningUILevel)(void);
static int (*DObjGetLocalClientIndex)(const DObj *obj);
static uint32_t (*GScr_AllocString)(const char *name);
static int (*DObjGetBoneIndex)(const DObj *obj, uint32_t name, uint16_t *index, int models);
static void (*DObjSetLocalTag)(DObj *obj, int32_t *part_bits, int index, const float *translation,
                               const float *angles);

// what is kept for each local client
typedef struct {
    aae_viewmodel_entries_t dvar_moves;
    aae_viewmodel_move_t *config_moves;
    int config_count, config_capacity;
} client_t;
static client_t clients[Local_Clients];
// the mod folder viewmodel_move.cfg was read from, "" before it was; T7Overcharged reads it once (after the
// commands above, again)
static char move_config_from[4096];

// A switch dvar as the game reads a bool: a bool's byte, a text's atoi (the mod sets them from scripts, which makes
// text dvars here where T7Overcharged had registered bools); none, or another kind, is off.
static bool switch_dvar(const char *name) {
    const uint8_t *dvar = Dvar_FindByHash(gsc_hash(name));
    uint32_t type;
    if (dvar == NULL)
        return false;
    memcpy(&type, dvar + dvar_Type, sizeof(type));
    if (type == Dvar_Type_Bool)
        return dvar[dvar_Value] != 0;
    if (type == Dvar_Type_String) {
        const char *text = Dvar_GetString((void *)dvar);
        return text != NULL && atoi(text) != 0;
    }
    return false;
}

// a text dvar's text, NULL for none and for every other kind (the game's getter hands those out as pointers)
static const char *dvar_text(const char *name) {
    const uint8_t *dvar = Dvar_FindByHash(gsc_hash(name));
    uint32_t type;
    if (dvar == NULL)
        return NULL;
    memcpy(&type, dvar + dvar_Type, sizeof(type));
    return type == Dvar_Type_String ? Dvar_GetString((void *)dvar) : NULL;
}

// A line when what a move came to is another than in the last frame (BO3MACFIX_VIEWMODELDEBUG=1): which bone of the
// view model it moved, and by how much, or that the model has no such bone. Moves are told apart by their place in
// the frame's order.
#define Noted_Moves 32
static void note_move(int local_client, int place, const char *bone, int index, const float translation[3],
                      const float rotation[3]) {
    static uint32_t last[Local_Clients][Noted_Moves];
    char line[512];
    if (!debug || place >= Noted_Moves)
        return;
    if (index < 0)
        snprintf(line, sizeof(line), "%s is no bone of the view model", bone);
    else
        snprintf(line, sizeof(line), "%s (bone %d) moved by %g %g %g, turned %g %g %g", bone, index,
                 (double)translation[0], (double)translation[1], (double)translation[2], (double)rotation[0],
                 (double)rotation[1], (double)rotation[2]);
    uint32_t hash = gsc_hash(line) | 1;
    if (last[local_client][place] != hash)
        printf("view model: local client %d: %s\n", local_client, line);
    last[local_client][place] = hash;
}

// T7O 0x180078e0c and 0x180078d64. T7Overcharged hands DObjGetBoneIndex a bone index it never set, which the game
// takes as a guess (0xffff would even be "not there"); its own callers start with 0xfffe, "don't know", as this does.
static void move_bone(int local_client, int place, DObj *obj, const char *bone, const char *position,
                      const char *angles) {
    float translation[3], rotation[3];
    if (!aae_viewmodel_transform(position, angles, translation, rotation) || bone[0] == '\0')
        return;
    uint32_t name = GScr_AllocString(bone);
    uint16_t index = 0xfffe;
    if (!DObjGetBoneIndex(obj, name, &index, -1)) {
        note_move(local_client, place, bone, -1, translation, rotation);
        return;
    }
    int32_t part_bits[Part_Bits];
    memset(part_bits, 0xff, sizeof(part_bits));
    DObjSetLocalTag(obj, part_bits, index, translation, rotation);
    note_move(local_client, place, bone, index, translation, rotation);
}

static void config_move(void *context, int local_client, const aae_viewmodel_move_t *move) {
    (void)context;
    if (local_client < 0 || local_client >= Local_Clients)
        return;
    client_t *client = &clients[local_client];
    if (client->config_count == client->config_capacity) {
        int capacity = client->config_capacity > 0 ? client->config_capacity * 2 : 8;
        aae_viewmodel_move_t *more = realloc(client->config_moves, (size_t)capacity * sizeof(*more));
        if (more == NULL)
            return;
        client->config_moves = more, client->config_capacity = capacity;
    }
    client->config_moves[client->config_count++] = *move;
}

// a file's text, NULL for none; T7Overcharged reads it whole
static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    long size;
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0 &&
        (text = malloc((size_t)size + 1)) != NULL) {
        *length = fread(text, 1, (size_t)size, file);
        text[*length] = '\0';
    }
    fclose(file);
    return text;
}

// viewmodel_move.cfg, read at the first frame of the mod, whole and as it is (T7Overcharged opens it binary). Read
// again here only for another mod's folder, and then every local client's list starts empty (at the first reading
// they all are; T7Overcharged empties the reading client's).
static void read_move_config(int local_client) {
    const char *folder = t7o_mod_folder();
    char path[sizeof(move_config_from) + 64];
    if (strcmp(move_config_from, folder) == 0)
        return;
    snprintf(move_config_from, sizeof(move_config_from), "%s", folder);
    for (int i = 0; i < Local_Clients; i++)
        clients[i].config_count = 0;
    snprintf(path, sizeof(path), "%s/%s/%s", folder, Config_Folder, Move_Config);
    size_t length = 0;
    char *text = read_file(path, &length);
    if (text != NULL && length > 0)
        aae_viewmodel_read_move_config(text, length, config_move, NULL);
    free(text);
    if (debug)
        printf("view model: %s: %d moves for local client %d\n", path, clients[local_client].config_count,
               local_client);
}

// one line when a dvar's text is another than last time (BO3MACFIX_VIEWMODELDEBUG=1)
static void note_text(int local_client, int number, const char *text) {
    static uint32_t last[Local_Clients][8];
    if (!debug || number >= 8)
        return;
    uint32_t hash = gsc_hash(text) | 1;
    if (last[local_client][number] != hash && text[0] == '\0')
        printf("view model: viewmodel_p%d_move%d is empty: the reading ends there, and the moves kept from it and the "
               "dvars after it stay as they were\n", local_client, number);
    else if (last[local_client][number] != hash)
        printf("view model: viewmodel_p%d_move%d is \"%s\"\n", local_client, number, text);
    last[local_client][number] = hash;
}

// T7O 0x18007b9d4, every frame
static void move_bones(int local_client, DObj *obj) {
    client_t *client = &clients[local_client];
    if (switch_dvar("viewmodel_disabled"))
        return;
    for (int number = 0;; number++) {
        char name[64];
        snprintf(name, sizeof(name), Client_Prefix "%d_move%d", local_client, number);
        const char *text = dvar_text(name);
        if (text != NULL)
            note_text(local_client, number, text);
        if (text == NULL || text[0] == '\0')
            break;
        aae_viewmodel_move_t move;
        aae_viewmodel_reading_t reading = aae_viewmodel_read_move(text, &move);
        if (reading != Move_Unreadable)
            aae_viewmodel_store(&client->dvar_moves, name, aae_viewmodel_text_active(text),
                                reading == Move_Read ? &move : NULL);
    }
    read_move_config(local_client);
    int place = 0;
    for (int i = 0; i < client->config_count; i++, place++)
        move_bone(local_client, place, obj, client->config_moves[i].bone, client->config_moves[i].position,
                  client->config_moves[i].angles);
    for (int i = 0; i < client->dvar_moves.count; i++) {
        const aae_viewmodel_entry_t *entry = &client->dvar_moves.entries[i];
        if (entry->active && entry->has_bone)
            move_bone(local_client, place++, obj, entry->move.bone, entry->move.position, entry->move.angles);
    }
}

// What T7Overcharged's hook does behind the game's own work (T7O 0x18007d940).
static void after_dynamic_bones(void *cg, DObj *obj) {
    if (cg == NULL || obj == NULL || !aae_plugin_loaded_lately() || !Sys_IsMainThread() || Com_IsRunningUILevel())
        return;
    int local_client = DObjGetLocalClientIndex(obj);
    if (local_client < 0 || local_client >= Local_Clients)
        return;
    move_bones(local_client, obj);
}

// The game sets the bones of the first person weapon it moves by itself: (cg_t *, playerState_s *, the view model's
// DObj, the weapon, centity_t *)
install_hook_name(CG_UpdateViewModelDynamicBones, void, void *cg, void *ps, DObj *obj, uint64_t weapon, void *cent) {
    orig_CG_UpdateViewModelDynamicBones(cg, ps, obj, weapon, cent);
    after_dynamic_bones(cg, obj);
}

void aae_viewmodel_init(uint64_t game_base_address, int gun_position) {
    if (!gun_position)
        return;
    const char *env = getenv("BO3MACFIX_VIEWMODELDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    Dvar_FindByHash = (void *)(game_base_address + ADDR_Dvar_FindByHash);
    Dvar_GetString = (void *)(game_base_address + ADDR_Dvar_GetString);
    Sys_IsMainThread = (void *)(game_base_address + ADDR_Sys_IsMainThread);
    Com_IsRunningUILevel = (void *)(game_base_address + ADDR_Com_IsRunningUILevel);
    DObjGetLocalClientIndex = (void *)(game_base_address + ADDR_DObjGetLocalClientIndex);
    GScr_AllocString = (void *)(game_base_address + ADDR_GScr_AllocString);
    DObjGetBoneIndex = (void *)(game_base_address + ADDR_DObjGetBoneIndex);
    DObjSetLocalTag = (void *)(game_base_address + ADDR_DObjSetLocalTag);
    install_hook_CG_UpdateViewModelDynamicBones((void *)(game_base_address + ADDR_CG_UpdateViewModelDynamicBones));
    printf("view model: AAEGunPosition on: All-around Enhancement's moves of the first person gun's bones, as its "
           "T7Overcharged makes them\n");
}
