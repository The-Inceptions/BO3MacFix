// Tests controllers.c against a fake game image and fake HID devices: when the game is asked to look for controllers
// (not before its gamepad code is up, requests close together merged, which SDL devices ask), the version SDL is
// given for PlayStation controllers, Steam Input's virtual controllers (which are told apart from a real Xbox 360
// controller, and hidden only for a PlayStation controller the bridge reads), and the log line of what the game
// found. Nothing here touches a real device or Steam's own file.
#include <sys/mman.h>

#define IOHIDDeviceGetProperty fake_IOHIDDeviceGetProperty
#define IOHIDManagerCopyDevices fake_IOHIDManagerCopyDevices
#include "../source/controllers.c"
#undef IOHIDDeviceGetProperty
#undef IOHIDManagerCopyDevices

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            failures++;                                                 \
        }                                                               \
    } while (0)

// what controllers.c links against
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    return 0;
}
const void *prepatch_original_code(const void *address, size_t size) {
    return address;
}
bool prepatch_active(void) {
    return false;
}
static bool bridge_on = true;
bool gc_bridge_enabled(void) {
    return bridge_on;
}
bool gc_bridge_is_playstation_user(int user_index) {
    return false;
}
static int bridge_pads = 0;
int gc_bridge_pad_count(void) {
    return bridge_pads;
}

// a fake HID device is its vendor, product, name and primary usage
typedef struct {
    int vendor, product;
    const char *name;
    int usage_page, usage;
} fake_device;

CFTypeRef fake_IOHIDDeviceGetProperty(IOHIDDeviceRef device, CFStringRef key) {
    const fake_device *d = (const fake_device *)device;
    static CFTypeRef values[5];
    int which = CFEqual(key, CFSTR(kIOHIDVendorIDKey)) ? 0 : CFEqual(key, CFSTR(kIOHIDProductIDKey)) ? 1 :
                CFEqual(key, CFSTR(kIOHIDPrimaryUsagePageKey)) ? 2 : CFEqual(key, CFSTR(kIOHIDPrimaryUsageKey)) ? 3 :
                CFEqual(key, CFSTR(kIOHIDProductKey)) ? 4 : -1;
    if (which < 0)
        return CFEqual(key, CFSTR(kIOHIDTransportKey)) ? CFSTR("Bluetooth") : NULL;
    if (values[which] != NULL)
        CFRelease(values[which]);
    values[which] = NULL;
    if (which == 4) {
        if (d->name != NULL)
            values[which] = CFStringCreateWithCString(NULL, d->name, kCFStringEncodingUTF8);
    } else {
        int v = which == 0 ? d->vendor : which == 1 ? d->product : which == 2 ? d->usage_page : d->usage;
        values[which] = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    }
    return values[which];
}

// the HID devices connected, as IOKit lists them
static const fake_device *present[8];
static int present_count = 0;
CFSetRef fake_IOHIDManagerCopyDevices(IOHIDManagerRef manager) {
    return CFSetCreate(NULL, (const void **)present, present_count, NULL);
}

static void set_present(const fake_device *a, const fake_device *b, const fake_device *c) {
    present_count = 0;
    const fake_device *list[] = { a, b, c };
    for (int i = 0; i < 3; i++)
        if (list[i] != NULL)
            present[present_count++] = list[i];
}

// what Steam's virtualgamepadinfo.txt says, in a file of our own (SteamVirtualGamepadInfo names it)
static char info_path[256];
static void set_steam_info(const char *text) {
    FILE *fp = fopen(info_path, "w");
    fputs(text, fp);
    fclose(fp);
}

static int asked = 0;
static void devices_changed(void) {
    asked++;
}

static void run_main_queue(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

static int version_of(CFTypeRef value) {
    int version = -1;
    if (value != NULL)
        CFNumberGetValue(value, kCFNumberSInt32Type, &version);
    return version;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint64_t base = (uint64_t)mmap(NULL, ADDR_dvar_gpadEnabled + 0x1000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(base != (uint64_t)MAP_FAILED);

    // other code at the game's handler: it's not called, so nothing is ever asked
    controllers_init(base, 1, 1);
    CHECK(GPad_DevicesChanged == NULL);
    static const uint8_t code[] = { 0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x35, 0x3A, 0x55, 0x4F, 0x00, 0x31, 0xFF, 0x5D, 0xE9 };
    memcpy((void *)(base + ADDR_GPad_DevicesChanged), code, sizeof(code));
    controllers_init(base, 1, 1);
    CHECK(GPad_DevicesChanged == (void *)(base + ADDR_GPad_DevicesChanged));
    GPad_DevicesChanged = devices_changed; // the real one is x86 code

    CFStringRef version_key = (CFStringRef)(base + ADDR_CFString_HIDVersionNumber);
    int v0100 = 0x0100, v0200 = 0x0200;
    CFNumberRef version_0100_in = CFNumberCreate(NULL, kCFNumberSInt32Type, &v0100);
    CFNumberRef version_0200_in = CFNumberCreate(NULL, kCFNumberSInt32Type, &v0200);
    fake_device xbox = { 0x045E, 0x0B13, NULL, 0, 0 }, dualsense = { 0x054C, 0x0CE6, NULL, 0, 0 },
                other_sony = { 0x054C, 0x1234, NULL, 0, 0 };

    // SDL's devices at startup, before GPad_Init: nothing is asked
    CHECK(controllers_hid_property((IOHIDDeviceRef)&xbox, version_key, version_0100_in) == version_0100_in);
    controllers_request_gamepad_scan("too early");
    run_main_queue(0.4);
    CHECK(asked == 0);

    // GPad_Init has registered its dvars: three requests close together are asked once
    *(void **)(base + ADDR_dvar_gpadEnabled) = (void *)0x1234;
    controllers_request_gamepad_scan("first");
    controllers_request_gamepad_scan("second");
    controllers_request_gamepad_scan("third");
    run_main_queue(0.4);
    CHECK(asked == 1);

    // other keys pass through and ask nothing
    CHECK(controllers_hid_property((IOHIDDeviceRef)&xbox, CFSTR(kIOHIDProductKey), version_0100_in) == version_0100_in);
    run_main_queue(0.4);
    CHECK(asked == 1);

    // bridge on: an Xbox controller SDL takes mid-game asks; a DualSense is kept from SDL (version FFFF) and the bridge
    // asks for it instead; a Sony device that isn't a PlayStation controller is SDL's and asks
    CHECK(controllers_hid_property((IOHIDDeviceRef)&xbox, version_key, version_0100_in) == version_0100_in);
    run_main_queue(0.4);
    CHECK(asked == 2);
    CHECK(version_of(controllers_hid_property((IOHIDDeviceRef)&dualsense, version_key, version_0100_in)) == 0xFFFF);
    run_main_queue(0.4);
    CHECK(asked == 2);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&other_sony, version_key, version_0200_in) == version_0200_in);
    run_main_queue(0.4);
    CHECK(asked == 3);

    // Steam Input's virtual controllers are told by their ids and name: "GamePad-N" (N = slot + 1, at most 16) on
    // 045e:028e, Valve's 28de:11ff with any name. A real Xbox 360 controller has the same ids but not the name
    CHECK(steam_pad_slot(0x045E, 0x028E, "GamePad-1") == 0);
    CHECK(steam_pad_slot(0x045E, 0x028E, "GamePad-16") == 15);
    CHECK(steam_pad_slot(0x045E, 0x028E, "GamePad-17") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x028E, "GamePad-0") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x028E, "GamePad-01") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x028E, "GamePad-1x") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x028E, "GamePad-") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x028E, "Steam Virtual Gamepad") == -1);
    CHECK(steam_pad_slot(0x045E, 0x028E, "Controller") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x028E, "Xbox 360 Wired Controller") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x028E, "") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x28DE, 0x11FF, "") == -1);
    CHECK(steam_pad_slot(0x28DE, 0x11FF, "GamePad-3") == 2);
    CHECK(steam_pad_slot(0x28DE, 0x1102, "Steam Controller") == NOT_STEAM_PAD); // a real Steam Controller
    CHECK(steam_pad_slot(0x054C, 0x0CE6, "GamePad-1") == NOT_STEAM_PAD);
    CHECK(steam_pad_slot(0x045E, 0x0B13, "GamePad-1") == NOT_STEAM_PAD);

    // Steam's file: the real controller behind a slot
    const char *info = "[slot 0]\nname=DualSense Wireless Controller\nVID=0x054c\nPID=0x0ce6\ntype=ps5\n"
                       "handle=0x0000000000000001\n[slot 1]\r\nname=Xbox Wireless Controller\r\nVID=0x045e\r\n"
                       "PID=0x0b13\r\ntype=xboxone\r\n";
    int real_vendor = 0, real_product = 0;
    CHECK(steam_info_lookup(info, 0, &real_vendor, &real_product) && real_vendor == 0x054C && real_product == 0x0CE6);
    CHECK(steam_info_lookup(info, 1, &real_vendor, &real_product) && real_vendor == 0x045E && real_product == 0x0B13);
    CHECK(!steam_info_lookup(info, 2, &real_vendor, &real_product));
    CHECK(!steam_info_lookup("", 0, &real_vendor, &real_product));
    CHECK(!steam_info_lookup("[slot 0]\nname=no ids\n[slot 1]\nVID=0x054c\nPID=0x0ce6\n", 0, &real_vendor, &real_product));

    // the decision: only with a PlayStation controller the bridge reads, and only for its stand-in
    bool hide;
    steam_pad_facts facts = { .bridged = 0 };
    steam_pad_verdict(&facts, &hide);
    CHECK(!hide);
    facts = (steam_pad_facts){ .bridged = 1 };
    steam_pad_verdict(&facts, &hide);
    CHECK(hide);
    facts = (steam_pad_facts){ .bridged = 1, .others = 1 };
    steam_pad_verdict(&facts, &hide);
    CHECK(!hide);
    facts = (steam_pad_facts){ .listed = true, .listed_bridged = true, .bridged = 1, .others = 1 };
    steam_pad_verdict(&facts, &hide);
    CHECK(hide);
    facts = (steam_pad_facts){ .listed = true, .listed_bridged = false, .bridged = 1 };
    steam_pad_verdict(&facts, &hide);
    CHECK(!hide);
    facts = (steam_pad_facts){ .listed = true, .listed_bridged = true, .bridged = 0 };
    steam_pad_verdict(&facts, &hide);
    CHECK(!hide);

    // the same through what SDL is given, with fake devices. Steam's file is ours, empty to start with
    snprintf(info_path, sizeof(info_path), "/tmp/bo3macfix-test-steaminfo-%d.txt", (int)getpid());
    set_steam_info("");
    setenv("SteamVirtualGamepadInfo", info_path, 1);
    int v0000 = 0, v0114 = 0x0114;
    CFNumberRef version_0000_in = CFNumberCreate(NULL, kCFNumberSInt32Type, &v0000);
    CFNumberRef version_0114_in = CFNumberCreate(NULL, kCFNumberSInt32Type, &v0114);
    fake_device steam_pad = { 0x045E, 0x028E, "GamePad-1", 1, 5 }, steam_pad2 = { 0x045E, 0x028E, "GamePad-2", 1, 5 };
    fake_device xbox360 = { 0x045E, 0x028E, "Controller", 1, 5 }, xbox_bt = { 0x045E, 0x0B13, "Xbox Wireless Controller", 1, 5 };
    fake_device dualsense_hid = { 0x054C, 0x0CE6, "DualSense Wireless Controller", 1, 5 };
    fake_device keyboard = { 0x05AC, 0x0342, "Keyboard", 1, 6 };
    int before = asked;

    // a DualSense IOKit has (the bridge hasn't seen it yet) and its stand-in: hidden, nothing asked
    set_present(&dualsense_hid, &steam_pad, &keyboard);
    CHECK(version_of(controllers_hid_property((IOHIDDeviceRef)&steam_pad, version_key, version_0000_in)) == 0xFFFF);
    run_main_queue(0.4);
    CHECK(asked == before);

    // the bridge has a DualSense IOKit doesn't list: hidden too
    set_present(&steam_pad, NULL, NULL);
    bridge_pads = 1;
    CHECK(version_of(controllers_hid_property((IOHIDDeviceRef)&steam_pad, version_key, version_0000_in)) == 0xFFFF);
    bridge_pads = 0;

    // no PlayStation controller: Steam's is the only way to play, it's SDL's and asks
    set_present(&steam_pad, &keyboard, NULL);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&steam_pad, version_key, version_0000_in) == version_0000_in);
    run_main_queue(0.4);
    CHECK(asked == before + 1);

    // a real Xbox 360 controller next to a DualSense is never hidden
    set_present(&dualsense_hid, &xbox360, NULL);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&xbox360, version_key, version_0114_in) == version_0114_in);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&xbox360, version_key, version_0000_in) == version_0000_in);
    run_main_queue(0.4);
    CHECK(asked == before + 2);

    // a DualSense and an Xbox controller, and Steam doesn't say which is which: both stand-ins stay with SDL
    set_present(&dualsense_hid, &xbox_bt, &steam_pad);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&steam_pad, version_key, version_0000_in) == version_0000_in);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&steam_pad2, version_key, version_0000_in) == version_0000_in);

    // Steam's file says slot 0 is the DualSense and slot 1 the Xbox controller: only the DualSense's is hidden
    set_steam_info(info);
    CHECK(version_of(controllers_hid_property((IOHIDDeviceRef)&steam_pad, version_key, version_0000_in)) == 0xFFFF);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&steam_pad2, version_key, version_0000_in) == version_0000_in);

    // the file lists the DualSense, but it's gone: the stand-in (about to go too) is left alone
    set_present(&xbox_bt, &steam_pad, NULL);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&steam_pad, version_key, version_0000_in) == version_0000_in);
    run_main_queue(0.4);
    before = asked;

    // bridge off: nothing is hidden
    bridge_on = false;
    set_present(&dualsense_hid, &steam_pad, NULL);
    CHECK(controllers_hid_property((IOHIDDeviceRef)&steam_pad, version_key, version_0000_in) == version_0000_in);
    run_main_queue(0.4);
    CHECK(asked == before + 1);
    bridge_on = true;
    unlink(info_path);
    unsetenv("SteamVirtualGamepadInfo");

    // bridge off: a DualSense goes to SDL as version 0100, which the mappings have, and asks
    bridge_on = false;
    CHECK(version_of(controllers_hid_property((IOHIDDeviceRef)&dualsense, version_key, version_0200_in)) == 0x0100);
    run_main_queue(0.4);
    CHECK(asked == before + 2);

    // what the game found, from its port table and its first gamepad's port
    int *ports = (int *)(base + ADDR_g_gpadPorts), *gamepad_port = (int *)(base + ADDR_g_gpads + 4);
    for (int i = 0; i < 8; i++)
        ports[i] = -1;
    *gamepad_port = 4;
    log_game_gamepads(NULL);
    CHECK(controllers_game_user_index() == -1);
    ports[0] = 0;
    ports[1] = 0;
    *gamepad_port = 0;
    log_game_gamepads(NULL);
    CHECK(controllers_game_user_index() == 0);
    ports[2] = 1;
    *gamepad_port = 1;
    CHECK(controllers_game_user_index() == 1);
    *gamepad_port = 4;
    log_game_gamepads(NULL);
    CHECK(controllers_game_user_index() == -1);

    if (failures == 0)
        printf("controllers tests passed\n");
    else
        printf("controllers tests: %d FAILED\n", failures);
    return failures != 0;
}
