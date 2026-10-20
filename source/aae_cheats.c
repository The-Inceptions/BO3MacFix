// AAE's bank and weapon locker at a map start, see aae_cheats.h
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "aae_cheats.h"
#include "dobby.h"
#include "offsets.h"
#include "zm_loadout.h"

bool aae_cheats_keep_off(const char *command, const char *mod, int elmg_cheats) {
    // the map command compares argv[0] with "devmap" itself; commands match whatever their case
    return command != NULL && mod != NULL && strcasecmp(command, "devmap") != 0 && strcmp(mod, Aae_Mod_Name) == 0 &&
           elmg_cheats == 0;
}

static uint64_t base = 0;
static void *(*Dvar_FindByHash)(uint32_t hash);
static int (*Dvar_GetInt)(void *dvar); // any type: a string dvar (one "set" made) reads as atoi of its text

// copies size bytes at address into out; false, without a crash, where nothing is mapped
static bool safe_read(uint64_t address, void *out, size_t size) {
    mach_vm_size_t got = 0;
    return address != 0 &&
           mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)out, &got) == KERN_SUCCESS &&
           got == size;
}

// a NUL-terminated string at address into out (at most size - 1 characters), false if it can't be read
static bool read_string(uint64_t address, char *out, size_t size) {
    if (!safe_read(address, out, size - 1))
        return false;
    out[size - 1] = '\0';
    return true;
}

// the console command being run's name, its argv[0]
static bool command_name(char *out, size_t size) {
    int32_t nesting = 0, argc = 0;
    uint64_t argv = 0, arg0 = 0;
    const uint64_t args = base + ADDR_cmd_args;
    return safe_read(args, &nesting, sizeof(nesting)) && nesting >= 0 && nesting < 8 &&
           safe_read(args + 0x64 + 4 * (uint64_t)nesting, &argc, sizeof(argc)) && argc > 0 &&
           safe_read(args + 0x88 + 8 * (uint64_t)nesting, &argv, sizeof(argv)) && safe_read(argv, &arg0, sizeof(arg0)) &&
           read_string(arg0, out, size);
}

static bool keep_cheats_off(void) {
    char command[16], mod[0x20 + 1];
    bool have_command = command_name(command, sizeof(command));
    bool have_mod = read_string(base + ADDR_Mods_LoadedName, mod, sizeof(mod));
    void *elmg_cheats = Dvar_FindByHash(loadout_script_hash("elmg_cheats"));
    return aae_cheats_keep_off(have_command ? command : NULL, have_mod ? mod : NULL,
                               elmg_cheats != NULL ? Dvar_GetInt(elmg_cheats) : 0);
}

// Dvar_SetBool(dvar, value): only the map command's Dvar_SetBool(sv_cheats, 1) for a loaded mod changes, to 0
install_hook_name(Dvar_SetBool, void, void *dvar, bool value) {
    if (value && __builtin_return_address(0) == (void *)(base + ADDR_Map_f_SetCheatsReturn) && keep_cheats_off()) {
        printf("BO3MacFix: map started with All-around Enhancement: sv_cheats stays off, so its bank and weapon locker "
               "load (AAEBankFix)\n");
        value = false;
    }
    orig_Dvar_SetBool(dvar, value);
}

void aae_cheats_install(uint64_t base_address, bool enabled) {
    if (!enabled || base_address == 0)
        return;
    base = base_address;
    Dvar_FindByHash = (void *)(base + ADDR_Dvar_FindByHash);
    Dvar_GetInt = (void *)(base + ADDR_Dvar_GetInt);
    install_hook_Dvar_SetBool((void *)(base + ADDR_Dvar_SetBool));
}
