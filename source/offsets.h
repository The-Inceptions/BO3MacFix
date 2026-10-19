// Modified by Mickael DE LA ROQUE on 2026-09-08 with offsets used by the
// unofficial custom-map stream-stall workaround.
// -- START OFFSETS [Mac v1.0.0.0, in-game v98, executable 5e42371b6a16f2b11e637f48e38554625570aa9f]
// functions
#define ADDR_HKAI_InitMapData 0xe09aa9 // func
#define ADDR_load_workshop_texture 0xe4a26c // func - name is a guess
#define ADDR_build_usermods_path 0xe49938 // func - name is a guess
#define ADDR_DB_FindXAssetHeader 0x1345b12 // func
#define ADDR_Com_Error 0xe71e51 // func
#define ADDR_Live_SystemInfo 0x129d650 // func
#define ADDR_Dvar_SetFromStringByName 0xfa3386 // func
#define ADDR_someLoggingFunction 0xe70f88 // func - name is a guess
#define ADDR_someWindowCreationFunction 0x1143ca // func - name is a guess
#define ADDR_Sys_Checksum 0x14025dd // func
#define ADDR_Sys_VerifyPacketChecksum 0x140258f // func
#define ADDR_Sys_CheckSumPacketCopy 0x1402648 // func
#define ADDR_Lua_CmdParseArgs 0x1408cf9 // func
#define ADDR_LobbyMsgRW_PrepWriteMsg 0xf415fa // func
#define ADDR_LobbyMsgRW_PrepReadMsg 0xf416fb // func
#define ADDR_MSG_ReadByte 0x1247989 // func
#define ADDR_MSG_WriteByte 0x12473d3 // func
#define ADDR_ASL_ToNativePath 0xe9f6d // func
#define ADDR_CSteamAPIContext_Init 0xd311e0 // func
#define ADDR_dwInstantMessageDispatch 0xe36915 // func
#define ADDR_hks_load_dll 0x148460d // func
#define ADDR_hksf_fopen_internal 0xfb503 // func - name is a guess, called by hksf_fopen
#define ADDR_CL_FirstSnapshot 0xe2f0be // func
#define ADDR_CL_SetCGameTime 0xe2fbe8 // func
#define ADDR_DB_GetStreamFraction 0x127c182 // func - name is a guess
#define ADDR_Com_GetBuildIntField 0xf16d9a // func
#define ADDR_Lua_GetBuildStringField 0xe66e2f // func - Engine.GetBuildStringField
#define ADDR_Lua_Exec 0x13a83fa // func - Engine.Exec
#define ADDR_Lua_GetBuildIntField 0xe66e92 // func - Engine.GetBuildIntField
#define ADDR_hksi_lua_pushlstring 0x1409c50 // func
#define ADDR_hksi_luaL_checklstring 0x1473440 // func
#define ADDR_hksi_hksL_loadbuffer 0x1474566 // func
#define ADDR_hksi_lua_pcall 0x14465f9 // func
#define ADDR_hks_pushnamedcclosure 0x1476385 // func
#define ADDR_R_TrackTextureMemory 0x126dad2 // func - name is a guess
#define ADDR_ASL_SleepEx 0x111752 // func - Aspyr's Sleep/SleepEx(milliseconds, alertable)
#define ADDR_Scr_LoadSourceText 0x149347d // func - void (script *), loads its source text from scriptsrc.zip (the mod's zone_source/, else zone/) into script+0x28, for script error call stacks - name is a guess
#define ADDR_Scr_ErrorInternal 0x11a2292 // func - void (scriptInstance_t inst), throws scrVarPub's error_message: with developer on it prints "throwing script exception" first, then longjmps
#define ADDR_Scr_RuntimeErrorInternal 0x126cc9f // func - void (inst, const char *codePos, unsigned int index, const char *msg, const char *dialogMessage), prints "******* script error *******" and the call stack while developer is on, Com_Error for a terminal error
#define ADDR_Scr_EvalEquality 0x12f7ed5 // func - void (inst, VariableValue *a, VariableValue *b), a = (a == b) as an int, for GSC's == and (negated, 0x12f8567) !=; throws "pair '%s' and '%s' has unmatching types" for types other than int and float that differ. The === and !== wrappers (0x12f8315, 0x12f8586) check the types first and give false/true - names are guesses
#define ADDR_Scr_FreeValuePair 0x12f8366 // func - void (inst, VariableValue *a, VariableValue *b), what the === and !== wrappers call on mismatched types before giving their answer - name is a guess
#define ADDR_Com_PrintScriptError 0xe714f7 // func - void (const char *fmt, ...), prints to console_mp.log's SCRIPTERROR channel - name is a guess
#define ADDR_Scr_GetCodePosInfo 0x1493740 // func - void (inst, const char *codePos, const char **file, int *line, const char **lineText), line -1 without line information - name is a guess
#define ADDR_Scr_FindScriptForCodePos 0x1493425 // func - entry * (inst, const char *codePos), the loaded script whose code holds codePos, its buffer (the compiled script's header) at +0 - name is a guess
#define ADDR_Cocoa_HandleMouseEvent 0x148470 // func - SDL 2.0.7, relative mode motion from [NSEvent deltaX/deltaY]
#define ADDR_SDL_GetMouse 0x1355b0 // func - SDL 2.0.7
#define ADDR_SDL_SendMouseMotion 0x135870 // func - SDL 2.0.7 (window, mouseID, relative, x, y)
#define ADDR_Stub_setjmp 0x1627c20 // __stubs entry for setjmp, jmp [lazy symbol pointer]
#define ADDR_Stub_longjmp 0x16279b0 // __stubs entry for longjmp, jmp [lazy symbol pointer]
#define ADDR_SDL_SetWindowTitle 0x1944d0 // func
#define ADDR_LiveSteam_RunEveryFrame 0x1434f3e // func
#define ADDR_SomeControllerTypeFunc 0x122f78d // func
#define ADDR_SDL_PrivateGameControllerAddMapping 0x13cff0 // func
#define ADDR_SDL_GameControllerInitMappings 0x13d450 // func
#define ADDR_Cbuf_AddText 0xf754aa // func
#define ADDR_GScr_IsProfileBuild 0x1205df7 // func - void (scriptInstance_t inst), the GSC/CSC builtin isprofilebuild(): Scr_AddInt(inst, 0)
#define ADDR_Scr_GetInt 0x11a1e78 // func - int (scriptInstance_t inst, unsigned int index), takes int and hash params
#define ADDR_Scr_GetString 0x11a2281 // func - const char * (scriptInstance_t inst, unsigned int index)
#define ADDR_Scr_AddInt 0x11a30e6 // func - void (scriptInstance_t inst, int value)
#define ADDR_Scr_AddVector 0x11a344b // func - void (scriptInstance_t inst, const float *value)

//globals
#define ADDR_g_workshopMapId 0x3087dac // char[] - name is a guess
#define ADDR_g_emuPath 0x4ae9c00 // char[] - name is a guess
#define ADDR_sSessionModeState 0x6939a14 // int
#define ADDR_DB_XAssetPool 0x1017cc40 // XAssetPool[]
#define ADDR_g_assetNames 0x20d84b0 // const char *[]
#define ADDR_g_d3d11Device 0x113c0b18 // ID3D11Device * - name is a guess
#define ADDR_DB_LOAD_TOTALS 0xb2b81c0 // int[4]
#define ADDR_DB_LOAD_STAGE_COUNT 0xb2b81d0 // int
#define ADDR_DB_LOAD_COMPLETED 0xb2b81e0 // int[4]
#define ADDR_CLIENT_CONNECTIONS 0x115324a0 // client connection array
#define ADDR_LOCAL_CLIENT_COUNT 0x11534590 // int
#define ADDR_LOCAL_CLIENTS 0x11534598 // pointer
#define ADDR_CLIENT_GLOBALS 0x115345a0 // pointer
#define ADDR_scrVarPub 0x1599c000 // 0x78 bytes per script instance: bool developer at +8 (on in the Mac build), bool developer_script at +9, const char *error_message at +0x10 - name is a guess
#define ADDR_scrJmpBufs 0x14444360 // jmp_buf (0x94 bytes) per call depth, 0x2594 bytes per script instance, where script errors longjmp to - name is a guess
#define ADDR_scrJmpBufDepth 0x14448e88 // int[2] - the current one of those per script instance - name is a guess
#define ADDR_scrVmPub 0x14450ef0 // scrVmPub_t[2] (server, client), 0xa048 bytes each: function_count at +0x10, VariableValue *top at +0x20, debugCode at +0x28, abort_on_error at +0x2a, terminal_error at +0x2b, unsigned int outparamcount at +0x38
#define ADDR_cgArray 0x145b9fd0 // cg_t * - the local clients' cg_t, 0x342a80 bytes each: refdef view origin at +0x131dc8, view angles at +0x2d8930 (GetCamPosByLocalClientNum / GetCamAnglesByLocalClientNum)
#define ADDR_Prepatch_Header 0x1964080 // pre-patched executable: header and stubs, in __TEXT's padding after __eh_frame
#define ADDR_Prepatch_CodeEnd 0x1965000 // end of that padding
#define ADDR_Prepatch_Slots 0x1a268710 // pre-patched executable: slots, in __DATA's zero-filled tail after __common
#define ADDR_Prepatch_SlotsEnd 0x1a269000 // end of __DATA

// controllers
#define ADDR_GPad_GetGamepadType 0x122f78d // func - returns gamepadType_e, Aspyr's always says DURANGO for a connected pad
#define ADDR_g_gpads 0x5dedb70 // gamepad[2], 0x70 bytes each, mapped port (0-3) at +4 - name is a guess
#define ADDR_g_gpadPorts 0x5dedc50 // { int xinputUserIndex; int controllerIndex; }[4] - name is a guess
#define ADDR_ASL_XInputSlots 0x2108d50 // { SDL_GameController *; SDL_Haptic *; int effect; }[4] - name is a guess
#define ADDR_ASL_XInputGetState 0x1294a8 // func - DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState)
#define ADDR_ASL_XInputSetState 0x1295ac // func - DWORD XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration)
#define ADDR_ASL_XInputEnabled 0x1b97300 // bool - XInputEnable state, starts true and nothing changes it - name is a guess
#define ADDR_CFString_HIDVersionNumber 0x1b8b5e0 // CFStringRef - the "VersionNumber" key SDL 2.0.7 reads
#define ADDR_GPad_DevicesChanged 0x122f77a // func - void (void), the main window's WM_DEVICECHANGE (DBT_DEVNODES_CHANGED) handler: Cbuf_InsertText(0, "wait; gpadupdate;wait 60; gpadupdate;\n") - name is a guess
#define ADDR_dvar_gpadEnabled 0x14ce0a28 // dvar_t * - bool, gpadupdate only looks for controllers while it's set, NULL until GPad_Init registers it - name is a guess

//function patches
#define ADDR_Scr_ErrorInternalPatch 0x11a238e // mid-func
#define ADDR_Curl_ContentLengthSetStart 0x159345c // mid-func
#define ADDR_Curl_ContentLengthSetEnd 0x159346f // mid-func
#define ADDR_Lua_CoD_LuaCall_GetProtocolVersion_Inst 0xE1FCA8 // mid-func
#define ADDR_LPC_GetRemoteManifest_Category_Inst 0xEA8C04 // mid-func
#define ADDR_Live_SystemInfo_LarpInstr 0x129dbcc // mid-func
#define ADDR_LobbyHostMsg_SendJoinRequest_GetChangelistCall 0x1232458 // mid-func
#define ADDR_HKAI_InitMapData_DemonWithinCheck 0xe09f68 // mid-func
#define ADDR_ASL_BuildModeTable_WidthCap 0x6f275 // mid-func - jg over modes wider than the desktop
#define ADDR_ASL_BuildModeTable_HeightCap 0x6f27a // mid-func - jg over modes taller than the desktop
#define ADDR_ASL_DXGIOutput_GetModes_WidthCap 0xdee97 // mid-func - jg over modes wider than the desktop
#define ADDR_ASL_DXGIOutput_GetModes_HeightCap 0xdeea0 // mid-func - jg over modes taller than the desktop
#define ADDR_JobThreadsDvar_DefaultInstr 0x1330b91 // mid-func - mov edx, 2 (default job worker thread count, range 2-4)
#define ADDR_ASL_FramesInFlightSemaphoreInstr 0x2accc // mid-func - mov edi, 2 before dispatch_semaphore_create (Metal frames in flight)
#define ADDR_ASL_Present_PresentDrawableCall 0xe91e1 // mid-func - call [objc_msgSend] for [commandBuffer presentDrawable:]
#define ADDR_ASL_Present_CopyToDrawableCall 0xe8fde // mid-func - call [objc_msgSend] for [blitEncoder copyFromTexture:backbuffer ... toTexture:drawable.texture ...]
// where the network version (18530) and the "tu30" string are hardcoded, each within 16 bytes after the address
#define ADDRS_NetworkVersion \
    0x00E078E4, 0x00E181D3, 0x00E83E4E, 0x00EB8389, 0x00EB91A9, 0x00F988A0, 0x0104E0CB, 0x0105973D, \
    0x01059844, 0x0105C368, 0x0105C5DC, 0x011F7C3E, 0x011F7C43, 0x011F7C49, 0x0120B547, 0x0120B56A, \
    0x0123244C, 0x0126F78C, 0x0126F897, 0x0126F8BC, 0x0126F90D, 0x0126F96F, 0x012D35DA, 0x013B5C1D
#define ADDRS_TUVersion 0x016AA513, 0x017251FB, 0x01725209, 0x0172529C, 0x017252AB, 0x017252B7
#define ADDR_JQ_IdleWait_TimeoutInstr 0x11a7551 // mid-func - mov esi, 1 (idle job workers' WaitForSingleObject timeout in ms)
#define ADDR_RenderThread_WaitTimeoutInstr 0xfeb491 // mid-func - mov edi, 1 (render thread's wait for work, in ms)
// following display changes: the engine's video code (engine_display.c)
#define ADDR_R_CheckVidRestart 0xdce17e // func - per frame on the engine's Main thread, applies pending video dvar changes
#define ADDR_R_SetWndParms 0xdcca9a // func - fills the window parameters from r_fullscreen, r_monitor, r_mode, r_refreshRate
#define ADDR_R_EnumDisplayModes 0xdcc7c6 // func - bool (IDXGIAdapter *), fills the monitor table, only R_InitHardware calls it
#define ADDR_R_EnterFullscreen 0xdccd55 // func - SetFullscreenState(TRUE, output r_monitor - 1) on the window thread (message 0x8044)
#define ADDR_R_CheckVideoDvarsModified 0xdce0ad // func - vid_restart: marks a restart pending if a video dvar has a latched value
#define ADDR_Dvar_FindByHash 0xf9fffa // func - dvar_t *(uint32_t hash)
#define ADDR_Dvar_GetInt 0xfa0252 // func
#define ADDR_Dvar_GetFloat 0xfa036d // func
#define ADDR_Dvar_GetString 0xfa0474 // func
#define ADDR_Dvar_SetFromString 0xfa2ef7 // func - (dvar, value, source), sources 1 and 2 only set a latched dvar's latched value
#define ADDR_Dvar_SetInt 0xfa2d59 // func - (dvar, value)
#define ADDR_CritSec_ScopedLock 0xd14710 // func - (lock *, critical section, 0), r_mode's critical section is 0x33 - name is a guess
#define ADDR_CritSec_ScopedUnlock 0xd7da30 // func - (lock *) - name is a guess
#define ADDR_ASL_Window_UpdateLetterbox 0xee14c // func - void (Window *), Window_SetMode calls it last, with ASL's window mutex held
#define ADDR_ASL_Window_UpdateClientRect 0xee352 // func - void (Window *), takes ASL's window mutex itself
#define ADDR_ASL_Window_MetalClientRect 0xee6c6 // func - void (Window *), UpdateClientRect's Metal half (through the backend, mutex held): forces the drawable, centres the blit rect
#define ADDR_ASL_Metal_GetView 0xf2ccb // func - ASL_MetalView *(SDL_Window *), its SDL window data "M"
#define ADDR_ASL_Metal_GetDrawableSize 0xf2ce5 // func - void (view, int *w, int *h), the layer's drawableSize
#define ADDR_ASL_Metal_ForceDrawableSize 0xf2d50 // func - void (view, w, h), [view forceDrawableSizeWidth:w height:h], 0 x 0 unforces
#define ADDR_SDL_GetWindowSize 0x1959c0 // func - void (SDL_Window *, int *w, int *h), in points
#define ADDR_ASL_WindowList 0x104c9bf8 // std::vector<Window *> { begin, end, capacity } - ASL's windows
#define ADDR_R_MonitorTable 0x113c0df8 // { uint32_t count; uint32_t modeCounts[8]; DXGI_MODE_DESC *modes[8]; }, modes from new[]
#define ADDR_R_MonitorSize 0x113c0da4 // { bool valid; int width, height; } - the monitor size fullscreen takes its aspect ratio from
#define ADDR_R_VidRestartPending 0x113c0df0 // bool - set by R_CheckVideoDvarsModified, cleared by R_CheckVidRestart
#define ADDR_R_SwapChain 0x113c0e78 // IDXGISwapChain *
#define ADDR_vidConfig_DisplaySize 0x113c0aec // int[2] - the game's resolution, as applied
#define ADDR_IID_IDXGIDevice 0x104c9a88 // GUID - filled at startup (0x1000cb900)
#define ADDR_IID_IDXGIAdapter 0x104c9a78 // GUID - filled at startup (0x1000cb900)
#define ADDR_dvar_r_fullscreen 0x159ad088 // dvar_t * - FullScreenMode, NULL until the dvar is registered
#define ADDR_dvar_r_fullscreenLast 0x159ad090 // dvar_t * - int 0-2, r_fullscreen's value before it last changed from non-zero (hash 0x847596d0, name unknown)
#define ADDR_R_RestoreFullscreen 0x113c0df4 // bool - fullscreen was left on deactivate (alt-tab) and comes back on activate
// -- end engine_display
// following display changes: SDL 2.0.7 and Aspyr's display layer (sdl_displays.c)
#define ADDR_ASL_BuildModeTable 0x6f076 // func - builds ASL's mode table for SDL display 0, once at startup on the main thread
#define ADDR_ASL_ModeTable 0x2105710 // std::vector<std::vector<mode>> { begin, end, capacity } - ASL's modes, one entry (display 0)
#define ADDR_ASL_WindowMutex 0x104c9bb8 // std::recursive_mutex - held by Window_SetMode while it waits for the main thread, and by present
#define ADDR_SDL_VideoDevice 0x210b1f8 // SDL_VideoDevice * - SDL 2.0.7's _this
#define ADDR_Cocoa_GetDisplayMode 0x146fe0 // func - SDL 2.0.7's static GetDisplayMode(CGDisplayModeRef, CFArrayRef modelist, CVDisplayLinkRef, SDL_DisplayMode *), no _this
#define ADDR_Cocoa_SetWindowFullscreen 0x14e5e0 // func - SDL 2.0.7 (_this, window, display, fullscreen)
#define ADDR_SDL_SetWindowPosition 0x195580 // func - SDL 2.0.7
#define ADDR_SDL_SetWindowSize 0x195830 // func - SDL 2.0.7
#define ADDR_SDL_GetTicks 0x144230 // func - SDL 2.0.7
#define ADDR_SDL_s_moveHack 0x210b1e8 // Uint32 - SDL_cocoawindow.m's s_moveHack: SDL_GetTicks() when SDL last went fullscreen; windowDidMove: undoes moves within 500 ms of it
// -- end sdl_displays
// following display changes: the present path (metalfx.m, gpu_tweaks.c, gpu_diag.m)
// -- end present
// zombies weapons and the cheat check (zm_inventory.c), from the game's give and take console commands (Cmd_Give_f at
// 0xf42c29, Cmd_Take_f at 0xf43169, both called by the client command dispatch at 0xf45eef)
#define ADDR_CheatsOk 0xf428c0 // func: int CheatsOk(gentity_t *ent): sv_cheats is on and ent is alive
// where CheatsOk returns to in Cmd_Give_f (its call at 0xf42c45) and in Cmd_Take_f (at 0xf4317d). Nine other calls ask
// it too, for the other cheat commands (god at 0xf4370f, with GAME_GODMODE_ON/OFF)
#define ADDR_CheatsOk_From_Give 0xf42c4a
#define ADDR_CheatsOk_From_Take 0xf43182
#define ADDR_g_entities_Ptr 0x1966be8 // holds g_entities' address (gentity_t, 0x4f8 bytes each; CheatsOk divides by it)
#define ADDR_bg_weaponVariantDefs 0x14ce0a70 // WeaponVariantDef *[], by weapon index (BG_GetWeaponDef at 0x12362cd)
#define ADDR_bg_lastWeaponIndex 0x14ce0a68 // int: the number of weapons less one (BG_GetNumWeapons at 0x12362a8)
#define gentity_Client 0x250 // gclient_t *
#define gentity_Health 0x2c8 // int, which CheatsOk wants above 0
#define gclient_Weapon 0x2c0 // Weapon: the one in hand (take clears it when it takes that one)
#define gclient_HeldWeapons 0x378 // the held weapons, each starting with its Weapon (give's ammo refill walks them)
#define gclient_HeldWeapon_Size 0x30
#define gclient_HeldWeapon_Count 15
#define WeaponVariantDef_WeaponDef 0x18 // WeaponDef *; the variant's name is its first field
#define WeaponDef_InventoryType 0x7c // int: 0 primary, 1 offhand, 3 alt-fire mode (which give refuses), 4 melee
// -- end zm inventory
// -- END OFFSETS
