// All-around Enhancement 4's free aim: the plugin's free_aim component.
//
// The game has no crosshair in third person: its test for drawing one says no while the view is a third person one
// (unless the weapon wants one anyway). The mod has a third person view of its own, and its plugin gives it a
// crosshair and makes shots go to what is under it. This file is that, piece by piece: the crosshair, the crosshair
// while aiming, and bullets, rockets, grenades and other projectiles ("where the shots go" further down). How a
// player on another machine tells the server where they aim is in aae_freeaim_net.c; its hooks are here.
//
// The crosshair: the game's test for drawing one says yes while free aim is on. Free aim is on, by the plugin's rule, for a local client
// whose crosshair code was at work within the last 250 ms (the plugin notes that at a place of its own right before
// the test; here the test's hook does it), whose view has been third person for 250 ms, who isn't a spectator, in a
// map whose name begins with "zm_", unless the dvar aae_freeaim_enabled is 0 (the plugin makes it with 1; without the
// plugin there is none until somebody sets it, and no dvar counts as 1). The second local client has
// aae_freeaim_enabled_1 as a switch of its own.
//
// The plugin wraps the one call of the test in the crosshair function. Here the test itself is hooked and only
// answers differently when that call is the one asking.
//
// The crosshair while aiming down the sights. In first person it goes away as the sights come up: the crosshair
// function keeps how far they are up (the player state's +0x2fc) and draws nothing when that is 1, fades and
// shrinks the crosshair by it before that, and has a row of tests for what the weapon is doing (reloading, being
// raised ...) that hide it as well. In third person there are no sights to look through, so the plugin keeps the
// crosshair: from behind the crosshair's colour on, its copy of the sights' value counts as 0, the tests for what
// the weapon is doing count as passed, and the gap between the crosshair's lines closes from what the weapon's
// spread gives to the weapon's least gap as the sights come up.
// On Windows that is three places (a register zeroed, a call's answer changed, a register changed where the gap
// is worked out). In the Mac's crosshair function the tests are part of the function itself, so here it is two:
// - behind the colour and the test for the crosshair being visible at all, where "cmp bl, 0 ; jne" begins the
//   test for the sights being all the way up: with free aim on, the function's copy of the sights' value (at rbp -
//   0x50) is set to 0 and the function goes on where it draws, past its tests;
// - where the gap is the larger one of the spread's and the weapon's least, in xmm1: changed as above.
// Both only from the pre-patched executable (detour.h).
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aae_freeaim.h"
#include "aae_freeaim_net.h"
#include "aae_plugin.h"
#include "detour.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are the plugin's, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Seen_For_Ms 250        // how long a client counts as seen after the crosshair code was at work for it
#define Third_Person_For_Ms 250 // how long the view has to have been third person
#define Max_Client_Number 0x3f
#define Player_Kind_Spectator 4 // the player state's kind of movement
#define Local_Clients 2

void aae_freeaim_seen(aae_freeaim_client_t *client, const void *cg, uint64_t now) {
    if (client->cg != cg)
        client->third_person = 0;
    client->cg = cg;
    client->seen_at = now;
}

bool aae_freeaim_on(aae_freeaim_client_t *client, uint64_t now, uint32_t client_number, bool third_person,
                    int player_kind, const char *map, float enabled, float enabled_for_client) {
    bool seen = client->seen_at != 0 && now - client->seen_at <= Seen_For_Ms && client->cg != NULL &&
                client_number <= Max_Client_Number;
    bool is_third = seen && third_person;
    uint64_t state = is_third ? 2 : 1, kept = client->third_person;
    if ((kept & 3) != state) {
        client->third_person = now * 4 | state;
        return false;
    }
    if (!is_third || kept >> 2 > now || now - (kept >> 2) < Third_Person_For_Ms)
        return false;
    if (player_kind == Player_Kind_Spectator)
        return false;
    if (map == NULL || strncmp(map, "zm_", 3) != 0)
        return false;
    return enabled != 0.0f && enabled_for_client != 0.0f;
}

static bool is_number(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000) != 0x7f800000;
}

float aae_freeaim_gap(float gap, float least, float sights) {
    if (!is_number(sights) || !is_number(gap) || !is_number(least) || !(0.0f <= least))
        return gap;
    if (0.0f <= sights) {
        if (1.0f < sights)
            sights = 1.0f;
    } else {
        sights = 0.0f;
    }
    return (least - gap) * sights + gap;
}

// ---- where the shots go: what the plugin works out

// a char as the plugin's compiler has it: with a sign
typedef signed char plugin_char;
static bool is_space(plugin_char c) {
    return c == ' ' || (unsigned char)(c - 9) < 5;
}

bool aae_freeaim_number(const char **text, float *value) {
    const plugin_char *p = (const plugin_char *)*text;
    double number = 0.0;
    int sign = 1;
    bool any = false;
    while (is_space(*p))
        p++;
    plugin_char c = *p;
    if (c == '+' || c == '-') {
        p++;
        if (c == '-')
            sign = -1;
    }
    c = *p;
    while (c > '/') {
        c = *p;
        if (c > '9')
            break;
        p++;
        any = true;
        number = number * 10.0 + (double)(c - '0');
        c = *p;
    }
    if (c == '.') {
        p++;
        if (*p > '/') {
            double place = 1.0;
            do {
                c = *p;
                if (c > '9')
                    break;
                place = place * 0.1;
                p++;
                any = true;
                number = number + (double)(c - '0') * place;
            } while (*p > '/');
        }
    }
    if (!any)
        return false;
    if (((unsigned char)((unsigned char)*p + 0xbb) & 0xdf) == 0) { // e or E
        plugin_char mark = p[1];
        const plugin_char *q = p + 1;
        int exponent_sign = 1;
        if (mark == '+' || mark == '-') {
            q = p + 2;
            if (mark == '-')
                exponent_sign = -1;
        }
        if ((unsigned char)(*q - '0') < 10) {
            int exponent = 0;
            do {
                if (*q > '9')
                    break;
                if (exponent < 100)
                    exponent = exponent * 10 + (*q - '0');
                q++;
            } while (*q > '/');
            exponent *= exponent_sign;
            if ((unsigned)(exponent + 38) > 76)
                return false;
            for (; exponent > 0; exponent--)
                number = number * 10.0;
            for (; exponent < 0; exponent++)
                number = number * 0.1;
            p = q;
        }
    }
    number = (double)sign * number;
    if (!isfinite(number) || !(fabs(number) <= 16777216.0))
        return false;
    *text = (const char *)p;
    *value = (float)number;
    return true;
}

bool aae_freeaim_target(const char *text, float target[3]) {
    float numbers[3];
    if (strnlen(text, 192) > 191)
        return false;
    for (int i = 0; i < 3; i++) {
        if (!aae_freeaim_number(&text, &numbers[i]) || (i < 2 && !is_space((plugin_char)*text)))
            return false;
    }
    while (is_space((plugin_char)*text))
        text++;
    if (*text != '\0')
        return false;
    memcpy(target, numbers, sizeof(numbers));
    return true;
}

float aae_freeaim_switch(const char *text, float otherwise) {
    float number;
    if (strnlen(text, 64) > 63 || !aae_freeaim_number(&text, &number))
        return otherwise;
    while (is_space((plugin_char)*text))
        text++;
    return *text == '\0' ? number : otherwise;
}

#define Turn_Tolerance 0.05f   // how far from length 1 and from a right angle the directions may be
#define Turn_Least 0.000001f   // a length below this is none
#define Turn_Farthest 16777216.0f

// the plugin's square root: of what a sum of squares came to
static float root(float sum) {
    return sqrtf(sum);
}
// to length 1, in the plugin's order of sums; false for a length that isn't a number or is none
static bool to_length_one(float v[3]) {
    float length = root(v[1] * v[1] + v[0] * v[0] + v[2] * v[2]);
    if (!is_number(length) || Turn_Least > length)
        return false;
    float scale = 1.0f / length;
    v[0] = v[0] * scale;
    v[1] = v[1] * scale;
    v[2] = v[2] * scale;
    return true;
}

bool aae_freeaim_turn(const float muzzle[3], const float directions[9], const float target[3], float turned[9]) {
    for (int i = 0; i < 3; i++) {
        const float *d = directions + 3 * i;
        if (!is_number(muzzle[i]) || fabsf(muzzle[i]) > Turn_Farthest)
            return false;
        float square = d[1] * d[1] + d[0] * d[0] + d[2] * d[2];
        if (!is_number(square) || fabsf(square - 1.0f) > Turn_Tolerance)
            return false;
    }
    const float *a = directions, *b = directions + 3, *c = directions + 6;
    if (fabsf(a[1] * b[1] + a[0] * b[0] + a[2] * b[2]) > Turn_Tolerance ||
        fabsf(a[1] * c[1] + a[0] * c[0] + a[2] * c[2]) > Turn_Tolerance ||
        fabsf(b[1] * c[1] + b[0] * c[0] + b[2] * c[2]) > Turn_Tolerance)
        return false;
    float forward[3] = { a[0], a[1], a[2] };
    float to[3] = { target[0] - muzzle[0], target[1] - muzzle[1], target[2] - muzzle[2] };
    if (!to_length_one(forward) || !to_length_one(to))
        return false;
    // the turn from forward to "to" as a quaternion: their cross product, and 1 + their dot product
    float x = forward[1] * to[2] - to[1] * forward[2];
    float y = to[0] * forward[2] - forward[0] * to[2];
    float z = forward[0] * to[1] - to[0] * forward[1];
    float w = forward[0] * to[0] + to[1] * forward[1] + forward[2] * to[2] + 1.0f;
    float length = root(x * x + y * y + z * z + w * w);
    if (!is_number(length))
        return false;
    if (Turn_Least > length) {
        // straight back: half a turn about something at a right angle to forward
        float side_x = fabsf(forward[2]) > 0.9f ? 1.0f : 0.0f, side_z = fabsf(forward[2]) > 0.9f ? 0.0f : 1.0f;
        x = forward[1] * side_z - forward[2] * 0.0f;
        y = forward[2] * side_x - forward[0] * side_z;
        z = forward[0] * 0.0f - forward[1] * side_x;
        w = 0.0f;
        length = root(y * y + x * x + z * z);
        if (Turn_Least > length)
            return false;
    }
    float scale = 1.0f / length;
    x = x * scale, y = y * scale, z = z * scale, w = w * scale;
    for (int i = 0; i < 3; i++) {
        const float *v = directions + 3 * i;
        float *out = turned + 3 * i;
        float tx = v[2] * y - v[1] * z, ty = v[0] * z - v[2] * x, tz = v[1] * x - v[0] * y;
        tx = tx + tx, ty = ty + ty, tz = tz + tz;
        out[0] = w * tx + v[0] + y * tz - z * ty;
        out[1] = w * ty + v[1] + z * tx - x * tz;
        out[2] = w * tz + v[2] + x * ty - y * tx;
        if (!is_number(out[0]) || !is_number(out[1]) || !is_number(out[2]))
            return false;
    }
    return true;
}

#define Projectile_Least_Speed 0.0001f // slower than this isn't moving
#define Projectile_Steep 0.99f         // forward's part up or down from which "right" is worked out from another side
#define Degrees_Per_Radian 57.29578f

bool aae_freeaim_projectile(const float origin[3], const float velocity[3], const float target[3],
                            float turned_velocity[3], float angles[3]) {
    float speed = root(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    if (!is_number(speed) || Projectile_Least_Speed >= speed)
        return false;
    // three directions for the turn: where it flies, and a right and an up made from that
    float directions[9], turned[9];
    float *forward = directions, *right = directions + 3, *up = directions + 6;
    float scale = 1.0f / speed;
    forward[0] = velocity[0] * scale;
    forward[1] = velocity[1] * scale;
    forward[2] = velocity[2] * scale;
    float side_x = fabsf(forward[2]) > Projectile_Steep ? 1.0f : 0.0f, side_z = 1.0f - side_x;
    float x = forward[1] * side_z - forward[2] * 0.0f;
    float y = forward[2] * side_x - forward[0] * side_z;
    float z = forward[0] * 0.0f - forward[1] * side_x;
    float length = root(y * y + x * x + z * z);
    if (!is_number(length) || Turn_Least > length)
        return false;
    scale = 1.0f / length;
    right[0] = x * scale;
    right[1] = y * scale;
    right[2] = z * scale;
    up[0] = right[1] * forward[2] - right[2] * forward[1];
    up[1] = right[2] * forward[0] - right[0] * forward[2];
    up[2] = right[0] * forward[1] - right[1] * forward[0];
    if (!aae_freeaim_turn(origin, directions, target, turned))
        return false;
    float level = root(turned[0] * turned[0] + turned[1] * turned[1]);
    double pitch = atan2((double)turned[2], (double)level);
    double yaw = atan2((double)turned[1], (double)turned[0]);
    turned_velocity[0] = turned[0] * speed;
    turned_velocity[1] = turned[1] * speed;
    turned_velocity[2] = turned[2] * speed;
    angles[0] = -(float)pitch * Degrees_Per_Radian;
    angles[1] = (float)yaw * Degrees_Per_Radian;
    angles[2] = 0.0f;
    return true;
}

#define Landing_Sideways 0.001f // of the speed downwards: how far to a side "straight down" may be

bool aae_freeaim_landing_blast(const char *weapon, const float velocity[3]) {
    if (weapon == NULL ||
        (strcmp(weapon, "launcher_standard_upgraded") != 0 && strcmp(weapon, "pistol_standard_upgraded") != 0))
        return false;
    float sideways = -velocity[2] * Landing_Sideways;
    return velocity[2] < 0.0f && fabsf(velocity[0]) <= sideways && fabsf(velocity[1]) <= sideways;
}

// ---- the client's side of a shot: what the plugin works out

#define Point_Bound 10000000.0f // the plugin takes a point from the game only with each coordinate below 1e7 either way

// a point the plugin takes as one (its FUN_18001d0c0; it also tests for numbers first, which this refuses as well)
static bool is_point(const float point[3]) {
    for (int i = 0; i < 3; i++)
        if (!(fabsf(point[i]) < Point_Bound))
            return false;
    return true;
}

// The tracer. A shot the client turned is kept with where the gun's tag_flash was, and when the queued shot's
// effects are drawn, its tracer starts there instead of at the point the game took (in third person one in front of
// the player's head). Which kept shot a queued one is goes by where both start and by how recent the kept one is.
#define Flash_Kept_Ms 250      // how long where the flash was last found stands in for one that can't be found
#define Shot_Kept_Ms 500       // how long a kept shot can be found
#define Flash_Farthest 16384.0f // 128 squared: how far the flash may be from where the shot starts, and from where
                                // the queued shot starts and its tracer would have
#define Start_Farthest 64.0f   // 8 squared: how far a kept shot's start may be from where the queued shot starts
#define Target_Nearest 1.0     // the point's distance from the flash, squared and in doubles: at least 1,
#define Target_Farthest 17179869184.0 // and at most 131072 squared

// The squared distance between two points, in the plugin's order (it adds the first square to 0 before the others,
// which changes nothing).
static float squared_distance(const float a[3], const float b[3]) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

bool aae_freeaim_flash(aae_freeaim_tracers_t *kept, const float start[3], const float *found, uint64_t now,
                       float flash[3]) {
    float point[3];
    if (found != NULL)
        memcpy(point, found, sizeof(point));
    else if (kept->flash_at != 0 && now >= kept->flash_at && now - kept->flash_at <= Flash_Kept_Ms)
        memcpy(point, kept->flash, sizeof(point));
    else
        return false;
    if (!is_point(point) || !(Flash_Farthest >= squared_distance(point, start)))
        return false;
    if (found != NULL) {
        memcpy(kept->flash, point, sizeof(point));
        kept->flash_at = now;
    }
    memcpy(flash, point, sizeof(point));
    return true;
}

bool aae_freeaim_keep(aae_freeaim_tracers_t *kept, const float start[3], const float flash[3], const float target[3],
                      uint64_t now) {
    if (!is_point(start) || !is_point(flash) || !is_point(target))
        return false;
    aae_freeaim_shot_t *shot = &kept->shots[kept->count % AAE_FREEAIM_SHOTS];
    memcpy(shot->start, start, sizeof(shot->start));
    memcpy(shot->flash, flash, sizeof(shot->flash));
    memcpy(shot->target, target, sizeof(shot->target));
    shot->made_at = now;
    shot->used = false;
    kept->count++;
    kept->pending = true;
    return true;
}

// a kept shot a queued one can still find now
static bool shot_findable(const aae_freeaim_shot_t *shot, uint64_t now) {
    return shot->made_at != 0 && !shot->used && now >= shot->made_at && now - shot->made_at <= Shot_Kept_Ms;
}

// Whether a kept shot fits a queued one (origin, muzzle: where the queued shot starts, and where its tracer would).
// The plugin tests a few of these twice, and the first test of the flash's distance from the muzzle lets a flash
// that isn't a number through; the ones here come to the same.
static bool shot_fits(const aae_freeaim_shot_t *shot, const float origin[3], const float muzzle[3]) {
    if (squared_distance(origin, shot->start) > Start_Farthest) // (a start that isn't a number passes, as there)
        return false;
    if (!is_point(shot->flash) || !(squared_distance(shot->flash, muzzle) <= Flash_Farthest) ||
        !is_point(shot->target))
        return false;
    double x = (double)shot->target[0] - (double)shot->flash[0], y = (double)shot->target[1] - (double)shot->flash[1],
           z = (double)shot->target[2] - (double)shot->flash[2];
    double between = x * x + y * y + z * z;
    if (!(between >= Target_Nearest) || !(Target_Farthest >= between))
        return false;
    return Flash_Farthest >= squared_distance(origin, muzzle) &&
           Flash_Farthest >= squared_distance(origin, shot->flash);
}

bool aae_freeaim_match(aae_freeaim_tracers_t *kept, const float origin[3], const float start[3],
                       const float muzzle[3], uint64_t now, float flash[3], float target[3]) {
    if (!is_point(muzzle) || !is_point(origin) || !is_point(start))
        return false;
    aae_freeaim_shot_t *found = NULL;
    for (uint32_t back = 1; back <= AAE_FREEAIM_SHOTS && found == NULL; back++) {
        aae_freeaim_shot_t *shot = &kept->shots[(kept->count - back) % AAE_FREEAIM_SHOTS]; // the newest first
        if (shot_findable(shot, now) && shot_fits(shot, origin, muzzle))
            found = shot;
    }
    if (found != NULL)
        found->used = true;
    kept->pending = false;
    for (int i = 0; i < AAE_FREEAIM_SHOTS; i++)
        if (shot_findable(&kept->shots[i], now))
            kept->pending = true;
    if (found == NULL)
        return false;
    memcpy(flash, found->flash, sizeof(found->flash));
    memcpy(target, found->target, sizeof(found->target));
    return true;
}

// ---- the game's side
#define Cg_Size 0x342a80
#define Cg_ThirdPerson 0x11a8c9  // a byte: the view is a third person one
#define Cg_PlayerState 0x11a8d0
#define Ps_Kind 0x4
#define Ps_Sights 0x2fc          // how far the sights are up, 0 to 1
#define Frame_Sights (-0x50)     // the crosshair function's own copy of that, from its frame pointer
#define Weapon_LeastGap 0x6f0    // the weapon's definition: the least gap of its crosshair, an int
#define Dvar_Type 0x1c
#define Dvar_Type_String 8
#define Entity_Client 0x250      // gentity_s: the player's state on the server (gclient_s), or NULL
#define Ps_ThirdPerson 0xbc      // a byte of the player state: the player's view is a third person one
#define Shot_Muzzle 0x24         // a shot's parameters: forward, right and up (three floats each), then the muzzle
#define Entity_Origin 0x1c       // gentity_s: where its way begins (three floats)
#define Entity_Velocity 0x28     // and how fast it goes along it
#define Entity_Angles 0x23c      // pitch, yaw and roll
#define Max_Players 64
#define Client_Size 0xe5270      // client_t, the server's record of a connected client (0xe5170 on Windows)

static uint64_t base_address;
static int debug;
static void *(*Dvar_FindByHash)(uint32_t hash);
static const char *(*Dvar_GetString)(void *dvar);
static uint8_t *const *cg_clients;
static const void *crosshair_asks_from; // where the crosshair code's own call of the test returns to
static aae_freeaim_client_t clients[Local_Clients];

static uint64_t uptime_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}
static uint64_t (*now_ms)(void) = uptime_ms;

// A dvar's text, when it is one that has text (the plugin only ever reads those): NULL for anything else and for
// no dvar. *there says whether the dvar exists.
static const char *dvar_text(const char *name, bool *there) {
    const uint8_t *dvar = Dvar_FindByHash(gsc_hash(name));
    int type = -1;
    if (dvar != NULL)
        memcpy(&type, dvar + Dvar_Type, sizeof(type));
    if (there != NULL)
        *there = dvar != NULL;
    return type == Dvar_Type_String ? Dvar_GetString((void *)dvar) : NULL;
}

// One of the two switches. The plugin makes them with "1" when it starts; here there is none until somebody sets
// one, and none counts as that 1. One that can't be read as a number counts as "otherwise".
static float switch_dvar(const char *name, float otherwise) {
    bool there;
    const char *text = dvar_text(name, &there);
    if (!there)
        return 1.0f;
    return text != NULL ? aae_freeaim_switch(text, otherwise) : otherwise;
}

// The crosshair code runs on the client's thread and shots are fired on the server's: what is kept for the local
// clients is only looked at and changed under this.
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

// what the client's side of a shot (further down) does whenever the rule is asked
static void tracers_view(int which, const uint8_t *cg);

static bool on_for(int which, const uint8_t *cg) {
    uint32_t number;
    int kind;
    memcpy(&number, cg, sizeof(number));
    memcpy(&kind, cg + Cg_PlayerState + Ps_Kind, sizeof(kind));
    const char *map = dvar_text("mapname", NULL);
    float enabled = switch_dvar("aae_freeaim_enabled", 0.0f);
    float own = switch_dvar(which == 0 ? "aae_freeaim_enabled" : "aae_freeaim_enabled_1", 1.0f);
    pthread_mutex_lock(&clients_lock);
    bool on = aae_freeaim_on(&clients[which], now_ms(), number, cg[Cg_ThirdPerson] != 0, kind, map, enabled, own);
    pthread_mutex_unlock(&clients_lock);
    tracers_view(which, cg);
    return on;
}

// which local client's cg_t this is, -1 for anything else (counted like this, something from before them is far
// behind them)
static int local_client_of(const uint8_t *cg) {
    const uint8_t *first = *cg_clients;
    uintptr_t offset = (uintptr_t)cg - (uintptr_t)first;
    if (cg == NULL || first == NULL || offset % Cg_Size != 0 || offset / Cg_Size >= Local_Clients)
        return -1;
    return (int)(offset / Cg_Size);
}

// The cg_t the crosshair code was last at work with for this local client, or NULL: for never, and for one that
// isn't this client's cg_t in the game's array any more (the plugin reads such a pointer with a call that fails for
// memory that is gone; here it isn't read at all).
static const void *seen_cg(int which) {
    pthread_mutex_lock(&clients_lock);
    const void *cg = clients[which].cg;
    pthread_mutex_unlock(&clients_lock);
    return local_client_of(cg) == which ? cg : NULL;
}
static bool on_now(const uint8_t *cg) {
    int which = local_client_of(cg);
    if (which < 0 || seen_cg(which) != cg || !aae_plugin_loaded_lately())
        return false;
    return on_for(which, cg);
}

// a line the first time each of the two places changes something (BO3MACFIX_FREEAIMDEBUG=1)
static void said_once(int which, const char *what) {
    static _Atomic unsigned char said[2];
    if (debug && !atomic_exchange_explicit(&said[which], 1, memory_order_relaxed))
        printf("free aim: %s (the first time)\n", what);
}

// "cmp bl, 0 ; jne": behind the crosshair's colour and its test for being visible at all, where the tests for the
// sights and for what the weapon is doing begin. The cg_t is in r13.
detour_choice(Crosshair_Tests, "mov %r13, %rdi\n mov %rbp, %rsi\n") {
    uint8_t *frame = (uint8_t *)second;
    if (!on_now((const uint8_t *)first))
        return false;
    const float down = 0.0f;
    memcpy(frame + Frame_Sights, &down, sizeof(down));
    said_once(0, "the crosshair is drawn whatever the sights and the weapon are doing");
    return true;
}

// "movss xmm0, [r12 + 0xe0c]": the gap is the larger one of the spread's and the weapon's least, in xmm1. The
// weapon's definition is in r12, the cg_t in r13.
detour_with_registers(Crosshair_Gap) {
    const uint8_t *cg = (const uint8_t *)registers->r13, *weapon = (const uint8_t *)registers->r12;
    if (!on_now(cg))
        return;
    float gap, sights;
    int32_t least;
    memcpy(&gap, registers->xmm[1], sizeof(gap));
    memcpy(&least, weapon + Weapon_LeastGap, sizeof(least));
    memcpy(&sights, cg + Cg_PlayerState + Ps_Sights, sizeof(sights));
    gap = aae_freeaim_gap(gap, (float)least, sights);
    memcpy(registers->xmm[1], &gap, sizeof(gap));
    said_once(1, "the crosshair's gap goes by how far the sights are up");
}

// the game's test for drawing the crosshair at all, (cg_t *, playerState_s *), which is hooked below
static bool (*orig_CG_DrawCrosshairTest)(uint8_t *cg, uint8_t *ps);

// What the test is to answer. from_the_crosshair: the crosshair function is the one asking, the only one that gets
// another answer than the game's.
static bool crosshair_test(uint8_t *cg, uint8_t *ps, bool from_the_crosshair) {
    bool draw = orig_CG_DrawCrosshairTest(cg, ps);
    if (!from_the_crosshair || cg == NULL || !aae_plugin_loaded_lately())
        return draw;
    int which = local_client_of(cg);
    if (which < 0)
        return draw;
    pthread_mutex_lock(&clients_lock);
    aae_freeaim_seen(&clients[which], cg, now_ms());
    pthread_mutex_unlock(&clients_lock);
    bool on = on_for(which, cg);
    if (debug) {
        static _Atomic int said = -1;
        if (atomic_exchange(&said, on) != (int)on)
            printf("free aim: %s for local client %d (the game's own answer for a crosshair: %s)\n", on ? "on" : "off",
                   which, draw ? "yes" : "no");
    }
    return draw || on;
}

static bool fake_CG_DrawCrosshairTest(uint8_t *cg, uint8_t *ps) {
    return crosshair_test(cg, ps, __builtin_return_address(0) == crosshair_asks_from);
}

// ---- where the shots go: the server's side
//
// The mod's script on the client works out the point in the world that is under the crosshair and leaves it in a
// dvar, as "x y z": aae_freeaim_target (aae_freeaim_target_1 for the second local client). The server fires a bullet
// with three directions (forward, right, up) from a muzzle. For a player whose view is third person, the plugin
// turns those so that forward points from the muzzle at that point, fires, and puts them back.
// Rockets, grenades and other projectiles are entities the server makes, each with a place and a velocity. Right
// after one is made for such a player the plugin turns its velocity at the point, from where the thing is, and
// gives it the angles of its new way (unless the dvar aae_freeaim_nonbullet is 0).
// For a player on this machine the point is read straight from the dvar. A player on another machine sends theirs
// to the server (aae_freeaim_net.c), and a point that came that way goes first.

// A dvar's text, copied out whole. The mod's script sets the point's dvar on the client's thread whenever the aim
// moves, and the server's thread reads it for every shot: the text can be replaced while it is read. The game puts
// the new text into the dvar before it lets go of the old one (0x100fa114f, then 0x100fa1190), so a copy is whole
// when the dvar still hands out the same text after the copy was made; when it doesn't, the copy is made again from
// the new one. (The plugin parses a copy as well, of the same 192 bytes.) False for no text, for one that doesn't fit,
// and for one that was replaced every time.
#define Text_Copy_Size 192
#define Text_Copy_Tries 4
static bool dvar_text_copy(const char *name, char copy[Text_Copy_Size]) {
    for (int tries = 0; tries < Text_Copy_Tries; tries++) {
        const char *text = dvar_text(name, NULL);
        size_t length = 0;
        if (text == NULL)
            return false;
        while (length < Text_Copy_Size && (copy[length] = text[length]) != '\0')
            length++;
        if (dvar_text(name, NULL) == text)
            return length < Text_Copy_Size;
    }
    return false;
}

// the point under the crosshair of a local client, as the mod's script left it
static bool local_target(int which, float target[3]) {
    char text[Text_Copy_Size];
    return dvar_text_copy(which == 0 ? "aae_freeaim_target" : "aae_freeaim_target_1", text) &&
           aae_freeaim_target(text, target);
}

// the number of the player this entity is, when their view is a third person one
static bool third_person_player(const uint8_t *entity, uint32_t *number) {
    const uint8_t *player;
    memcpy(number, entity, sizeof(*number));
    memcpy(&player, entity + Entity_Client, sizeof(player));
    return player != NULL && player[Ps_ThirdPerson] != 0 && *number < Max_Players;
}

// The point this player's shots are to go to. A player whose machine sent one: that point, unless the dvar
// aae_freeaim_enabled is 0 or the lookup says no (the client went away, the map isn't a zombies one), and then none
// at all: the plugin doesn't look at the local clients for such a player. Anybody else: the point of the first
// local client that is this player, while free aim is on for them.
static bool target_of(uint32_t number, float target[3]) {
    if (aae_freeaim_net_remote_entry(number))
        return switch_dvar("aae_freeaim_enabled", 0.0f) != 0.0f && aae_freeaim_net_remote_target(number, target);
    for (int which = 0; which < Local_Clients; which++) {
        const uint8_t *cg = seen_cg(which);
        uint32_t cg_number;
        if (cg == NULL)
            continue;
        memcpy(&cg_number, cg, sizeof(cg_number));
        if (cg_number == number)
            return on_for(which, cg) && local_target(which, target);
    }
    return false;
}

// whether a debug line is due: for the first shot of its kind and for every one at another point than the last
// one's (only the server's thread comes here)
static bool worth_a_line(unsigned *count, float last[3], const float target[3]) {
    bool elsewhere = fabsf(target[0] - last[0]) + fabsf(target[1] - last[1]) + fabsf(target[2] - last[2]) > 1.0f;
    memcpy(last, target, 3 * sizeof(float));
    return ++*count == 1 || elsewhere;
}

// The directions a shot of this player is to be fired with instead of its own, if any.
static bool turned_shot(const uint8_t *attacker, const uint8_t *shot, float turned[9]) {
    uint32_t number;
    float target[3], directions[9], muzzle[3];
    if (attacker == NULL || shot == NULL || !aae_plugin_loaded_lately())
        return false;
    if (!third_person_player(attacker, &number) || !target_of(number, target))
        return false;
    memcpy(directions, shot, sizeof(directions));
    memcpy(muzzle, shot + Shot_Muzzle, sizeof(muzzle));
    if (!aae_freeaim_turn(muzzle, directions, target, turned))
        return false;
    if (debug) {
        static unsigned count;
        static float last[3];
        if (worth_a_line(&count, last, target))
            printf("free aim: shot %u of player %u goes to %.1f %.1f %.1f: from %.1f %.1f %.1f, forward %.3f %.3f "
                   "%.3f instead of %.3f %.3f %.3f\n", count, number, target[0], target[1], target[2], muzzle[0],
                   muzzle[1], muzzle[2], turned[0], turned[1], turned[2], directions[0], directions[1], directions[2]);
    }
    return true;
}

// the server fires a bullet: (the player's entity, the shot's parameters, ...), with the spread as a float
install_hook_name(Bullet_Fire, void, uint8_t *attacker, uint8_t *shot, uint64_t third, uint64_t fourth, uint64_t fifth,
                  uint64_t sixth, float spread) {
    float turned[9], own[9];
    if (!turned_shot(attacker, shot, turned)) {
        orig_Bullet_Fire(attacker, shot, third, fourth, fifth, sixth, spread);
        return;
    }
    memcpy(own, shot, sizeof(own));
    memcpy(shot, turned, sizeof(turned));
    orig_Bullet_Fire(attacker, shot, third, fourth, fifth, sixth, spread);
    memcpy(shot, own, sizeof(own));
}

// The name the mod's script asks the game for a weapon by (GetWeapon's): the second name in the weapon's entry of
// the game's table, which a Weapon's low nine bits are the index of. NULL for no weapon.
#define Weapon_Index 0x1ff
static const uint8_t *const *weapon_variants;
static const int32_t *last_weapon;
static const char *weapon_name(uint64_t weapon) {
    uint32_t index = (uint32_t)(weapon & Weapon_Index);
    const uint8_t *variant;
    const char *name;
    if (weapon_variants == NULL || index == 0 || (int32_t)index > *last_weapon)
        return NULL;
    variant = weapon_variants[index];
    if (variant == NULL)
        return NULL;
    memcpy(&name, variant + WeaponVariantDef_ScriptName, sizeof(name));
    return name;
}

// Something that flies was made for this entity: turned at the point its player aims at, if any. Not the landing
// explosion of the mod's PhD Flopper, which is no shot and belongs where the player lands (weapon: the missile's, 0
// for a grenade, which that explosion never is).
enum { Kind_Grenade, Kind_Rocket, Kind_Projectile, Kinds };
static void turn_projectile(const uint8_t *owner, uint8_t *made, uint64_t weapon, int kind) {
    static const char *const names[Kinds] = { "grenade", "rocket", "projectile" };
    uint32_t number;
    float target[3], origin[3], velocity[3], turned[3], angles[3];
    if (owner == NULL || made == NULL || !aae_plugin_loaded_lately() || !third_person_player(owner, &number))
        return;
    if (switch_dvar("aae_freeaim_nonbullet", 1.0f) == 0.0f || !target_of(number, target))
        return;
    memcpy(origin, made + Entity_Origin, sizeof(origin));
    memcpy(velocity, made + Entity_Velocity, sizeof(velocity));
    if (aae_freeaim_landing_blast(weapon_name(weapon), velocity)) {
        if (debug)
            printf("free aim: a %s of player %u is left alone, PhD Flopper's landing explosion: %s from %.1f %.1f "
                   "%.1f, velocity %.1f %.1f %.1f, not to %.1f %.1f %.1f\n", names[kind], number, weapon_name(weapon),
                   origin[0], origin[1], origin[2], velocity[0], velocity[1], velocity[2], target[0], target[1],
                   target[2]);
        return;
    }
    if (!aae_freeaim_projectile(origin, velocity, target, turned, angles))
        return;
    memcpy(made + Entity_Velocity, turned, sizeof(turned));
    memcpy(made + Entity_Angles, angles, sizeof(angles));
    if (debug) {
        static unsigned count[Kinds];
        static float last[Kinds][3];
        const char *name = weapon_name(weapon);
        if (worth_a_line(&count[kind], last[kind], target))
            printf("free aim: %s %u of player %u goes to %.1f %.1f %.1f: from %.1f %.1f %.1f, velocity %.1f %.1f %.1f "
                   "instead of %.1f %.1f %.1f, angles %.1f %.1f%s%s\n", names[kind], count[kind], number, target[0],
                   target[1], target[2], origin[0], origin[1], origin[2], turned[0], turned[1], turned[2], velocity[0],
                   velocity[1], velocity[2], angles[0], angles[1], name != NULL ? ", weapon " : "",
                   name != NULL ? name : "");
    }
}

// The server makes a grenade, a rocket or another projectile: each of the three functions gets the entity that
// fires first and gives the new entity. The rest is handed on as it came (in registers and on the stack); a rocket's
// and a projectile's second one is the weapon.
install_hook_name(Grenade_Spawn, uint8_t *, uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth, uint64_t seventh) {
    uint8_t *made = orig_Grenade_Spawn(owner, second, third, fourth, fifth, sixth, seventh);
    turn_projectile(owner, made, 0, Kind_Grenade);
    return made;
}
install_hook_name(Rocket_Spawn, uint8_t *, uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth, uint64_t seventh) {
    uint8_t *made = orig_Rocket_Spawn(owner, second, third, fourth, fifth, sixth, seventh);
    turn_projectile(owner, made, second, Kind_Rocket);
    return made;
}
install_hook_name(Projectile_Spawn, uint8_t *, uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth, uint64_t seventh, uint64_t eighth) {
    uint8_t *made = orig_Projectile_Spawn(owner, second, third, fourth, fifth, sixth, seventh, eighth);
    turn_projectile(owner, made, second, Kind_Projectile);
    return made;
}

// ---- players on other machines: what aae_freeaim_net.c asks of this file, and its two hooks

// The plugin's "is free aim on for this local client", as its hook on CL_FinishMove asks it every frame. For a
// client the crosshair code never was at work for, the plugin's rule still runs and notes "not third person".
static bool net_on(int which, uint64_t now) {
    const uint8_t *cg = seen_cg(which);
    if (cg != NULL)
        return on_for(which, cg);
    pthread_mutex_lock(&clients_lock);
    aae_freeaim_on(&clients[which], now, UINT32_MAX, false, 0, NULL, 0.0f, 0.0f);
    pthread_mutex_unlock(&clients_lock);
    return false;
}
static const char *net_map(void) {
    return dvar_text("mapname", NULL);
}
static uint64_t net_now(void) {
    return now_ms();
}
// a debug line for the first few of something that can come many times a second, and for every 200th after
static bool few(unsigned *count) {
    return debug && (++*count <= 5 || *count % 200 == 0);
}
static void (*CL_AddReliableCommand)(int local_client, const char *text);
static void queue_command(int local_client, const char *text) {
    static unsigned count;
    if (few(&count))
        printf("free aim: local client %d tells the server \"%s\" (%u so far)\n", local_client, text, count);
    CL_AddReliableCommand(local_client, text);
}
static aae_freeaim_net_game_t net_game = { .active = aae_plugin_loaded_lately, .now_ms = net_now, .map = net_map,
                                           .on = net_on, .local_target = local_target,
                                           .add_reliable_command = queue_command, .client_size = Client_Size };

// The client has put together what it sends the server for this frame: (the local client, the command). The
// plugin's work comes behind the game's.
install_hook_name(CL_FinishMove, void, int local_client, void *command) {
    orig_CL_FinishMove(local_client, command);
    aae_freeaim_net_client_frame(local_client);
}

// The server got a command from a client: (the client's record, the text, two switches). "aae ..." is the plugin's
// and doesn't reach the game.
install_hook_name(SV_ExecuteClientCommand, void, uint8_t *client, const char *text, int client_ok,
                  int from_old_server) {
    if (aae_freeaim_net_server_command(client, text, from_old_server)) {
        static unsigned count;
        if (few(&count))
            printf("free aim: a client's \"%s\" was taken (%u so far)\n", text, count);
        return;
    }
    orig_SV_ExecuteClientCommand(client, text, client_ok, from_old_server);
}

// ---- the client's side of a shot
//
// The shooting player's own client works out a copy of each of its bullets, to draw what the player sees of it: the
// sparks, marks and blood where it hits, and its tracer. The server's copy is the one that does damage, and that one
// is turned at the point under the crosshair (above). The client's copy went straight ahead from the eye, so in
// third person, where the camera looks from beside the player, the impacts the player saw were beside the crosshair,
// about as far beside it as the camera is (and a zombie under the crosshair could be hit without any blood shown).
// The plugin hooks the function that works out where the client's copy ends (from where the shot starts, along its
// forward, spread with its right and up) and turns those three directions as the server's are turned, at the same
// point, for the shooting player of a local client whose free aim is on. It does that only when the function is
// called from one of the two calls in the client's fire function, whose frame holds the shooting player's entity: the
// plugin's wrapper hands its hook that frame for those two calls and for no others.

#define Cent_Number 0x3f8       // centity_s: the entity's number (a player's is the client number)
#define Fire_Frame_Cent (-0x58) // the client's fire function keeps the shooting entity's centity_s here, from its
                                // frame pointer

static const void *endpos_asked_from[2]; // where the client's fire function goes on after each of its two calls

// What is kept of each local client's turned shots for their tracers. The hook here and the drawing of queued shots
// run on the client's thread; the rule for free aim being on, which forgets about them, runs on the server's too.
static pthread_mutex_t tracers_lock = PTHREAD_MUTEX_INITIALIZER;
static aae_freeaim_tracers_t tracers[Local_Clients];

// The plugin forgets that a queued shot of this client may find a kept one whenever it sees that the client's view
// isn't a third person one: whenever it is asked whether free aim is on (and when the crosshair code is at work,
// which asks that here).
static void tracers_view(int which, const uint8_t *cg) {
    if (cg != NULL && cg[Cg_ThirdPerson] != 0)
        return;
    pthread_mutex_lock(&tracers_lock);
    tracers[which].pending = false;
    pthread_mutex_unlock(&tracers_lock);
}

static void *(*Com_GetClientDObj)(int number, int local_client);
static int (*CG_DObjGetWorldTagPos)(const uint8_t *cent, void *object, uint32_t tag, float position[3]);
static const uint32_t *tag_flash_name; // the game's own "tag_flash", made when the client's game starts (0 before)

// Where the gun's tag_flash on this entity's model is in the world now (the plugin's FUN_180023290: it makes the
// name itself, the same one as the game's; it also wants the entity's number below 0x800, which a player's is).
static bool tag_flash_of(int which, const uint8_t *cent, float flash[3]) {
    uint32_t number, tag = *tag_flash_name;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    memcpy(&number, cent + Cent_Number, sizeof(number));
    if (tag == 0)
        return false;
    void *object = Com_GetClientDObj((int)number, which);
    if (object == NULL || !CG_DObjGetWorldTagPos(cent, object, tag, position) || !is_point(position))
        return false;
    memcpy(flash, position, sizeof(position));
    return true;
}

// The plugin's "is free aim on for this local client" (FUN_1800229f0) by the client alone. For a client the
// crosshair code was never at work for, its rule still runs and notes "not third person" (net_on does that).
static bool on_for_client(int which) {
    const uint8_t *cg = seen_cg(which);
    if (cg != NULL)
        return on_for(which, cg);
    tracers_view(which, NULL);
    return net_on(which, now_ms());
}

// Which local client this player is, if free aim is on for it, or -1 (the plugin's FUN_180022df0). The plugin asks
// its table of points other machines sent first: a player who has an entry there isn't a local client's, whatever
// else the entry says, and nothing of theirs is turned on this side. Otherwise the first local client whose cg_t has
// this client number decides.
static uint32_t client_number(const uint8_t *cg) {
    uint32_t number;
    memcpy(&number, cg, sizeof(number));
    return number;
}
static int local_client_of_player(uint32_t number) {
    if (aae_freeaim_net_remote_entry(number))
        return -1;
    for (int which = 0; which < Local_Clients; which++) {
        const uint8_t *cg = seen_cg(which);
        if (cg != NULL && client_number(cg) == number)
            return on_for(which, cg) ? which : -1;
    }
    return -1;
}

// whether free aim is on for any local client: the plugin's first question, before it looks at the shot at all
static bool on_for_any(void) {
    for (int which = 0; which < Local_Clients; which++)
        if (on_for_client(which))
            return true;
    return false;
}

// The directions the client's copy of a shot is to be worked out with instead of its own, if any (the plugin's
// FUN_18001eff0). caller: where the call returns to in the game; frame: the frame pointer the caller had.
static bool turned_client_shot(const void *caller, const uint8_t *frame, const float *start, const float *forward,
                               const float *right, const float *up, float turned[9]) {
    const uint8_t *cent;
    uint32_t number;
    if (caller != endpos_asked_from[0] && caller != endpos_asked_from[1])
        return false;
    if (!aae_plugin_loaded_lately() || !on_for_any() || frame == NULL)
        return false;
    memcpy(&cent, frame + Fire_Frame_Cent, sizeof(cent));
    if (cent == NULL)
        return false;
    memcpy(&number, cent + Cent_Number, sizeof(number));
    if (debug) {
        static _Atomic bool said;
        if (!atomic_exchange(&said, true))
            printf("free aim: the client's fire function's frame is at %p, its shooting entity %p is number %u (the "
                   "first time)\n", (const void *)frame, (const void *)cent, number);
    }
    if (number >= Max_Players)
        return false;
    // (the plugin asks whether free aim is on for that client once more: each time with the clock as it is then)
    int which = local_client_of_player(number);
    if (which < 0 || !on_for_client(which))
        return false;
    if (start == NULL || forward == NULL || right == NULL || up == NULL)
        return false;
    float from[3], directions[9], target[3];
    memcpy(from, start, sizeof(from));
    memcpy(directions, forward, 3 * sizeof(float));
    memcpy(directions + 3, right, 3 * sizeof(float));
    memcpy(directions + 6, up, 3 * sizeof(float));
    if (!is_point(from))
        return false;
    // Where the gun's flash is, to keep with the shot for its tracer, if free aim is still on for the client: found on
    // the model now, or where it was found within the last 250 ms.
    float flash[3];
    bool keep = false;
    if (on_for_client(which)) {
        float found[3];
        bool fresh = tag_flash_of(which, cent, found);
        pthread_mutex_lock(&tracers_lock);
        keep = aae_freeaim_flash(&tracers[which], from, fresh ? found : NULL, now_ms(), flash);
        pthread_mutex_unlock(&tracers_lock);
        if (debug) {
            static unsigned count; // only the client's thread comes here
            if (few(&count))
                printf("free aim: the gun's tag_flash for the client's shot %u of player %u: %s, at %.1f %.1f %.1f "
                       "(%s)\n", count, number, fresh ? "on the model" : "not on the model", keep ? flash[0] : 0.0f,
                       keep ? flash[1] : 0.0f, keep ? flash[2] : 0.0f,
                       keep ? (fresh ? "kept with the shot" : "the one found last, kept with the shot")
                            : "nothing kept with the shot");
        }
    }
    if (!local_target(which, target) || !aae_freeaim_turn(from, directions, target, turned))
        return false;
    if (keep) {
        pthread_mutex_lock(&tracers_lock);
        aae_freeaim_keep(&tracers[which], from, flash, target, now_ms());
        pthread_mutex_unlock(&tracers_lock);
    }
    if (debug) {
        static unsigned count; // only the client's thread comes here
        static float last[3];
        if (worth_a_line(&count, last, target))
            printf("free aim: the client's shot %u of player %u goes to %.1f %.1f %.1f: from %.1f %.1f %.1f, forward "
                   "%.3f %.3f %.3f instead of %.3f %.3f %.3f (%s call)\n", count, number, target[0], target[1],
                   target[2], from[0], from[1], from[2], turned[0], turned[1], turned[2], directions[0], directions[1],
                   directions[2], caller == endpos_asked_from[0] ? "first" : "second");
    }
    return true;
}

// The client works out where its own copy of a shot ends and its direction: (seed, two spreads, start, end out,
// direction out, two fractions, forward, right, range, up, weapon, the pellet and how many there are), the floats
// in registers of their own. Everything is handed on as it came, with the turned directions when there are.
static void client_shot(const void *caller, const uint8_t *frame, uint64_t seed, float spread, float spread2,
                        float *start, uint64_t end, uint64_t direction, float fraction, float fraction2, float *forward,
                        float *right, float range, float *up, uint64_t weapon, uint64_t pellet, uint64_t pellets);
install_hook_name(Bullet_Endpos, void, uint64_t seed, float spread, float spread2, float *start, uint64_t end,
                  uint64_t direction, float fraction, float fraction2, float *forward, float *right, float range,
                  float *up, uint64_t weapon, uint64_t pellet, uint64_t pellets) {
    // The caller's frame pointer is what this function saved first: the jump from the game to here pushed nothing.
    const uint8_t *frame = *(uint8_t *const *)__builtin_frame_address(0);
    client_shot(__builtin_return_address(0), frame, seed, spread, spread2, start, end, direction, fraction, fraction2,
                forward, right, range, up, weapon, pellet, pellets);
}
static void client_shot(const void *caller, const uint8_t *frame, uint64_t seed, float spread, float spread2,
                        float *start, uint64_t end, uint64_t direction, float fraction, float fraction2, float *forward,
                        float *right, float range, float *up, uint64_t weapon, uint64_t pellet, uint64_t pellets) {
    float turned[9];
    if (turned_client_shot(caller, frame, start, forward, right, up, turned))
        forward = turned, right = turned + 3, up = turned + 6;
    orig_Bullet_Endpos(seed, spread, spread2, start, end, direction, fraction, fraction2, forward, right, range, up,
                       weapon, pellet, pellets);
}

// ---- the client's side of a shot: the tracer from the gun
//
// Most bullets the client works out are queued, traced by a job of their own, and their effects drawn together
// later: the impacts, and the tracer from the point the game took for it, in third person one 30 units in front of
// the player's eyes. The plugin keeps each shot it turned with where the gun's tag_flash was, and right before a
// queued shot's effects are drawn it finds the kept shot it is and starts the tracer at the gun: the plugin's own
// stub in front of the game's function for one queued shot calls it, for local client 0 or 1, while a queued shot of
// that client may find a kept one. On the Mac that function is part of the loop over the queued shots, so this is a
// detour at the loop's head. The shot has been traced by then, so where it hits stays as it is. The plugin also
// writes where the shot was fired from and where it starts (the gun, and the point it was turned at) and puts all
// three back after the drawing; nothing reads the first two in the drawing on the Mac, and nothing reads a queued
// shot after it, up to the next shot queued there, which writes them all anew. So the first two are written as the
// plugin does and nothing is put back.

#define Queue_Entry_Size 0x8b0 // a queued shot: the local client's number (an int) first,
#define Queued_Shot 0x4        // the shot's parameters,
#define Shot_Origin 0x10       //   in those: where it was fired from,
#define Shot_Start 0x1c        //   and where it starts,
#define Queued_Number 0x58     // the shooting entity's number,
#define Queued_Muzzle 0x64     // where its tracer starts

// A queued shot's effects are about to be drawn (the plugin's FUN_180023bd0).
static void queued_shot(uint8_t *entry) {
    int32_t local_client;
    uint32_t number;
    memcpy(&local_client, entry, sizeof(local_client));
    if ((uint32_t)local_client >= Local_Clients)
        return;
    pthread_mutex_lock(&tracers_lock);
    bool pending = tracers[local_client].pending;
    pthread_mutex_unlock(&tracers_lock);
    if (!pending || !aae_plugin_loaded_lately() || !on_for_client(local_client))
        return;
    memcpy(&number, entry + Queued_Number, sizeof(number));
    if (number >= Max_Players || local_client_of_player(number) != local_client)
        return;
    uint8_t *shot = entry + Queued_Shot;
    float origin[3], start[3], muzzle[3], flash[3], target[3];
    memcpy(origin, shot + Shot_Origin, sizeof(origin));
    memcpy(start, shot + Shot_Start, sizeof(start));
    memcpy(muzzle, entry + Queued_Muzzle, sizeof(muzzle));
    pthread_mutex_lock(&tracers_lock);
    bool found = aae_freeaim_match(&tracers[local_client], origin, start, muzzle, now_ms(), flash, target);
    pthread_mutex_unlock(&tracers_lock);
    if (debug) {
        static unsigned count; // only the client's thread comes here
        if (few(&count))
            printf("free aim: queued shot %u of player %u: %s %.1f %.1f %.1f, the gun at %.1f %.1f %.1f\n", count,
                   number, found ? "its tracer starts at the gun instead of" : "no kept shot fits, the tracer from",
                   muzzle[0], muzzle[1], muzzle[2], found ? flash[0] : 0.0f, found ? flash[1] : 0.0f,
                   found ? flash[2] : 0.0f);
    }
    if (!found)
        return;
    memcpy(shot + Shot_Origin, flash, sizeof(flash));
    memcpy(shot + Shot_Start, target, sizeof(target));
    memcpy(entry + Queued_Muzzle, flash, sizeof(flash));
}

// "imul rcx, rsi, 0x8b0": the head of the loop over the queued shots, after they have been traced. The queue's
// entries are at r13, this shot's index is in rsi.
detour_with_registers(Queued_Effects) {
    queued_shot((uint8_t *)registers->r13 + registers->rsi * Queue_Entry_Size);
}

void aae_freeaim_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_FREEAIMDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_GetString = (void *)(base_address + ADDR_Dvar_GetString);
    cg_clients = (uint8_t *const *)(base_address + ADDR_cgArray);
    crosshair_asks_from = (const void *)(base_address + ADDR_CG_DrawCrosshair_TestReturn);
    DobbyHook((void *)(base_address + ADDR_CG_DrawCrosshairTest), (void *)fake_CG_DrawCrosshairTest,
              (void **)&orig_CG_DrawCrosshairTest);
    install_hook_Bullet_Fire((void *)(base_address + ADDR_Bullet_Fire));
    install_hook_Grenade_Spawn((void *)(base_address + ADDR_Grenade_Spawn));
    install_hook_Rocket_Spawn((void *)(base_address + ADDR_Rocket_Spawn));
    install_hook_Projectile_Spawn((void *)(base_address + ADDR_Projectile_Spawn));
    weapon_variants = (const uint8_t *const *)(base_address + ADDR_bg_weaponVariantDefs);
    last_weapon = (const int32_t *)(base_address + ADDR_bg_lastWeaponIndex);
    CL_AddReliableCommand = (void *)(base_address + ADDR_CL_AddReliableCommand);
    net_game.server_state = (const int32_t *)(base_address + ADDR_sv_state);
    net_game.client_connections = (uint8_t *const *)(base_address + ADDR_clientConnections);
    net_game.server_clients = (uint8_t *const *)(base_address + ADDR_svs_clients);
    aae_freeaim_net_init(&net_game);
    install_hook_CL_FinishMove((void *)(base_address + ADDR_CL_FinishMove));
    install_hook_SV_ExecuteClientCommand((void *)(base_address + ADDR_SV_ExecuteClientCommand));
    // the client's own copy of a shot
    endpos_asked_from[0] = (const void *)(base_address + ADDR_Bullet_Endpos_Return1);
    endpos_asked_from[1] = (const void *)(base_address + ADDR_Bullet_Endpos_Return2);
    install_hook_Bullet_Endpos((void *)(base_address + ADDR_Bullet_Endpos));
    Com_GetClientDObj = (void *)(base_address + ADDR_Com_GetClientDObj);
    CG_DObjGetWorldTagPos = (void *)(base_address + ADDR_CG_DObjGetWorldTagPos);
    tag_flash_name = (const uint32_t *)(base_address + ADDR_scr_tag_flash);
    if (!prepatch_has_hook(ADDR_CG_DrawCrosshair_Tests) || !prepatch_has_hook(ADDR_CG_DrawCrosshair_Gap)) {
        printf("free aim: not running from the pre-patched executable: the third person crosshair goes away while "
               "aiming down the sights, as the game's own does\n");
        return;
    }
    install_detour_Crosshair_Tests((void *)(base_address + ADDR_CG_DrawCrosshair_Tests),
                                   (void *)(base_address + ADDR_CG_DrawCrosshair_Draws));
    install_detour_Crosshair_Gap((void *)(base_address + ADDR_CG_DrawCrosshair_Gap));
    // the tracer of a shot the client turned starts at the gun
    if (prepatch_has_hook(ADDR_Bullet_QueuedShots_Loop))
        install_detour_Queued_Effects((void *)(base_address + ADDR_Bullet_QueuedShots_Loop));
}
