// Tests controllers.c against a fake game image and fake HID devices: when the game is asked to look for controllers
// (not before its gamepad code is up, requests close together merged, which SDL devices ask), the version SDL is
// given for PlayStation controllers, and the log line of what the game found. Nothing here touches a real device.
#include <sys/mman.h>

#define IOHIDDeviceGetProperty fake_IOHIDDeviceGetProperty
#include "../source/controllers.c"
#undef IOHIDDeviceGetProperty

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

// a fake HID device is its vendor and product
typedef struct {
    int vendor, product;
} fake_device;

CFTypeRef fake_IOHIDDeviceGetProperty(IOHIDDeviceRef device, CFStringRef key) {
    const fake_device *d = (const fake_device *)device;
    static CFNumberRef numbers[2];
    int which = CFEqual(key, CFSTR(kIOHIDVendorIDKey)) ? 0 : CFEqual(key, CFSTR(kIOHIDProductIDKey)) ? 1 : -1;
    if (which < 0)
        return CFEqual(key, CFSTR(kIOHIDTransportKey)) ? CFSTR("Bluetooth") : NULL;
    int v = which == 0 ? d->vendor : d->product;
    if (numbers[which] != NULL)
        CFRelease(numbers[which]);
    numbers[which] = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    return numbers[which];
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
    fake_device xbox = { 0x045E, 0x0B13 }, dualsense = { 0x054C, 0x0CE6 }, other_sony = { 0x054C, 0x1234 };

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

    // bridge off: a DualSense goes to SDL as version 0100, which the mappings have, and asks
    bridge_on = false;
    CHECK(version_of(controllers_hid_property((IOHIDDeviceRef)&dualsense, version_key, version_0200_in)) == 0x0100);
    run_main_queue(0.4);
    CHECK(asked == 4);

    // what the game found, from its port table and its first gamepad's port
    int *ports = (int *)(base + ADDR_g_gpadPorts), *gamepad_port = (int *)(base + ADDR_g_gpads + 4);
    for (int i = 0; i < 8; i++)
        ports[i] = -1;
    *gamepad_port = 4;
    log_game_gamepads(NULL);
    ports[0] = 0;
    ports[1] = 0;
    *gamepad_port = 0;
    log_game_gamepads(NULL);
    ports[2] = 1;
    *gamepad_port = 4;
    log_game_gamepads(NULL);

    if (failures == 0)
        printf("controllers tests passed\n");
    else
        printf("controllers tests: %d FAILED\n", failures);
    return failures != 0;
}
