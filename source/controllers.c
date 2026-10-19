// PlayStation controller support.
//
// BO3 statically links SDL 2.0.7, which reads controllers through IOKit and only turns ones with a mapping into
// game controllers. Its built-in list has the DualShock 4 (at version 0x0100 only) but no DualSense, and
// Aspyr's GPad_GetGamepadType always says Xbox, so even a working DualShock 4 shows Xbox button prompts.
//
// The game also only looks for new controllers when asked, and Aspyr's port never asks in a way it understands, so
// a controller connected while it runs gets it asked here (controllers_request_gamepad_scan).
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include "dobby.h"
#include "offsets.h"
#include "controllers.h"
#include "prepatch.h"
#include "gc_bridge.h"

#define Controller_DB_Path "../../../BO3MacFix/gamecontrollerdb.txt"

#define SONY_VENDOR_ID 0x054C

// gamepadType_e
enum {
    GAMEPAD_TYPE_ORBIS = 0,
    GAMEPAD_TYPE_DURANGO = 1,
    GAMEPAD_TYPE_STEAM = 2,
    GAMEPAD_TYPE_COUNT = 3,
};

static uint64_t base_address = 0;
static bool mappings_enabled = false;
static int button_prompts_mode = 0;

// SDL 2.0.7's IOKit driver builds every GUID as bus 0x03 (even over Bluetooth), vendor, product and version.
// The DualSense's buttons and axes come through in the same order as the DualShock 4's.
#define PlayStation_Layout "a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12," \
    "leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11," \
    "righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,"
static const char *playstation_mappings[] = {
    "030000004c050000e60c000000010000,PS5 Controller," PlayStation_Layout,
    "030000004c050000e60c000000000000,PS5 Controller," PlayStation_Layout,
    "030000004c050000f20d000000010000,PS5 Controller," PlayStation_Layout, // DualSense Edge
    "030000004c050000f20d000000000000,PS5 Controller," PlayStation_Layout,
    "030000004c050000c405000000000000,PS4 Controller," PlayStation_Layout,
    "030000004c050000cc09000000000000,PS4 Controller," PlayStation_Layout,
    "030000004c050000a00b000000000000,PS4 Controller," PlayStation_Layout, // DualShock 4 USB wireless adaptor
};
#define PlayStation_Mapping_Count (sizeof(playstation_mappings) / sizeof(playstation_mappings[0]))

// returns the mapping lines of BO3MacFix/gamecontrollerdb.txt joined by newlines, without blank and # lines
static char *read_user_mappings(void) {
    FILE *fp = fopen(Controller_DB_Path, "r");
    if (fp == NULL)
        return NULL;
    char *mappings = NULL;
    size_t len = 0;
    char line[2048];
    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line + strspn(line, " \t");
        if (*p == '\0' || *p == '#')
            continue;
        size_t line_len = strlen(p);
        char *grown = realloc(mappings, len + line_len + 2);
        if (grown == NULL)
            break;
        mappings = grown;
        if (len > 0)
            mappings[len++] = '\n';
        memcpy(mappings + len, p, line_len + 1);
        len += line_len;
    }
    fclose(fp);
    return mappings;
}

static void append_line(char *config, const char *line) {
    if (config[0] != '\0')
        strcat(config, "\n");
    strcat(config, line);
}

// SDL reads SDL_GAMECONTROLLERCONFIG (one mapping per line) right after its built-in list, and a later line
// replaces an earlier one for the same GUID. Ours go first so Steam's and the user's own mappings win.
// With the GameController bridge on, SDL gets none of ours, the bridge reads PlayStation controllers instead.
static void add_sdl_mappings(void) {
    const char *existing = getenv("SDL_GAMECONTROLLERCONFIG");
    char *user_mappings = read_user_mappings();
    size_t count = gc_bridge_enabled() ? 0 : PlayStation_Mapping_Count;
    size_t size = 1;
    for (size_t i = 0; i < count; i++)
        size += strlen(playstation_mappings[i]) + 1;
    if (existing != NULL)
        size += strlen(existing) + 1;
    if (user_mappings != NULL)
        size += strlen(user_mappings) + 1;

    char *config = calloc(1, size);
    if (config == NULL) {
        free(user_mappings);
        return;
    }
    for (size_t i = 0; i < count; i++)
        append_line(config, playstation_mappings[i]);
    if (existing != NULL && existing[0] != '\0')
        append_line(config, existing);
    if (user_mappings != NULL && user_mappings[0] != '\0')
        append_line(config, user_mappings);
    if (config[0] != '\0')
        setenv("SDL_GAMECONTROLLERCONFIG", config, 1);
    if (count > 0)
        printf("BO3MacFix: added %zu PlayStation controller mappings%s\n", count,
               user_mappings != NULL ? " and BO3MacFix/gamecontrollerdb.txt" : "");
    else
        printf("BO3MacFix: no PlayStation controller mappings for SDL, the GameController bridge reads them%s\n",
               user_mappings != NULL ? " (added BO3MacFix/gamecontrollerdb.txt)" : "");
    free(config);
    free(user_mappings);
}

static int hid_int_property(IOHIDDeviceRef device, CFStringRef key) {
    int value = 0;
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    if (ref != NULL && CFGetTypeID(ref) == CFNumberGetTypeID())
        CFNumberGetValue(ref, kCFNumberSInt32Type, &value);
    return value;
}

static bool is_playstation_product(int product) {
    switch (product) {
    case 0x05C4: // DualShock 4
    case 0x09CC: // DualShock 4 (2nd generation)
    case 0x0BA0: // DualShock 4 USB wireless adaptor
    case 0x0CE6: // DualSense
    case 0x0DF2: // DualSense Edge
        return true;
    }
    return false;
}

// Controllers connected while the game runs. The game only looks for XInput controllers in its gpadupdate command,
// which runs at startup and, on Windows, when WM_DEVICECHANGE says devices changed (DBT_DEVNODES_CHANGED); every
// frame it reads only the user indexes it found then. Aspyr's port sends that message without DBT_DEVNODES_CHANGED,
// which the game ignores, and nothing at all for the GameController bridge's controllers, so a controller connected
// later was never picked up. This calls the game's own handler for the message instead.
static void (*GPad_DevicesChanged)(void);
static atomic_bool scan_pending = false;

// up once GPad_Init has registered its dvars, the command buffer the handler writes to comes before that
static bool gamepads_up(void) {
    return GPad_DevicesChanged != NULL && *(void *volatile *)(base_address + ADDR_dvar_gpadEnabled) != NULL;
}

// the XInput user index (player) the game's first gamepad reads, from its port table, -1 for none
static int game_user_index(void) {
    int port = *(const volatile int *)(base_address + ADDR_g_gpads + 4);
    int user_index = port >= 0 && port <= 3 ? *(const volatile int *)(base_address + ADDR_g_gpadPorts + port * 8) : -1;
    return user_index >= 0 && user_index <= 3 ? user_index : -1;
}

int controllers_game_user_index(void) {
    return gamepads_up() ? game_user_index() : -1;
}

// which XInput user indexes (players) the game found, and which one its first gamepad reads
static void log_game_gamepads(void *context) {
    const volatile int *ports = (const volatile int *)(base_address + ADDR_g_gpadPorts);
    char found[24] = "";
    int count = 0;
    for (int port = 0; port < 4; port++) {
        if (ports[port * 2] < 0)
            continue;
        size_t length = strlen(found);
        snprintf(found + length, sizeof(found) - length, "%s%i", count++ > 0 ? ", " : "", ports[port * 2] + 1);
    }
    int reads = game_user_index();
    if (count == 0)
        printf("BO3MacFix: the game found no controllers\n");
    else if (reads >= 0)
        printf("BO3MacFix: the game found %s %s and reads player %i\n", count > 1 ? "controllers on players" :
               "a controller on player", found, reads + 1);
    else
        printf("BO3MacFix: the game found %s %s but reads none\n", count > 1 ? "controllers on players" :
               "a controller on player", found);
}

static void scan_now(void *why) {
    atomic_store(&scan_pending, false);
    printf("BO3MacFix: asking the game to look for controllers (%s)\n", (const char *)why);
    GPad_DevicesChanged();
    // it looks a frame later, and again 60 frames after that
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, log_game_gamepads);
}

void controllers_request_gamepad_scan(const char *why) {
    if (!gamepads_up() || atomic_exchange(&scan_pending, true))
        return;
    // from the main thread, which can write to the command buffer (it has its own lock), and a moment later: SDL and
    // Aspyr have taken a controller of theirs by then, and controllers that come together make one request
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), (void *)why,
                     scan_now);
}

// SDL only asks for the version while building a new controller's GUID. Log what it will see, and give known
// PlayStation controllers the version 0x0100 the mappings use, in case Bluetooth or firmware reports another.
// With the GameController bridge on, give them a version no mapping has instead, so SDL leaves them to the bridge.
CFTypeRef controllers_hid_property(IOHIDDeviceRef device, CFStringRef key, CFTypeRef value) {
    if (base_address == 0 || key != (CFStringRef)(base_address + ADDR_CFString_HIDVersionNumber))
        return value;
    int vendor = hid_int_property(device, CFSTR(kIOHIDVendorIDKey));
    int product = hid_int_property(device, CFSTR(kIOHIDProductIDKey));
    bool bridged = gc_bridge_enabled() && vendor == SONY_VENDOR_ID && is_playstation_product(product);
    // after startup, this is a controller connected while the game runs: Aspyr takes it as soon as SDL has, but the
    // game has to be asked to look (the bridge asks for its own controllers)
    if (gamepads_up()) {
        printf("BO3MacFix: SDL found a controller connected while the game runs (%04x:%04x)\n", vendor, product);
        if (!bridged)
            controllers_request_gamepad_scan("SDL found a new controller");
    }
    if ((!mappings_enabled && !gc_bridge_enabled()) || vendor != SONY_VENDOR_ID)
        return value;
    int version = 0;
    if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(value, kCFNumberSInt32Type, &version);
    char transport[32] = "unknown";
    CFTypeRef transport_ref = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDTransportKey));
    if (transport_ref != NULL && CFGetTypeID(transport_ref) == CFStringGetTypeID())
        CFStringGetCString(transport_ref, transport, sizeof(transport), kCFStringEncodingUTF8);
    printf("BO3MacFix: Sony controller %04x:%04x version %04x over %s\n", vendor, product, version, transport);

    if (bridged) {
        static CFNumberRef version_unmapped = NULL;
        if (version_unmapped == NULL) {
            int v = 0xFFFF;
            version_unmapped = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
        }
        printf("BO3MacFix: keeping it away from SDL, the GameController bridge reads it\n");
        return version_unmapped;
    }
    if (!is_playstation_product(product) || version == 0x0100)
        return value;
    static CFNumberRef version_0100 = NULL;
    if (version_0100 == NULL) {
        int v = 0x0100;
        version_0100 = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    }
    return version_0100;
}

// is the controller behind this gamepad port made by Sony? an SDL one, or one from the GameController bridge
static bool is_playstation_port(int port) {
    int user_index = *(int *)(base_address + ADDR_g_gpadPorts + port * 8);
    if (user_index < 0 || user_index > 3)
        return false;
    const uint8_t *controller = *(const uint8_t **)(base_address + ADDR_ASL_XInputSlots + user_index * 24);
    if (controller == NULL)
        return gc_bridge_is_playstation_user(user_index);
    // SDL_GameController.guid is at +0xC, and the GUID's vendor at +4 in that
    uint16_t vendor;
    memcpy(&vendor, controller + 0x10, sizeof(vendor));
    return vendor == SONY_VENDOR_ID;
}

// Aspyr's version returns DURANGO for any gamepad on a port. The Lua UI (Engine.GamepadType, which uses the
// ps3button_* images for ORBIS) and the ^B button glyphs in text both ask this function.
install_hook_name(GPad_GetGamepadType, int, int controllerIndex) {
    int port = *(int *)(base_address + ADDR_g_gpads + controllerIndex * 0x70 + 4);
    if ((unsigned int)port > 3)
        return GAMEPAD_TYPE_COUNT;
    bool playstation = button_prompts_mode == 2 || (button_prompts_mode == 0 && is_playstation_port(port));
    int type = playstation ? GAMEPAD_TYPE_ORBIS : GAMEPAD_TYPE_DURANGO;
    static int last_type = -1;
    if (type != last_type) {
        printf("BO3MacFix: using %s button prompts\n", playstation ? "PlayStation" : "Xbox");
        last_type = type;
    }
    return type;
}

static bool prompts_hooked = false;

static bool install_prompts_hook(void) {
    // push rbp; mov rbp, rsp; movsxd rax, edi; imul rax, rax, 0x70
    static const uint8_t expected[] = { 0x55, 0x48, 0x89, 0xE5, 0x48, 0x63, 0xC7, 0x48, 0x6B, 0xC0, 0x70 };
    void *func = (void *)(base_address + ADDR_GPad_GetGamepadType);
    if (memcmp(prepatch_original_code(func, sizeof(expected)), expected, sizeof(expected)) != 0) {
        printf("unexpected code at %08x, not hooking\n", ADDR_GPad_GetGamepadType);
        return false;
    }
    install_hook_GPad_GetGamepadType(func);
    prompts_hooked = true;
    return true;
}

void controllers_init(uint64_t base, int add_mappings, int button_prompts) {
    base_address = base;
    // push rbp; mov rbp, rsp; lea rsi, [rip+0x4f553a] ("wait; gpadupdate;..."); xor edi, edi; pop rbp; jmp Cbuf_InsertText
    static const uint8_t devices_changed[] = { 0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x35, 0x3A, 0x55, 0x4F, 0x00, 0x31, 0xFF, 0x5D, 0xE9 };
    void *func = (void *)(base + ADDR_GPad_DevicesChanged);
    if (memcmp(prepatch_original_code(func, sizeof(devices_changed)), devices_changed, sizeof(devices_changed)) == 0)
        GPad_DevicesChanged = func;
    else
        printf("unexpected code at %08x, controllers connected while the game runs won't be picked up\n",
               ADDR_GPad_DevicesChanged);
    if (add_mappings) {
        mappings_enabled = true;
        add_sdl_mappings();
    }
    button_prompts_mode = button_prompts;
    if (button_prompts != 1)
        install_prompts_hook();
}

// ButtonPrompts changed while the game runs. Started with Xbox prompts (1), the hook isn't in place, and adding it
// now is only a data write from the pre-patched executable; otherwise that needs a restart.
bool controllers_prompts_live(void) {
    return prompts_hooked || prepatch_active();
}

bool controllers_set_button_prompts(int mode) {
    if (mode < 0 || mode > 2)
        return false;
    if (!prompts_hooked && mode != 1 && (!prepatch_active() || !install_prompts_hook()))
        return false;
    button_prompts_mode = mode;
    printf("BO3MacFix: button prompts now %s\n", mode == 0 ? "automatic" : mode == 1 ? "Xbox" : "PlayStation");
    return true;
}
