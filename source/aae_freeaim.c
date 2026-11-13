// All-around Enhancement 4's free aim: the plugin's free_aim component.
//
// The game has no crosshair in third person: its test for drawing one says no while the view is a third person one
// (unless the weapon wants one anyway). The mod has a third person view of its own, and its plugin gives it a
// crosshair and makes shots go to what is under it. This file is that, piece by piece: the crosshair, the crosshair
// while aiming, and bullets, rockets, grenades and other projectiles ("where the shots go" further down).
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

// whether free aim is on for the local client this cg_t is of, for the places behind the crosshair's test: the
// client has been seen there with this cg_t
static const void *seen_cg(int which) {
    pthread_mutex_lock(&clients_lock);
    const void *cg = clients[which].cg;
    pthread_mutex_unlock(&clients_lock);
    return cg;
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
// For a player on this machine the point is read straight from the dvar. (A player on another machine sends theirs
// to the server; that part isn't here yet.)

// the point under the crosshair of a local client, as the mod's script left it
static bool local_target(int which, float target[3]) {
    const char *text = dvar_text(which == 0 ? "aae_freeaim_target" : "aae_freeaim_target_1", NULL);
    return text != NULL && aae_freeaim_target(text, target);
}

// the number of the player this entity is, when their view is a third person one
static bool third_person_player(const uint8_t *entity, uint32_t *number) {
    const uint8_t *player;
    memcpy(number, entity, sizeof(*number));
    memcpy(&player, entity + Entity_Client, sizeof(player));
    return player != NULL && player[Ps_ThirdPerson] != 0 && *number < Max_Players;
}

// The point this player's shots are to go to: the one of the first local client that is this player, while free aim
// is on for them.
static bool target_of(uint32_t number, float target[3]) {
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

// Something that flies was made for this entity: turned at the point its player aims at, if any.
enum { Kind_Grenade, Kind_Rocket, Kind_Projectile, Kinds };
static void turn_projectile(const uint8_t *owner, uint8_t *made, int kind) {
    uint32_t number;
    float target[3], origin[3], velocity[3], turned[3], angles[3];
    if (owner == NULL || made == NULL || !aae_plugin_loaded_lately() || !third_person_player(owner, &number))
        return;
    if (switch_dvar("aae_freeaim_nonbullet", 1.0f) == 0.0f || !target_of(number, target))
        return;
    memcpy(origin, made + Entity_Origin, sizeof(origin));
    memcpy(velocity, made + Entity_Velocity, sizeof(velocity));
    if (!aae_freeaim_projectile(origin, velocity, target, turned, angles))
        return;
    memcpy(made + Entity_Velocity, turned, sizeof(turned));
    memcpy(made + Entity_Angles, angles, sizeof(angles));
    if (debug) {
        static const char *const names[Kinds] = { "grenade", "rocket", "projectile" };
        static unsigned count[Kinds];
        static float last[Kinds][3];
        if (worth_a_line(&count[kind], last[kind], target))
            printf("free aim: %s %u of player %u goes to %.1f %.1f %.1f: from %.1f %.1f %.1f, velocity %.1f %.1f %.1f "
                   "instead of %.1f %.1f %.1f, angles %.1f %.1f\n", names[kind], count[kind], number, target[0],
                   target[1], target[2], origin[0], origin[1], origin[2], turned[0], turned[1], turned[2], velocity[0],
                   velocity[1], velocity[2], angles[0], angles[1]);
    }
}

// The server makes a grenade, a rocket or another projectile: each of the three functions gets the entity that
// fires first and gives the new entity. The rest is handed on as it came (in registers and on the stack).
install_hook_name(Grenade_Spawn, uint8_t *, uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth, uint64_t seventh) {
    uint8_t *made = orig_Grenade_Spawn(owner, second, third, fourth, fifth, sixth, seventh);
    turn_projectile(owner, made, Kind_Grenade);
    return made;
}
install_hook_name(Rocket_Spawn, uint8_t *, uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth, uint64_t seventh) {
    uint8_t *made = orig_Rocket_Spawn(owner, second, third, fourth, fifth, sixth, seventh);
    turn_projectile(owner, made, Kind_Rocket);
    return made;
}
install_hook_name(Projectile_Spawn, uint8_t *, uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth, uint64_t seventh, uint64_t eighth) {
    uint8_t *made = orig_Projectile_Spawn(owner, second, third, fourth, fifth, sixth, seventh, eighth);
    turn_projectile(owner, made, Kind_Projectile);
    return made;
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
    if (!prepatch_has_hook(ADDR_CG_DrawCrosshair_Tests) || !prepatch_has_hook(ADDR_CG_DrawCrosshair_Gap)) {
        printf("free aim: not running from the pre-patched executable: the third person crosshair goes away while "
               "aiming down the sights, as the game's own does\n");
        return;
    }
    install_detour_Crosshair_Tests((void *)(base_address + ADDR_CG_DrawCrosshair_Tests),
                                   (void *)(base_address + ADDR_CG_DrawCrosshair_Draws));
    install_detour_Crosshair_Gap((void *)(base_address + ADDR_CG_DrawCrosshair_Gap));
}
