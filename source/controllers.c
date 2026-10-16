// PlayStation controller support.
//
// BO3 statically links SDL 2.0.7, which reads controllers through IOKit and only turns ones with a mapping into
// game controllers. Its built-in list has the DualShock 4 (at version 0x0100 only) but no DualSense, and
// Aspyr's GPad_GetGamepadType always says Xbox, so even a working DualShock 4 shows Xbox button prompts.
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

// SDL reads SDL_GAMECONTROLLERCONFIG (one mapping per line) right after its built-in list, and a later line
// replaces an earlier one for the same GUID. Ours go first so Steam's and the user's own mappings win.
static void add_sdl_mappings(void) {
    const char *existing = getenv("SDL_GAMECONTROLLERCONFIG");
    char *user_mappings = read_user_mappings();
    size_t size = 1;
    for (size_t i = 0; i < PlayStation_Mapping_Count; i++)
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
    for (size_t i = 0; i < PlayStation_Mapping_Count; i++) {
        if (i > 0)
            strcat(config, "\n");
        strcat(config, playstation_mappings[i]);
    }
    if (existing != NULL && existing[0] != '\0') {
        strcat(config, "\n");
        strcat(config, existing);
    }
    if (user_mappings != NULL && user_mappings[0] != '\0') {
        strcat(config, "\n");
        strcat(config, user_mappings);
    }
    setenv("SDL_GAMECONTROLLERCONFIG", config, 1);
    printf("BO3MacFix: added %zu PlayStation controller mappings%s\n", PlayStation_Mapping_Count,
           user_mappings != NULL ? " and BO3MacFix/gamecontrollerdb.txt" : "");
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

// SDL only asks for the version while building a new controller's GUID. Log what it will see, and give known
// PlayStation controllers the version 0x0100 the mappings use, in case Bluetooth or firmware reports another.
CFTypeRef controllers_hid_property(IOHIDDeviceRef device, CFStringRef key, CFTypeRef value) {
    if (!mappings_enabled || key != (CFStringRef)(base_address + ADDR_CFString_HIDVersionNumber))
        return value;
    int vendor = hid_int_property(device, CFSTR(kIOHIDVendorIDKey));
    if (vendor != SONY_VENDOR_ID)
        return value;
    int product = hid_int_property(device, CFSTR(kIOHIDProductIDKey));
    int version = 0;
    if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(value, kCFNumberSInt32Type, &version);
    char transport[32] = "unknown";
    CFTypeRef transport_ref = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDTransportKey));
    if (transport_ref != NULL && CFGetTypeID(transport_ref) == CFStringGetTypeID())
        CFStringGetCString(transport_ref, transport, sizeof(transport), kCFStringEncodingUTF8);
    printf("BO3MacFix: Sony controller %04x:%04x version %04x over %s\n", vendor, product, version, transport);

    if (!is_playstation_product(product) || version == 0x0100)
        return value;
    static CFNumberRef version_0100 = NULL;
    if (version_0100 == NULL) {
        int v = 0x0100;
        version_0100 = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    }
    return version_0100;
}

// is the SDL controller behind this gamepad port made by Sony?
static bool is_playstation_port(int port) {
    int user_index = *(int *)(base_address + ADDR_g_gpadPorts + port * 8);
    if (user_index < 0 || user_index > 3)
        return false;
    const uint8_t *controller = *(const uint8_t **)(base_address + ADDR_ASL_XInputSlots + user_index * 24);
    if (controller == NULL)
        return false;
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

void controllers_init(uint64_t base, int add_mappings, int button_prompts) {
    base_address = base;
    if (add_mappings) {
        mappings_enabled = true;
        add_sdl_mappings();
    }
    button_prompts_mode = button_prompts;
    if (button_prompts != 1) {
        // push rbp; mov rbp, rsp; movsxd rax, edi; imul rax, rax, 0x70
        static const uint8_t expected[] = { 0x55, 0x48, 0x89, 0xE5, 0x48, 0x63, 0xC7, 0x48, 0x6B, 0xC0, 0x70 };
        void *func = (void *)(base + ADDR_GPad_GetGamepadType);
        if (memcmp(func, expected, sizeof(expected)) == 0)
            install_hook_GPad_GetGamepadType(func);
        else
            printf("unexpected code at %08x, not hooking\n", ADDR_GPad_GetGamepadType);
    }
}
