// T7Overcharged's moves of the first person gun's bones, which All-around Enhancement 4 uses for its gun position
// option and more (AAEGunPosition). See aae_viewmodel.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void aae_viewmodel_init(uint64_t game_base_address, int gun_position);

// ---- what T7Overcharged makes of the texts

// One number as T7Overcharged reads it (MSVC's from_chars over the whole piece, else std::stof): as much of the
// piece's start as strtof reads; false when that is nothing or the number is beyond a float.
bool aae_viewmodel_number(const char *piece, size_t length, float *value);
// One whole number the same way (from_chars, else std::stoi): false for none or one beyond 32 bits.
bool aae_viewmodel_integer(const char *piece, size_t length, int *value);

// A move of a view model bone as T7Overcharged keeps it: the bone's name and its six numbers as two texts of three,
// printed with "%g", which keeps six significant digits. A bone name of 256 characters or more is kept as "": no
// model has such a bone.
typedef struct {
    char bone[256];
    char position[64], angles[64];
} aae_viewmodel_move_t;

// What a viewmodel_p<N>_move<M> dvar's text is to T7Overcharged.
typedef enum {
    Move_Read,       // a bone and six numbers (a '!' or '#' in front is left out for reading)
    Move_None,       // "skip", or fewer than seven pieces: kept as a move without a bone
    Move_Unreadable, // a number that isn't one: the dvar is left out this time
} aae_viewmodel_reading_t;
aae_viewmodel_reading_t aae_viewmodel_read_move(const char *text, aae_viewmodel_move_t *move);
// Whether the text switches its move on: it isn't "skip" and doesn't begin with '!' or '#'.
bool aae_viewmodel_text_active(const char *text);
// The translation and angles a move gives its bone: both texts read back with "%f %f %f" (false unless each gives
// three numbers); the first three numbers a b c are the translation (c, -a, b), the last three the angles.
bool aae_viewmodel_transform(const char *position, const char *angles, float translation[3], float rotation[3]);

// The moves T7Overcharged keeps from one local client's dvars: one per dvar name, in the order they were first seen,
// never removed.
typedef struct {
    char name[64];
    bool active;   // the last text read switched it on
    bool has_bone; // the move has a bone (texts of fewer than seven pieces store one without)
    aae_viewmodel_move_t move;
} aae_viewmodel_entry_t;
typedef struct {
    aae_viewmodel_entry_t *entries;
    int count, capacity;
} aae_viewmodel_entries_t;
// Notes a dvar's text: the entry of that name is made when missing; it takes the text's switch, and only when that
// is on also its move (NULL for one without a bone). False when there was no memory for a new entry.
bool aae_viewmodel_store(aae_viewmodel_entries_t *entries, const char *name, bool active,
                         const aae_viewmodel_move_t *move);
void aae_viewmodel_entries_clear(aae_viewmodel_entries_t *entries);

// The lines of viewmodel_move.cfg, each "[viewmodel_p<N>] bone n n n n n n" ("//" begins a comment; without
// viewmodel_p<N> the line is for local client 0): each one T7Overcharged takes is handed to the function, in order.
typedef void (*aae_viewmodel_config_move_t)(void *context, int local_client, const aae_viewmodel_move_t *move);
void aae_viewmodel_read_move_config(const char *text, size_t length, aae_viewmodel_config_move_t each, void *context);
