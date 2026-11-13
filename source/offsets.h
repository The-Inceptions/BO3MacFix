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
#define ADDR_CL_PlayLogoMovies 0xe752d6 // func - void (void), starts "BO3_Global_Logo_LogoSequence;AspyrLogo" once while ui_playLogoMovie (0xb3b9dc1f) is set, which defaults to 0 only when the command line has devmap (a +map start plays them during the load) - name is a guess
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
#define ADDR_hksi_luaL_unref 0x1445693 // func - void (lua_State *, int index, int ref); 0x147488a jumps to it
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
#define ADDR_Cmd_FindCommand 0xf764e8 // func - cmd_function_s *(const char *name), NULL if there's no such console command
#define ADDR_Cmd_AddCommandInternal 0xf7651d // func - void (const char *name, void (*function)(void), cmd_function_s *alloc), puts alloc (0x30 bytes: next at 0, name at 8, function at 0x20) first in the command list, unless the name is taken
#define ADDR_Sys_GetTLS 0xd0604f // func - the thread's own block, whose pointer at +0x18 is the arguments the running console command reads (the same layout as ADDR_cmd_args) - name is a guess
#define ADDR_CL_ConsolePrint 0x11ca87b // func - void (int localClientNum, int channel, const char *text, int duration, int pixelWidth, int flags), queues text for the console and for the game message windows whose filter takes the channel (window 0's is "gamenotify obituary"); Com_PrintMessage calls it with 0, 0 for the window's own time and width
#define ADDR_CL_ControllerToLocalClient 0xeae875 // func - int (int controller), the local client a controller plays as, -1 for none; Engine.SendMenuResponse (0x1318856) starts with it
#define ADDR_CL_MenuIndex 0x12a6969 // func - int (int instance, const char *menu), the menu's config string index (kind 0x11, 64 of them), 0 if none; instance 1 is the client
#define ADDR_CL_MenuResponseIndex 0x12a6d5b // func - int (int instance, const char *response), the menu response's config string index (kind 0x15, 256), 0 if none
#define ADDR_CL_SERVER_ID 0x117f3678 // int - the server id Engine.SendMenuResponse puts in "cmd mr(p) <server id> <menu> <response>" (0x1115345b0 + 0x2bf0c8)
#define ADDR_ChatClient_Send 0x13bef1c // func - void (int localClientNum, const char *text, int channel), what the chat box calls on enter: sends chat "<channel> <text>" (or say "<text>") to the server; name is a guess
#define ADDR_Scr_LoadScript 0x140a337 // func - int (scriptInstance_t inst, const char *name), loads the scriptparsetree asset <name>.gsc (.csc for the client's) through the loader at 0x149293c (its includes first, then the linker at 0x1492536) - "Script file not found: '%s'", "Error linking script '%s'%s%s"
#define ADDR_Scr_GetBuiltinFunction 0x14904c4 // func - void *(inst, uint32_t name, int *kind, int *minParams, int *maxParams), a builtin function by name hash, as the linker's import resolver (0x1491852) asks; kind 1 is dev-only, 2 is closed to scripts with flag 2 - name is a guess
#define ADDR_Scr_GetBuiltinMethod 0x14904ee // func - the same for builtin methods - name is a guess
#define ADDR_Scr_LinkerSkipsDevCalls 0xe4962f // func - bool (), when true the import resolver leaves the /# #/ imports of a script without flag 2 unlinked - name is a guess
#define ADDR_scrObjTable 0x199a62e0 // the loaded scripts, 500 entries of 0x60 bytes per script instance: the script's bytes at +0 and +8, its name at +0x10, its code's start and end at +0x18 and +0x20 - name is a guess
#define ADDR_scrObjCount 0x199bd9e0 // uint32_t[2]: how many of them per script instance - name is a guess
#define ADDR_Scr_InitVariables 0x12f3177 // func - void (scriptInstance_t inst), clears the instance's script variable table (130,000 entries of 0x40 bytes for the server, 65,000 for the client; allocated once as "ScriptVars" at 0x12f309f, which ends by jumping here) and chains every entry as free: type 0x1b at +8, the next free one at +0x18, entry 0's +0x18 the first; AllocVariable (0x12f36a9) takes from the head and the free at 0x12f35a8 puts back there - name is a guess
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
#define ADDR_scrVmPos 0x14464ff0 // 0x70 bytes per script instance, const char *pos at +0: the running script's code position, which VM_ExecuteInternal (0x11a0beb) and each opcode handler it calls (given its address) advance in place; after a throw its catch (0x11a0d09) puts back the faulting instruction's start for the report, and Scr_ErrorInternal's terminal path reports it as it is - name is a guess
#define ADDR_cgArray 0x145b9fd0 // cg_t * - the local clients' cg_t, 0x342a80 bytes each: refdef view origin at +0x131dc8, view angles at +0x2d8930 (GetCamPosByLocalClientNum / GetCamAnglesByLocalClientNum)
// the client's viewmodel animations (weapon_anim.h): the predicted player state is at cg_t +0x11a8d0, its
// handState[0].weapAnim (uint16_t) at cg_t +0x11a934 and handState[1].weapAnim at +0x11a950 (the netfields at 0x100ddc622)
#define ADDR_CG_UpdateViewWeaponAnim 0xd87230 // func - void (int localClientNum, int), both hands: starts an animation when the predicted weapAnim differs from the one it last started - name is a guess
#define ADDR_cgViewWeaponStates 0x159df5e8 // pointer to 0x3a0 bytes per local client: the weapAnim each hand last started, uint16_t at +0x36c (hand 0) and +0x374 (hand 1) - name is a guess
#define ADDR_Prepatch_Header 0x1964080 // pre-patched executable: header and stubs, in __TEXT's padding after __eh_frame
#define ADDR_Prepatch_CodeEnd 0x1965000 // end of that padding
#define ADDR_Prepatch_Stubs2 0x1900 // pre-patched executable: the stubs that don't fit there, in the padding between the Mach-O header's load commands (they end at 0x17f8) and __text
#define ADDR_Prepatch_Stubs2End 0x2100 // where __text starts
#define ADDR_Prepatch_Slots 0x1a268710 // pre-patched executable: slots, in __DATA's zero-filled tail after __common
#define ADDR_Prepatch_SlotsEnd 0x1a269000 // end of __DATA

// the crosshair and where shots go (aae_freeaim.c) - names are guesses
#define ADDR_Bullet_Fire 0xe285ad // func - void (gentity_s *attacker, float spread, the shot's parameters, gentity_s *weapon?, int): the server fires a bullet. The parameters begin with the shot's forward, right and up (3 floats each) and its muzzle (Windows 0x141580000)
#define ADDR_Grenade_Spawn 0x1022bc8 // func - gentity_s *(gentity_s *owner, start, direction, weapon, int time, bool, bool): the server makes a grenade for the entity that throws or fires it; the new entity has where its way begins at +0x1c, its velocity at +0x28 and its angles at +0x23c (Windows 0x141a1c210)
#define ADDR_Rocket_Spawn 0x1023e64 // func - gentity_s *(gentity_s *owner, weapon, start, direction, the gun's velocity, target, the target's offset): the server makes a rocket (Windows 0x141a194a0)
#define ADDR_Projectile_Spawn 0x10230bf // func - gentity_s *(the same seven, bool): the server makes any other projectile; one caller, 0x12d00ea (Windows 0x141a1c600)
#define ADDR_CL_FinishMove 0x149f3aa // func - void (int localClientNum, usercmd_s *): the client finishes the command it sends the server for this frame (the angles times 182.04445, the buttons); called twice from 0xd2190b (Windows 0x141340710)
#define ADDR_CL_AddReliableCommand 0x1109e58 // func - void (int localClientNum, const char *): queues a command the server is sure to get; at most 128 unacknowledged (Windows 0x14135d4e0)
#define ADDR_clientConnections 0x115345a0 // clientConnection_t *: the local clients' connections, 0x25780 bytes each; reliable commands sent at +0x130, acknowledged by the server at +0x134 (Windows 0x145359bb8)
#define ADDR_sv_state 0x144e8500 // int: the server's state, 0 while this machine runs none (the first field of sv; Windows 0x15767ac90)
#define ADDR_SV_ExecuteClientCommand 0x105b379 // func - void (client_t *, const char *text, int clientOK, int fromOldServer): the server runs a command a client sent; one caller, 0x105c1ce (Windows 0x1421ed830)
#define ADDR_svs_clients 0x144e7c18 // client_t *: the server's clients, 0xe5270 bytes each (0xe5170 on Windows), the state an int at +0 with 5 for one that is in the game (Windows 0x15767a398)
#define ADDR_CG_DrawCrosshairTest 0xd54e99 // func - bool (cg_t *, playerState_s *): whether a crosshair is drawn at all; no while the view is third person (cg + 0x11a8c9), unless the weapon has flag 0x4000 (Windows 0x1406e9c90)
#define ADDR_CG_DrawCrosshair_TestReturn 0xd54fac // where the crosshair function (it starts at 0xd54f45, (int localClientNum)) goes on after asking that (Windows 0x1406d062f)
#define ADDR_CG_DrawCrosshair_Tests 0xd555d7 // "cmp bl, 0 ; jne" in the crosshair function: behind the crosshair's colour and the test of its alpha, where the tests for the sights being up (the function's copy of the player state's +0x2fc is at rbp - 0x50) and for what the weapon is doing begin; cg_t in r13 (Windows: 0x1406d5911 and the call at 0x1406d595c, where that row of tests is a function of its own, 0x1406c5aa0)
#define ADDR_CG_DrawCrosshair_Draws 0xd55755 // where that goes on when all those tests pass
#define ADDR_CG_DrawCrosshair_Gap 0xd561de // "movss xmm0, [r12 + 0xe0c]": the gap between the crosshair's lines is in xmm1, the larger one of the spread's and the weapon's least (an int at the weapon definition's +0x6f0, r12); cg_t in r13 (Windows 0x1406db615, gap in xmm12)
// the client's own copy of a shot (aae_freeaim.c) - names are guesses
#define ADDR_Bullet_Endpos 0xd39481 // func - void (int *seed, float spread, float spread2, const float *start, float *end, float *direction, float fraction, float fraction2, const float *forward, const float *right, float range, const float *up, weapon, int pellet, int pellets): the client works out where its own copy of a shot ends and its direction, from start along forward, spread with right and up (Windows 0x14123db70)
#define ADDR_Bullet_Endpos_Return1 0xd3b7a0 // where the client's fire function (0xd3b59a (int localClientNum, centity_s *shooter, ...), which keeps the shooter at rbp - 0x58 all along) goes on after its first call of that: weapons whose shots aren't queued (Windows 0x14123a4f2)
#define ADDR_Bullet_Endpos_Return2 0xd3bad3 // and after its second: every other bullet, which is then queued for its tracer and impacts (Windows 0x14123a7b3)
#define ADDR_Bullet_QueuedShots_Loop 0xd38755 // "imul rcx, rsi, 0x8b0": the head of the loop over the client's queued shots, after the jobs that trace them are done, in the function that draws their effects (0xd3870e): the queue's entries (0x8b0 bytes each, at most 16, at 0x10876ba0, their count at 0x10876b98) at r13, this one's index in rsi. An entry: the local client (int) at +0, the shot's parameters at +4 (where it was fired from at +0x10 in those, where it starts at +0x1c), the shooting entity's number at +0x58, where the tracer starts at +0x64 (Windows: the per-shot function 0x14123de80 called at 0x1412419ef)
#define ADDR_Com_GetClientDObj 0xf543c3 // func - DObj *(int entity number, int localClientNum) (Windows 0x1420f5680)
#define ADDR_CG_DObjGetWorldTagPos 0x10b5d51 // func - bool (centity_s *, DObj *, scr_string_t tag, float position[3]): where a tag of an entity's model is in the world (Windows 0x1401abd50)
#define ADDR_scr_tag_flash 0x107e1384 // scr_string_t: "tag_flash", made when the client's game starts (at 0xd1593f, between tag_clip_le and tag_flash_le; Windows 0x14a6aa400)

// the campaign's prompts in the world, which All-around Enhancement 4 uses in zombies (aae_prompt.c) - names are guesses
#define ADDR_Prompt_NearTest 0x1304343 // "mov edi, 2 ; call Com_SessionMode_IsMode ; test al, al ; je" in the code that places a prompt on the screen: only the campaign keeps a prompt's "nearTo" model (Windows: the call at 0x14275a7ae)
#define ADDR_Prompt_NearTest_Yes 0x1304355 // where that goes on after a yes
#define ADDR_Prompt_FarTest 0x13052bc // the same before a prompt's "isFarAway" model is set and its text becomes the distance (Windows 0x14274dd9c)
#define ADDR_Prompt_FarTest_Yes 0x13052ca
#define ADDR_Prompt_NotFarTest 0x1305344 // the same before "isFarAway" is cleared (Windows 0x14274de81)
#define ADDR_Prompt_NotFarTest_Yes 0x1305352
#define ADDR_Prompt_NoObjective 0x1305337 // "cmp dword [rbx + 0x154], -2 ; je": a prompt (its UI element in rbx) without an objective skips the rest (Windows 0x14274de6f, element in rdi)
#define ADDR_Prompt_Placed 0x1304895 // "movups xmm0, [r14 + 0x34]" in the placing code: the prompt (element r14) has its place on the screen (two floats at rbp - 0x70) and its rectangle is about to be written; the time for moves is in xmm7, the frame's ms in r15d, the screen's width over its height at rbp - 0x54 (Windows 0x14275e372)
#define ADDR_Prompt_NotPlaced 0x1304789 // "movss xmm5, [rbp - 0x54]": the other branch, the prompt is kept at a fixed place of the screen (Windows 0x14275e3b8)
#define ADDR_Prompt_IsFarAwayText 0x1742145 // "isFarAway"
// the grenade's prediction line (aae_arc.c): T7Overcharged's names in quotes
#define ADDR_CG_ShouldPredictArc 0x11f5450 // func - bool (int localClientNum, Weapon): whether the job predicts an arc this frame; its only caller is the job (Windows 0x1405f8790, "OFF_CG_ArcPrediction_ShouldDraw")
#define ADDR_CG_ArcPrediction_NoTacticalMode 0x11f29a5 // "movss xmm0, [rbp - 0x2c]" in the job (it starts at 0x11f255f): the branch for Tactical Mode (0x11f187c) being off; only the je at 0x11f2975 comes here, right behind the test's call (Windows: the call at 0x1405fbf3e, "OFF_CG_ArcPrediction_TmodeNop2")
#define ADDR_CG_ArcPrediction_TacticalMode 0x11f2977 // the branch for it being on: at most 3000 ms ahead, in steps of a 50th of that, at least 3
#define ADDR_CG_ArcPrediction_WeaponDef 0x11f2643 // "mov [rbp - 0xa0], rax" in the job right behind its BG_GetWeaponDef: the definition in rax, the weapon in rbx (Windows: the call at 0x1405fbbd3)
#define ADDR_CG_ArcPrediction_AddSegment 0x11f531b // func - void (int localClientNum, trace *, float, float): adds a segment to the arc, the floats are its thickness at both ends (xmm0, xmm1); the job's two calls (Windows 0x1405f11b0)
#define ADDR_CG_ArcBlocks 0x148a13e0 // per local client, 0xeec bytes: 5 segments of 60 points (vec3), their point counts at +0xe10, the number of segments at +0xe60, the end marker's byte at +0xee9 (Windows 0x1447992b0)
#define ADDR_CG_DrawArc_FirstLine 0x11f4b37 // "lea rdi, [rbp - 0xa8] ; call" in the arc's draw function (it starts at 0x11f4926 with the local client at rbp - 0xac): the first loop over the segments hands the game a draw command (start +4, end +0x10, last point +0x40, points +0x68) (Windows: the call at 0x1405f684d)
#define ADDR_CG_DrawArc_SecondLine 0x11f4c4a // the same in its second loop (Windows: the call at 0x1405f697c)
#define ADDR_CG_DrawArc_MarkerTest 0x11f4c8b // "cmp byte [r15 + rsi + 0xee9], 0 ; je": whether the end marker is drawn, rsi the arcs, r15 the local client's offset (Windows 0x1405f69a7)
#define ADDR_CG_ArcMaterials 0x1536c4f8 // 4 Material *: what the draw function gives its commands (+0x38), copied from the client's media (+0x258 to +0x270) at 0x12ba85e
#define ADDR_R_AddCodeMeshDrawSurf 0xe8a35d // func - void (Material *, ... 9 arguments): reads material + 0x278 first (Windows 0x141c92ce0, "OFF_R_AddCodeMeshDrawSurf")
#define ADDR_DObjCalcSkel 0xe398de // func - (DObj *, const uint32_t parts[12], const float origin[3] or NULL): works out a model's skeleton; its callers set no other register and use nothing it returns; hooked once for every piece, see aae_skeleton.c (Windows 0x1422c0480)
#define ADDR_DObjCalcSkel_ViewModelReturn 0x10b55ea // where the view model's two calls of it return to: the call at 0x10b55e5 (Windows 0x1401abb5c) and the one at 0x14bb2c0 (Windows 0x14099da09)
#define ADDR_DObjCalcSkel_ViewModelReturn2 0x14bb2c5
#define ADDR_DObjGetBoneIndex 0xf74d4f // func - int (DObj *, scr_string_t name, uint16_t *index, int model): 0xffff as the index makes it say no (Windows 0x1422c4240)
#define ADDR_SL_GetStringOfSize 0x11b6d8d // func - scr_string_t (const char *, int user, int length + 1, int type) (Windows 0x1412d8830)
#define ADDR_BG_GetWeaponDef 0x12362cd // func - WeaponDef * (Weapon) (Windows 0x142670e50)
#define ADDR_BG_WeaponAmmo 0xf5354d // func - int (playerState_s *, Weapon): the ammo of the weapon's slot (ps + 0x378, 15 of 0x30) (Windows 0x14266e850, "BG_WeaponAmmo")
#define ADDR_BG_FindWeaponIndexByName 0x1236c1b // func - int (const char *, the two below): the djb2 of the lowered name looked up, the index & 0x1ff, 0 for none (Windows 0x142670d80, with its tables built in)
#define ADDR_bg_weaponNameHashes 0x5e901d0 // {hash, index} for each weapon, sorted the first time
#define ADDR_bg_weaponNameHashesSorted 0x5e911d0 // a byte: they are
#define ADDR_UIModel_Child 0x10365c3 // func - uint16_t (uint16_t model, const char *name): the model's child of that name, made if it isn't there (0x1036418 doesn't make it)
#define ADDR_UIModel_SetBool 0x1036a60 // func - bool (uint16_t model, bool value): whether that changed it

// entities the client scripts link to the camera, which All-around Enhancement 4's left-hand gestures use (aae_linkcam.c) - names are guesses
#define ADDR_CScr_LinkToCamera 0xef8f94 // func - the client scripts' method linktocamera([mode], [offset]): (instance, the entity's number in the low half of the second, the local client in the high half of the third); adds it to the local client's cg_t list (+0x3272f4: count, 4 numbers, 4 modes, 4 offsets) or makes a script error when 4 are there (Windows 0x140b2cab0)
#define ADDR_CScr_Delete 0xef8174 // func - the client scripts' method delete, the same arguments (Windows 0x140a222f0)
#define ADDR_CScr_ForceDelete 0xef825d // func - and forcedelete (Windows 0x140a25580)
#define ADDR_AddDObjToScene 0xe0f8b8 // func - void (the model, centity_t *, int number, flags, a lighting struct or NULL): draws a client entity (Windows 0x141ce9410)
#define ADDR_CG_CalcViewValues_ViewAxis 0xfbd911 // "cmp ebx, 0xb ; je" right behind AnglesToAxis(cg->viewAngles, cg->refdef.viewaxis) in CG_CalcViewValues, which a frame calls after it has drawn the client entities; cg_t in r12 (and r14), the local client in r15d (Windows: the call at 0x1410ae259)
#define ADDR_AnglesToAxis 0x1352ba1 // func - void (const float angles[3], float axis[3][3]); reads 16 bytes of angles (Windows 0x14224e620)
#define ADDR_CG_EntityPoseChanged 0x104b9ab // func - void (int localClientNum, int entnum): what the game calls after it moved a client entity's origin or angles (Windows 0x1412ab840)

// the host's leave decision and the migration that follows it (aae_hostmig.c) - names are guesses
#define ADDR_HostMigrationStart_Decision 0x140daf1 // "test al, al ; je <end the match> ; mov edi, 4" in the server command hostmigration_start (it starts at 0x140dae8): right behind its call of the game's test whether it may migrate (0x127b0c2: no mod loaded, online, game mode not 1, 5 or 6; Windows 0x1420ead00), al its answer. Then it asks Com_SessionMode_IsGameMode(4) (0x127af8e, freerun) and ends the match (0xff422b) on either (Windows: the command at 0x1421e8cf0, its calls at 0x1421e8cf4 and 0x1421e8d02)
#define ADDR_HostMigrationStart_Migrate 0x140db09 // where it goes on to migrate: sv_running, no migration in progress, then the migration's start (0xe06d3e, Windows 0x1421f9bb0)
#define ADDR_SessionMode 0x6939a14 // uint32_t: the session's mode, its network mode in bits 6 to 9 (2 online), its game mode in bits 14 to 17 (1 a custom game, 4 freerun) (Windows 0x15686e874)
#define ADDR_HostMigration 0x28290e0 // the migration: int state at +0 (1 the players are asked to rate themselves, 4 they're told where the new host is), 18 entries of 0x20 bytes for the players at +8, the new host's entry (int, -1 none) at +0x3c8 and xuid at +0x3d0, a byte "in progress" at +0x3e4 (Windows 0x157756be0)
#define ADDR_HostMigration_PickNewHost 0xe07413 // func - void (void): picks the new host, the best rating above 1, or ends the match (Windows 0x1421f9530, jumped to by the migration's frame at 0x1421f9853, here 0xe071e7)
#define ADDR_HostMigration_SendToNewHost 0x123297b // func - bool (int controller, uint64_t new host's xuid, int lobby type, uint64_t xuids[], int count): the "migrate to" message to those players, through that lobby if the new host is a member of it (Windows 0x141ed50b0, its one call at 0x1421fa299, here 0xe076b9)
#define ADDR_LobbySessions 0x101c8760 // the lobby sessions, 0x66828 bytes each: 0 the private (party) one, 1 the game's; state at +0x40 (2 when it can migrate), 18 members of 0x30 bytes at +0xf8, their xuid first (Windows 0x1555fd410)

// T7Overcharged's slide changes (aae_move_slide.c): the player state (ps) and the move (pml_t) are at the same places as
// in the Windows game; T7O's names for the places (wip/night/agent-t7o-movement.md, sections 3, 4, 6)
#define ADDR_CanSlide_SpeedTest 0xe814f5 // detour - in the test whether a slide may start (0x100e8148b, Windows 0x1401684b0): "ucomiss xmm0, xmm2; jbe", xmm0 the least speed 150, xmm2 the horizontal speed (OFF_InstantSlide_Patch, Windows 0x140168534)
#define ADDR_PM_Accelerate 0x1147ed9 // func - void (playerState_s *, pml_t *, const float wishdir[3], float wishspeed, float accel) (Windows 0x14260ecd0, whose wishspeed is in xmm3 and accel on the stack)
#define ADDR_PM_CmdScale_SlideRampCmp 0x11486ac // detour - in PM_CmdScaleImpl (Windows 0x142611630): "sub ecx, [rdx+0x844]; cmp ecx, eax; jge", the time since a slide ended against the length of the speed ramp after it (OFF_SlideSlowdown_Patch1, Windows 0x142611575)
#define ADDR_PM_DoSlideAdjustments 0x11488f0 // func - void (playerState_s *, pml_t *): the push of a sliding player after each move's PM_Accelerate (Windows 0x1426121f0)
#define ADDR_PM_Friction_SlideFriction 0x113c526 // detour - in PM_Friction (Windows 0x142613540): "mulss xmm0, xmm2; mulss xmm0, [r14+0x24]", the extra friction on a slide surface, its 1.5 in xmm0 (OFF_SlideSlowdown_Patch2, Windows 0x1426136f4)
#define ADDR_PM_UpdateSlide 0xe8189d // func - void (pmove_t *, pml_t *): a slide's start (on the ground: the command's slide button, pm+0x13 bit 0, while sprinting at 100; on landing after one was asked for in the air), its course and end; once a move from PmoveSingle (0x101140ef6); Windows 0x1401683a0 - name is a guess (hooked only for BO3MACFIX_MOVEDEBUG)
#define ADDR_PM_UpdateViewAngles_SlideClamp 0x113e4b3 // detour - in PM_UpdateViewAngles: "mov edx, 20" before the call of PM_ClampViewAngles (0x10113e860, Windows 0x142618590) that clamps the view's pitch while sliding
#define ADDR_PM_UpdateViewAngles_SlideClampDone 0x113e4c3 // behind that call
#define ADDR_StartSlide_PitchLoad 0xe82226 // detour - in a slide's start (0x100e82086, Windows 0x140169080): "movss xmm0, [rax+0x318]", the view's pitch, compared with 10.05 next to pull the view to 10 (OFF_SlideAngles_Patch, Windows 0x140169268)

// traversal, the way over obstacles from multiplayer and the campaign (aae_mantle.c) - names are guesses
#define ADDR_level_clients 0x1096ee40 // gclient_s *: the game's players, 0x17210 bytes each (0x20 more than in the Windows game: what comes after the player's state is that much further). The number one was connected as is at +0x16c18 (Windows +0x16bf8)
#define ADDR_sv_gentities 0x144fbde8 // gentity_s *: the entities the server was last given (what the game's start gives it is the array at 0x110cea410), 0x4f8 bytes each: type (a word, 15 a vehicle) at +0x1a0, origin +0x230, angles +0x23c, client +0x250, vehicle data +0x270
#define ADDR_sv_gentitySize 0x144fbdf0 // int: and how big each is
#define ADDR_Vehicle_MantleSide 0xd691e0 // func - int (gentity_s *vehicle, gentity_s *player): which of the vehicle's sides to climb it from the player is at, 4 for none (Windows 0x141b8bb30)
#define ADDR_CG_DrawMantleHint 0x112fe20 // func - void (int localClientNum, the HUD element, ...): draws the prompts for mantling, a hatch and mounting, by the predicted player state's mantle flags (cg_t + 0x11b0b4: 0x10 mantle, 0x80 mount, 0x800 hatch)
#define ADDR_TRM_CanStart 0x14c4f60 // func - bool (pmove_t *, pml_t *)
#define ADDR_TRM_Init 0x14c5a9e // func - void (alloc function): makes the traversal animations ("PLAYER_TRM") at a map's start, if trm_enabled
#define ADDR_TRM_Scan 0x14c3d7e // func - bool (pmove_t *, pml_t *, vec3 *ledge, vec3 *normal)
#define ADDR_TRM_Type 0x14c50bc // func - int (playerState_s *, vec3 ledge by value)
#define ADDR_PM_PlayerTrace 0x113b850 // func - void (pmove_t *, trace_t *, start, mins, maxs, end, passEntityNum, contentMask)
#define ADDR_GameTypeSettings_GetFloat 0xd973da // func - float (const char *name, bool)

// T7Overcharged's movement patch fastjump (aae_move_fastjump.c)
#define ADDR_PM_AirMove_AccelerateReturn 0x1147411 // where PM_AirMove (0x1147137) goes on after the first of its two calls of it (Windows 0x14260f320)
#define ADDR_PM_WalkMove_AccelerateReturn 0x1147096 // where PM_WalkMove (0x1146cf7) goes on after the first of its two (Windows 0x14261a6d5)
#define ADDR_PM_CheckJump_DelayCmp 0x14dd451 // "cmp ecx, 500 ; jl <no jump>" in PM_CheckJump (0x14dd41e), ecx the command's time minus the player state's jump time (+0x110) (Windows 0x1425fce51)
#define ADDR_PM_Friction 0x113c32e // func - void (pml_t *, pmove_t *): the ground's friction on the velocity (Windows 0x142613540)
#define ADDR_PM_Friction_GotFriction 0x113c500 // "mulss xmm0, [rbp - 0x1c]" right behind PM_Friction's main call of BG_GetFriction (0x1073e6b, the constant 5.5): the friction in xmm0, the player state in rbx, the pmove in r15, the pml in r14 (Windows: behind the call at 0x1426136cd)
#define ADDR_PM_IsInAir 0x113d293 // func - bool (pmove_t *, pml_t *): no ground entity (0x3ff), pm_type not 1 or 7, not 0x1398d1f(ps), and pml + 0x34 is 0 (Windows 0x142614860)

// T7Overcharged's jumpslowdown, which All-around Enhancement switches on (aae_move_jump.c) - names are T7Overcharged's
#define ADDR_BG_GetViewDip 0x113caec // func - int (playerState_s *, float fall speed): how far the camera dips on a landing; called by the landing (0x114669c) and the client's view (0x121e7f9) (Windows 0x14260e710, the speed in xmm1)
#define ADDR_Jump_ReduceFriction 0x14dcf3a // func - float (playerState_s *): PM_Friction's (0x113c32e) multiplier of the ground's friction while movement flag 18 is set, 2 down to 1 over the slowdown after a jump (Windows 0x1425fd630)
#define ADDR_Jump_SlowDown 0x14dceb2 // func - void (playerState_s *): PM_WalkMove's start while movement flag 18 is set: the first frame on the ground after a jump multiplies the velocity by 0.8 and starts the slowdown (pm_time 1000) (Windows 0x1425fcd70)
#define ADDR_Jump_Start_ClampSpeed 0x14dd2c0 // "movsd xmm1, [r12 + 0x3c]" in Jump_Start (0x14dd05e, (pmove_t *, pml_t *, float height)): the cap of the jump's horizontal speed (Windows' Jump_ClampHorizontalVelocity 0x1425fd040, inlined here) is in xmm0, the player state in r12, the pmove_t in r14; next "ucomiss speed squared, cap squared ; jbe <no cap>"
#define ADDR_Jump_Start_SlowdownTest 0x14dd146 // "mov esi, 13 ; mov rdi, r15 ; call <active gadget> ; test al, al ; je" in Jump_Start: a jump while the slowdown is pending asks whether gadget 13 spares the lowered jump (Windows 0x1425fd7b4; T7Overcharged's OFF_Jump_SlowdownPatch is its je at 0x1425fd7c3)
#define ADDR_Jump_Start_SlowdownTest_Yes 0x14dd15b // where that goes on after a yes: the divisor of the upward speed's square is 1
#define ADDR_PM_GroundTrace_CrashLandBegin 0x1146389 // "mov r12, [r12] ; mov rdi, r12" in PM_GroundTrace (0x1145e07): the landing begins (Windows' PM_CrashLand 0x142611a30, inlined here); r12 the pmove_t, r13 the pml_t, r15 the player state
#define ADDR_PM_GroundTrace_CrashLandEnd 0x114698d // "and byte [r15 + 0x908], 0xf9": every way through the landing comes out here, and only those (Windows: where PM_CrashLand returns to, 0x1426145e5); r15 the player state

// T7Overcharged's changes to sprinting (aae_move_sprint.c); the names are T7Overcharged's (its OFF_... records)
#define ADDR_Jump_Start_SprintLeapCmp1 0x14dd0c1 // "cmp ecx, 1500 ; jg" in Jump_Start (it starts at 0x14dd05e, (pmove_t *, pml_t *, float height)): ecx the ms of sprint, full sprint-leap height past 1500; pmove_t in r14, playerState in r15 (Windows 0x1425fd726, "cmp eax, 1500", OFF_Jump_SprintLeapMs1)
#define ADDR_Jump_Start_SprintLeapCmp2 0x14dd200 // "cmp eax, 1500 ; jg" there: eax the ms of sprint, full sprint-leap speed past 1500; the same registers (Windows 0x1425fd849, OFF_Jump_SprintLeapMs2)
#define ADDR_PM_CmdScale_SprintLerpCmp 0x1148644 // "cmp eax, 349 ; movss ; jg" in PM_CmdScaleImpl (it starts at 0x1148438, (pmove_t *, usercmd *)), where the move speed's scale is inlined: eax the ms of sprint, the speed no longer ramped up past 349; pmove_t in r15 (Windows 0x1426114d8 "cmp ecx, 350 ; jge" in 0x1426113d0, OFF_Jump_SprintLerpInMs)
#define ADDR_PM_UpdateSprint 0x1143aa2 // func - void (pmove_t *, pml_t *): the sprint decision, with what Windows has in functions of its own (0x142613b20, 0x142615c70, 0x142615e60, 0x142617df0) inlined (Windows 0x142617cb0, OFF_PM_UpdateSprint_hook)
#define ADDR_PM_UpdateSprint_SlideStateCmp 0x1143e1b // "cmp dword [rbx + 0x8c], 3 ; je <no sprint>" there, in the inlined test whether a sprint may start: playerState in rbx, state 3 the slide state (Windows 0x142615f77, its je at 0x142615f7e is OFF_CanSlideThenSprint_Patch)
#define ADDR_PM_UpdateSprint_SlideStateCmp_Past 0x1143e28 // behind that je: the test's next question
#define ADDR_PM_WalkMove_StrafeScaled 0x1146d54 // "mulss xmm0, [rbp - 0x2c]" in PM_WalkMove (it starts at 0x1146cf7, (pmove_t *, pml_t *)) right behind its call of BG_GetSprintStrafeSpeedScale (0x1073e5d, returns 0.667): xmm0 its result, [rbp - 0x2c] the side stick as a float, pmove_t in rbx (Windows: the call at 0x14261a391 of 0x1426043e0, OFF_BG_GetSprintStrafeSpeedScale_hook)

// the player's storage files: stats, loadouts and settings, on disk and online (display_testhooks.m) - names are guesses
#define ADDR_Storage_Request 0x1102f5f // func - bool (controller, op: 0 read, 1 write, file, slot): asks for one of them to be read or written
#define ADDR_Storage_Files 0x47fa680 // per controller (0x8558 bytes apart) 64 entries of 0x210 bytes, { descriptor *, int slot, buffer, ... }; a descriptor has the file's id at +0, its name at +8, its size at +0x18 and where it's kept at +0x20: 0 hdd, 1 dwuser (online), 2 memory, 3 fastfile

// the game's memory for what it loads (pmem_spill.c)
#define ADDR_PMem_Alloc 0xd32be1 // func - void *(size, alignment, type of address space 0-4, stack 0-10, tag)
#define ADDR_PMem_Stacks 0x2522a18 // 11 stacks of 0xf68 bytes: the fill of their 5 address spaces at +0x10, the pools' pointers at +0xf40
#define ADDR_AddressSpace_Create 0xe47989 // func - bool (name, flags, size, address space *): reserves it and adds it to the game's list - name is a guess

// the Lua VMs' memory (lua_heap.c) - names are guesses
#define ADDR_Lua_HeapCreate 0x144849f // func - heap *(init): a dlmalloc heap in the block init's callback gives it, for a Lua VM
#define ADDR_LUI_HeapAlloc 0xd4d640 // func - the UI VM's callback for its heap's block: void *(user, block, old_size, new_size), gives the 60 MB "LUI_Main" block once
#define ADDR_LUI_HeapBlock 0x2564bc8 // void *, that block; the heap itself starts 16 bytes into it
#define ADDR_LUI_HeapBlockTaken 0x2564f40 // bool, whether a heap has it

// the TrueType glyph cache and its atlas (font_resolution.c) - the FontCache names are guesses
#define ADDR_FontCache_GetGlyph 0x1278311 // func - glyph *(face, height, character, effect): the cached glyph, made first if it's new
#define ADDR_FontCache_Reset 0x127920f // func - void (): empties the cache and the atlas
#define ADDR_FontCache_Allocate 0x1279a54 // func - bool (packer, width, height, rotate, int position[3]): a free rectangle of the atlas
#define ADDR_FontCache_AllocateReturn 0x12786e4 // where FontCache_GetGlyph's call of FontCache_Allocate returns to
#define ADDR_FontCache_Upload 0x12799b2 // func - void (left, top, right, bottom): queues that part of the atlas for the GPU
#define ADDR_FT_Outline_Render 0x474e9a // func - FreeType's, int (library, outline, params)
#define ADDR_FontCache_RenderReturn 0x1278c5f // where FontCache_GetGlyph's call of FT_Outline_Render returns to
#define ADDR_FontCache_Bitmap 0x631d2c0 // the atlas: 2048x2048 bytes

// controllers
#define ADDR_GPad_GetGamepadType 0x122f78d // func - returns gamepadType_e, Aspyr's always says DURANGO for a connected pad
#define ADDR_g_gpads 0x5dedb70 // gamepad[2], 0x70 bytes each, mapped port (0-3) at +4 - name is a guess
#define ADDR_g_gpadPorts 0x5dedc50 // { int xinputUserIndex; int controllerIndex; }[4] - name is a guess
#define ADDR_ASL_XInputSlots 0x2108d50 // { SDL_GameController *; SDL_Haptic *; int effect; }[4] - name is a guess
#define ADDR_ASL_XInputGetState 0x1294a8 // func - DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState)
#define ADDR_ASL_XInputSetState 0x1295ac // func - DWORD XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration)
#define ADDR_ASL_XInputEnabled 0x1b97300 // bool - XInputEnable state, starts true and nothing changes it - name is a guess
#define ADDR_ASL_XInputRefresh 0x129170 // func - void (void), on SDL_CONTROLLERDEVICEADDED/REMOVED (its event filter at 0x129157): under ADDR_ASL_XInputMutex, closes detached controllers, then puts each newly opened game controller in the first slot whose controller is NULL - name is a guess
#define ADDR_ASL_XInputMutex 0x1b972c0 // std::mutex (a pthread_mutex_t) - held by ASL_XInputRefresh and the two functions that empty the slots (0x129364, 0x12972c); the XInputGetState/SetState bodies read the slots without it - name is a guess
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
#define ADDR_Main_RenderWait_TimeoutInstr 0xd05c07 // mid-func - mov esi, 1 (main thread's wait for the render thread, in ms)
// following display changes: the engine's video code (engine_display.c)
#define ADDR_R_CheckVidRestart 0xdce17e // func - per frame on the engine's Main thread, applies pending video dvar changes
#define ADDR_R_SetWndParms 0xdcca9a // func - fills the window parameters from r_fullscreen, r_monitor, r_mode, r_refreshRate
#define ADDR_SyncHardwareProfileWithDvars 0xeb1beb // func - void (bool make_latched_current), the dvars' current values into the hardware profile's
#define ADDR_HWProfile_QuitSyncReturn 0xeb1c80 // where the quit's call to it returns, before SaveHardwareProfile writes config.ini
#define ADDR_HWProfile_Keys 0x3a78960 // 58 x { const char *key; uint32_t dvar_hash; const char *comment; }, 0x18 bytes each
#define ADDR_HWProfile_Values 0x3a78ed0 // 58 x char[0x40], what SaveHardwareProfile writes to config.ini
#define ADDR_R_EnumDisplayModes 0xdcc7c6 // func - bool (IDXGIAdapter *), fills the monitor table, only R_InitHardware calls it
#define ADDR_R_EnterFullscreen 0xdccd55 // func - SetFullscreenState(TRUE, output r_monitor - 1) on the window thread (message 0x8044)
#define ADDR_R_CheckVideoDvarsModified 0xdce0ad // func - vid_restart: marks a restart pending if a video dvar has a latched value
#define ADDR_Dvar_FindByHash 0xf9fffa // func - dvar_t *(uint32_t hash)
#define ADDR_Dvar_GetInt 0xfa0252 // func
#define ADDR_Dvar_GetFloat 0xfa036d // func
#define ADDR_Dvar_GetString 0xfa0474 // func
#define ADDR_Dvar_SetFromString 0xfa2ef7 // func - (dvar, value, source), sources 1 and 2 only set a latched dvar's latched value
#define ADDR_Dvar_SetInt 0xfa2d59 // func - (dvar, value)
// the game's dvar registration, which T7Overcharged's movement component registers its dvars with (aae_movement.c):
// each gives the dvar back; an existing one that was set before as text ("External Dvar", flag 0x400) gets the type,
// its text turned into a value and put inside the domain (0xfa4dc5, 0xfa30ff, 0xfa00b5)
#define ADDR_Dvar_RegisterBool 0xfa1328 // func - dvar_t *(uint32_t hash, const char *name, bool value, uint32_t flags, const char *description) (Windows 0x142273de0)
#define ADDR_Dvar_RegisterInt 0xfa149e // func - dvar_t *(uint32_t hash, const char *name, int value, int min, int max, uint32_t flags, const char *description) (Windows 0x142273fc0)
#define ADDR_Dvar_RegisterFloat 0xfa1614 // func - dvar_t *(uint32_t hash, const char *name, float value, float min, float max, uint32_t flags, const char *description) (Windows 0x142273f40)
#define ADDR_Dvar_SetName 0xfa4242 // func - void (dvar_t *, const char *name): gives a dvar its name (+8) the game's way: releases the one there (FreeString 0x1254262) and stores the game's own copy (CopyString 0x125420f), which Dvar_Shutdown (0xfa0aa4) releases at every quit; the register function calls it for dvars with flag 0x20000 (0xfa49cf) - name is a guess
// fast ADS in the ADS update (0xed5ad3: how far the sights are up, the player state's +0x2fc, moves by msec * rate), as
// T7Overcharged's fastads patch changes it (aae_movement.c): the perk test, and the doubling behind it
#define ADDR_ADS_FastAdsPerkTest 0xed5e2b // mid-func - "test r15b, r15b ; je 0xed5e64": r15b "fast", whether the player has the fast-ADS perk (perk 23 at ps+0xb04, or the gadget's weapon's +0x286), rbx the player state, r12 the weapon's definition (anim type at +0x68), r13b whether the sights go up; reached whenever the sights move, the perk or not (Windows 0x14265e2f3 "test bl, bl ; je")
#define ADDR_ADS_FastAdsTestsPassed 0xed5e58 // where the game goes on when the perk test and both anim type tests pass ("movaps xmm0, [rbp - 0x40]", the rate), past the two tests for sniper rifles (0xed5e30..0xed5e56: [r12 + 0x68] against the getter 0xfdcf14's 1 and 2; Windows 0x14265e2f7..0x14265e313, the two "je" T7Overcharged makes "nop")
#define ADDR_ADS_SniperAnimTypes 0x4076854 // int[2]: the player anim types "sniper" and "rearclipsniper", the getter 0xfdcf14's 1 and 2 (filled when the anim types are read, 0xfdd0c3..0xfdd179), for the debug line
#define ADDR_ADS_FastAdsDouble 0xed5e5c // mid-func - "addss xmm0, xmm0 ; movaps [rbp - 0x40], xmm0": the rate in xmm0 doubled and kept (Windows 0x14265e315 "mulss xmm6, [2.0]", which T7Overcharged points at its multiplier)
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
// ResolutionPercent: int 50-200, LATCH (Dvar_RegisterInt at 0x101333f57). Each frame the view setup (0x100fbce5d) reads its
// current value and asks 0x100e8d843 for render targets of that share of the view's size: an LRU cache of 8 sets that
// allocates a set on a miss and frees sets 1-7 after 11 frames unused. A latched value instead makes R_CheckVidRestart
// (0x100dce192) apply it and rebuild every render target (R_ResizeBackBuffer).
#define ADDR_dvar_r_sceneResolutionMultiplier 0x159adce8 // dvar_t * - hash 0xbb27457f, NULL until registered
// the render target sets' cache, read by the lookup at 0x100e8d843 (w, h): a pointer to the render target table (0x6e
// entries of 0xae0 bytes); the size of set i (0-7) is the 16-bit width and height at +0x6cc0 + i * 0x150 (0 when free).
// At each lookup the engine frees sets 1-7 last used 11 or more frames before the frame counter (and sets their last
// use to 0); a miss takes a free set, or the least recently used one when all 8 are in use.
#define ADDR_rt_table 0x118fb5e8         // render target table pointer
#define rt_set_size 0x6cc0               // + set * rt_set_stride: uint16 width, uint16 height
#define rt_set_stride 0x150
#define ADDR_rt_set_last_use 0x03519f70  // uint64[8]: the frame counter at each set's last use
#define ADDR_rt_frame_counter 0x113b7b30 // uint64 (0x1113b7880 + 0x2b0), what the lookup compares the last use with
#define Hash_com_maxfps 0x29b732e8 // MaxFPS
#define ADDR_dvar_r_fullscreenLast 0x159ad090 // dvar_t * - int 0-2, r_fullscreen's value before it last changed from non-zero (hash 0x847596d0, name unknown)
#define ADDR_R_RestoreFullscreen 0x113c0df4 // bool - fullscreen was left on deactivate (alt-tab) and comes back on activate
// -- end engine_display
// following display changes: SDL 2.0.7 and Aspyr's display layer (sdl_displays.c)
#define ADDR_ASL_BuildModeTable 0x6f076 // func - builds ASL's mode table for SDL display 0, once at startup on the main thread
#define ADDR_ASL_ModeTable 0x2105710 // std::vector<std::vector<mode>> { begin, end, capacity } - ASL's modes, one entry (display 0)
#define ADDR_ASL_WindowMutex 0x104c9bb8 // std::recursive_mutex - held by Window_SetMode while it waits for the main thread, and by present
#define ADDR_SDL_KeyboardFocus 0x21097b0 // SDL_Window * - SDL 2.0.7's SDL_keyboard.focus; the window ASL's GetForegroundWindow answers with, which the game's input frame compares with its own before it reads the controllers
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
// a map started with a mod loaded and sv_cheats (aae_cheats.c): the map and devmap command (at 0x140f243) does
// "if (a mod is loaded) Dvar_SetBool(sv_cheats, 1)" just before it spawns the server, for both commands alike
#define ADDR_Dvar_SetBool 0xfa2d49 // func - void (dvar_t *dvar, bool value)
#define ADDR_Map_f_SetCheatsReturn 0x140f46b // where that Dvar_SetBool(sv_cheats, 1) (the call at 0x140f466) returns to
#define ADDR_cmd_args 0x125dc3c0 // the console command's arguments: int nesting at 0, int argc[8] at 0x64, char **argv[8] at 0x88
#define ADDR_Mods_LoadedName 0x30878dc // char[0x20]: the loaded mod's folder name ("" without one), "ModLoad start: %s"
#define ADDR_bg_weaponVariantDefs 0x14ce0a70 // WeaponVariantDef *[], by weapon index (BG_GetWeaponDef at 0x12362cd)
#define ADDR_bg_lastWeaponIndex 0x14ce0a68 // int: the number of weapons less one (BG_GetNumWeapons at 0x12362a8)
#define gentity_Size 0x4f8
#define gentity_Client 0x250 // gclient_t *
#define gentity_Health 0x2c8 // int, which CheatsOk wants above 0
#define gclient_Weapon 0x2c0 // Weapon: the one in hand (take clears it when it takes that one)
#define gclient_HeldWeapons 0x378 // the held weapons, each starting with its Weapon (give's ammo refill walks them)
#define gclient_HeldWeapon_Size 0x30
#define gclient_HeldWeapon_Count 15
#define WeaponVariantDef_WeaponDef 0x18 // WeaponDef *; the variant's name is its first field
#define WeaponVariantDef_ScriptName 0x8 // const char *: the name the server's weapon lookup has it by (ray_gun_upgraded for ray_gun_upgraded_zm), the second table BG_RegisterWeapon at 0x123650e fills
#define WeaponDef_InventoryType 0x7c // int: 0 primary, 1 offhand, 3 alt-fire mode (which give refuses), 4 melee
// a held weapon's weapon options (uint64_t, camo in the low byte) and attachment cosmetic variants (uint32_t), what the
// GetWeaponOptions (0xdaf9e6) and GetWeaponAcvi (0xda40b6) builtins return
#define gclient_HeldWeapon_Options 0x8
#define gclient_HeldWeapon_Acvi 0x18
#define gclient_WeaponSwitch 0x16b50 // uint64_t: cleared with the one in hand when take takes that one (Cmd_Take_f)
// the full loadout (zm_inventory.c, zm_loadout.c): what the GiveWeapon, TakeWeapon, SwitchToWeapon and HasPerk builtins
// call, and what the script VM's callbacks (Scr_PlayerConnect at 0x11901c3) use to run a script function on a player
#define ADDR_G_GivePlayerWeapon 0x12d2216 // func - int (gentity_t *, gclient_t *, Weapon, uint64_t options, int 0, int acvi), GiveWeapon's - name is a guess
#define ADDR_G_GiveStartAmmo 0xdb767d // func - void (gentity_t *, Weapon, int held_before), GiveWeapon's after giving, and GiveStartAmmo's - name is a guess
#define ADDR_G_TakePlayerWeapon 0x12d21e5 // func - int (gentity_t *, gclient_t *, Weapon, bool), Cmd_Take_f's - name is a guess
#define ADDR_G_SelectWeapon 0x12d2a8f // func - void (int clientNum, Weapon), tells the client to select it (0: none) - name is a guess
#define ADDR_G_SwitchToWeapon 0xdb7a16 // func - void (gentity_t *, int immediate, Weapon), SwitchToWeapon's - name is a guess
#define ADDR_BG_GetWeaponForName 0x1237bcf // func - Weapon (const char *), "name+attachment+..." the way the server's GetWeapon has it
#define ADDR_BG_GetPerkIndexForName 0x14d7be9 // func - int (const char *), 0x69 for none
#define ADDR_BG_HasPerk 0x14d7d44 // func - bool (gclient_t *, int perk index)
#define ADDR_bg_attachmentNames 0x20fc4b0 // const char *[44]: the attachment ids in a Weapon's 6-bit fields, 0 "none"
#define ADDR_SV_Cmd_ArgvBuffer 0xf75cfb // func - void (int arg, char *buffer, int size), the client command's arguments
#define ADDR_Scr_GetFunctionHandle 0x1492c98 // func - handle *(handle *out, inst, const char *script, uint32_t namespace, uint32_t name, int *checksum, bool required): a 0x110-byte handle, "%s::%s" then the code position at +0x108, NULL there if not found - name is a guess
#define ADDR_Scr_ExecEntThread 0x148d9e7 // func - unsigned int (gentity_t *, int params, handle passed by value), runs a server script function with the entity as self
#define ADDR_Scr_FreeThread 0x11a18f9 // func - void (inst, unsigned int thread)
#define ADDR_Scr_AddString 0x11a339d // func - void (inst, const char *)
#define ADDR_Scr_AddWeapon 0x11a32fa // func - void (inst, Weapon, int clientNum), a weapon object (class 5) - name is a guess
#define ADDR_scrClientNum 0x208f1c8 // int: the clientNum GetWeapon and the callbacks pass along - name is a guess
#define ADDR_scrVarGlob 0x1599be00 // 0x100 bytes per script instance: uint32_t *hash buckets at +0, the variables (0x40 bytes each) at +0x80 (FindVariable at 0x12f46c2) - name is a guess
#define ADDR_scrClassMap 0x20fc330 // a class table pointer per script instance, 0x20 bytes per class: the class's object at +4, per client at +0x18 (Scr_GetEntityId at 0x12f676b) - name is a guess
#define ADDR_scrDefaultFunction 0x21011a8 // const char *: the do-nothing code Scr_GetFunctionHandle gives a server script that lacks the function - name is a guess
#define ADDR_scrStringTable 0x150e6048 // char *: 0x1c bytes per string id, its text at +4 (SL_ConvertToString at 0x11b6940) - name is a guess
#define ADDR_BG_GetAmmoInClip 0xf5326a // func - int (playerState_t *, Weapon): GetWeaponAmmoClip's; the clip weapon's held slot, its clip at ps+0x684 + slot*4
#define ADDR_BG_GetAmmoStock 0xf532a8 // func - int (playerState_t *, Weapon): GetWeaponAmmoStock's for a weapon that isn't clip-only; ps+0x648 + slot*4
#define ADDR_BG_SetAmmoInClip 0xf52bf9 // func - void (playerState_t *, Weapon, int): SetWeaponAmmoClip's, after it clamps to the clip size
#define ADDR_BG_SetAmmoStock 0xf52cdf // func - void (playerState_t *, Weapon, int): SetWeaponAmmoStock's, after it clamps to BG_GetMaxAmmo
#define ADDR_BG_GetClipSize 0xf52ed4 // func - int (Weapon)
#define ADDR_BG_WeaponIsClipOnly 0xf52ec2 // func - int (Weapon): SetWeaponAmmoStock sets the clip of one
#define ADDR_BG_GetMaxAmmo 0xf52d88 // func - int (playerState_t *, Weapon, uint64_t): SetWeaponAmmoStock's clamp, its third argument read from ADDR_bg_maxAmmoArg
#define ADDR_bg_maxAmmoArg 0x189cb10 // uint64_t SetWeaponAmmoStock passes BG_GetMaxAmmo - name is a guess
#define ADDR_sv_running 0x118df7f0 // dvar_t *: sv_running (hash 0xefd7ca2e; Dvar_RegisterBool at 0xe77008, default off), on while this Mac runs the game's server (solo or host)
#define dvar_Type 0x1c // uint32_t in dvar_t: 1 for a bool (Dvar_GetInt at 0xfa0252)
#define dvar_Value 0x28 // the current value: a bool's is a byte
#define dvar_Latched 0x40 // the latched value: values are 24 bytes (Dvar_HasLatchedValue at 0x100f9fdfa compares +0x28 with +0x40, Dvar_MakeLatchedValueCurrent at 0x100fa0bef sets +0x40)
// whether the player has control (the gun in hand after a restore made during a map's intro, zm_inventory.c)
#define gentity_TagInfo 0x3e8 // pointer, not NULL while linked to another entity (G_EntUnlink at 0x10fdb07 returns without one; Unlink 0x13eaf65)
#define gentity_LinkFlags 0x2a9 // uint8_t: IsLinkedTo (0x1011845f2) wants 0x10 before it looks further (logged only)
#define gclient_Flags 0x17004 // uint32_t: 0x4 FreezeControls (0xdaeacc), 0x10 FreezeControlsAllowLook (0xdaeb48); AreControlsFrozen (0xdaebc4) tests 0x4
#define gclient_Frozen 0x14
#define gclient_WeaponFlags 0x18 // uint8_t in the player state (gclient's first field): 0x40 DisableWeapons (0xdaed12)
#define gclient_WeaponsDisabled 0x40
// the hero weapon's charge: the gadget slots (3 x 0x20 bytes) in the player state, what GadgetPowerGet (0x13ee2df) reads
// and GadgetPowerSet (0x13ee20d) sets; the stock _zm_hero_weapon.gsc keeps its hero weapon in slot 0
#define gclient_Gadgets 0x88c
#define gclient_Gadget0Power 0x8a4 // float, slot 0's power (0 to the gadget's most, 100)
#define ADDR_Gadget_SlotWeapon 0x1107f36 // func - Weapon (gclient_t *, int slot), GadgetPowerSet's - name is a guess
#define ADDR_G_GadgetPowerSet 0xe2b079 // func - void (gclient_t *, gadget *, Weapon, int time, float power): clamps to the gadget's most, GadgetPowerSet's - name is a guess
#define ADDR_level_Time 0x10975324 // int, the time GadgetPowerSet passes (level_locals + 0x64e4) - name is a guess
#define scrVarPub_LevelId 0x2c // uint32_t in scrVarPub (ADDR_scrVarPub, 0x78 bytes per instance): the level object's id, as GetLevel's opcode handler (0x12768de) and LevelEvalFieldVariable's (0x1277291) read it (+0x24, which handler 0x127685c pushes, is another object: live, it had no pap_lvup)
// the shield's health: the engine keeps it per client in the server's BG state, the bgs_t its server code reaches
// through a thread-local pointer (set to level_bgs at 0xdc20b2), clamped to 0..the shield's start hit points.
// DamageRiotShield (0xdb15db -> 0x13d42bc) lowers it (0x1387fe2); the player's "weaponhealth" field (setter 0x10e7937)
// sets it through G_SetShieldHealth; giving the shield puts it at full (0x13d4290, from 0x12d2351)
#define ADDR_level_bgs 0x10992850 // bgs_t, the server's - name is a guess
#define bgs_ShieldHealth 0x920 // int: client 0's shield health; clients are bgs_Client_Size apart (0x1387ecf, 0x1387f45)
#define bgs_Client_Size 0xed0
#define gclient_ClientNum 0x0 // int, the player state's first field: the client number the shield functions are passed
#define WeaponDef_Type 0x6c // int: 8 for a shield, what the engine looks for among the held weapons (0x13d4308)
#define WeaponType_Shield 8
#define WeaponDef_StartHitPoints 0x864 // int: a shield's full health (weaponstarthitpoints), the clamp's top
#define ADDR_G_SetShieldHealth 0x13d4567 // func - void (gentity_t *, int health): the first held shield's health, clamped, then its damage model (0x13d4393) - name is a guess
#define ADDR_BG_GetShieldHealth 0x1387ecf // func - int (int, int client_num, Weapon): through the thread-local bgs; the start hit points without one - name is a guess
#define ADDR_BG_ThreadBgs 0x12ddff8 // func - bgs_t **(void): this thread's bgs pointer (thread-local) - name is a guess
// -- end zm inventory
// -- client effects (aae_clientfx.c): the "BG cache" of numbered assets, matched with Windows (wip/night/impl-clientfx.md)
#define ADDR_bgCache 0xa974930 // two copies of 0x45fe00 bytes, the server's then the client's; client effects at +0x43c00, 1024 entries of 16 bytes (Windows 0x1436051f0)
#define ADDR_BG_Cache_Init 0x12a1ae0 // func - void (int side): zeroes that copy, then registers bgCachePrintValues (Windows 0x1400a8310)
#define ADDR_BG_Cache_GetIndexInternal 0x12a7048 // func - int (int side, int kind, entry *table, int count, const char *name, bool free_entry): index, 0 for none; "Exceeded" when full (Windows 0x1400a4860)
#define ADDR_BG_Cache_RegisterDef 0x12a7d0d // func - int (int side, int kind, entry *table, int count, const char *name, const void *asset, bool reference): index, 0 for none (Windows 0x1400a5d60; T7Overcharged's name)
#define ADDR_BG_Cache_Release 0x12a7237 // func - void (int side, int kind, entry *table, int count, const char *name): a reference less, free at 0 (inlined in Windows 0x1400a4550) - name is a guess
#define ADDR_BG_Cache_FindAsset 0x12a29ea // func - bool (int asset type, const char *name, const void **asset): DB_FindXAssetHeader, "Couldn't find asset '%s'" when missing - name is a guess
#define ADDR_BG_Cache_ReleaseClientFx 0x12a75a1 // func - (int side, const char *name), the type table's release for client_fx (Windows 0x1400a4550, T7Overcharged's clientfx function 1)
#define ADDR_BG_Cache_GetClientFx 0x12a7474 // func - const void *(int side, int index) (Windows 0x1400a7710, function 2)
#define ADDR_BG_Cache_GetClientFxIndex 0x12a74d5 // func - int (int side, const char *name) (Windows 0x1400a7730, function 3)
#define ADDR_BG_Cache_GetClientFxName 0x12a749c // func - const char *(int side, int index) (Windows 0x1400a7770, function 4)
#define ADDR_BG_Cache_RegisterClientFx 0x12a750a // func - void (int side, const char *name, bool reference); the type table's register is 0x12a7592, which calls it with 1 (Windows 0x1400a9f20, function 5)
#define ADDR_Com_Printf 0xe71396 // func - void (int kind, int channel, const char *format, ...): the cache prints with kind 7, 8 or 9 and channel 0x17 (Windows 0x1420f04a0; T7Overcharged's name)
// -- T7O crash guards (t7o_guards.c)
// "mov ecx, [rax + 4*rcx + 0x3c0]" in the function that works out one of a weapon's fields for CG_RegisterWeapon
// (0x1381200: weapon, its variant definition, field, the weapon's mode attachment; Windows 0x1426752f0): the override
// bits of the mode attachment's entry, rax, loaded at 0x1381298 from the definition's table at +0x40 without a null
// test, for fields 0x68..0x6c and 0xad..0xaf; rcx is field >> 5, r12 the field, rbx the attachment's number, r15 the
// weapon. Behind it bt/jb; 0x13812b3, where the game goes on when the attachment doesn't override the field, is where
// T7Overcharged's bg_weapon_attachment_fix sends it when rax is null (its BG_WeaponAttachment_CrashRip/_CrashFix,
// Windows 0x1426753a3 / 0x1426753b3)
#define ADDR_BG_WeaponAttachment_Overrides 0x13812a2
// func - int (int localClientNum): the controller of a local client from the 2-entry table at 0x119c1370 (0x24 bytes
// each: +4 the client's number, +8 its controller), -1 when that entry's number isn't localClientNum; no bounds test
// (Windows 0x1420e31b0, which T7Overcharged's local_client_bounds_fix hooks)
#define ADDR_Com_LocalClient_GetControllerIndex 0xeae63e
// -- end T7O crash guards
// All-around Enhancement 4's pause for everybody in a co-op game (aae_coop_pause.c)
#define ADDR_SV_CheckPaused 0x11d5ba4 // func - int (void): with cl_paused set and one client, sets sv_paused and gives 1; with more clients puts cl_paused and sv_paused back to 0; 0 not paused. Called by SV_Frame and the game's frame (Windows 0x1421f7080)
#define ADDR_SV_Frame 0x11d64a6 // func - int (int msec): the server's frame, gives msec back; runs the server through 0x11d652d with the same msec unless SV_CheckPaused says paused (Windows 0x1421f76e0)
#define ADDR_Com_Frame_SV_FrameReturn 0xe75a3e // where the game's frame's call of SV_Frame with the frame's msec returns to
#define ADDR_Com_Frame_SV_FrameReturn2 0xe75a54 // and its call with -1 (one call on Windows; 0xe1c63a's call with 0 is another caller)
#define ADDR_CL_Frame 0xe1a4c3 // func - void (int localClientNum, int msec, float frac in xmm0): the client's frame, called once a frame by the game's frame; hands msec to the add to the client's real time (0x110aa1d) (Windows 0x14135c910) - name is a guess
#define ADDR_CanPauseGame_Test 0x136b736 // "mov al, 1 ; cmp dword [rcx + 0x1790], 0 ; jne <pop rbp; ret>" at the start of Engine.CanPauseGame's function (0x136b72b, bool (void); rcx = 0x1113b7880 there, its +0x1790 a flag set only while the game renders one picture in several passes, 0 in play) (Windows 0x140843bd0)
#define ADDR_Cmd_TokenizeString 0xf76456 // func - void (const char *text) (Windows 0x1420e2690)
#define ADDR_CL_ServerCommand_TokenizeReturn 0xe2dfc7 // where CL_CGameNeedsServerCommand's call of it returns to (Windows: the call at 0x14131fd33)
#define ADDR_NET_GetPacket 0xd6dacf // func - int (netadr_t *from, msg_t *msg): msg's data (+8) is moved past the header byte, its size (+0x1c) is the packet's, +0x34 the socket it came for (Windows 0x1422b9040)
#define ADDR_Netchan_GetPacketReturn 0x14012ca // where the network thread's (0x1401052) call of it returns to (Windows: the call at 0x14211e083)
#define ADDR_Netchan_SendLocalClient 0x13ff83e // func - bool (int localClientNum): the network thread's sending for a local client, its one call at 0x1401469 (Windows 0x14211e580) - name is a guess
#define ADDR_NET_SendPacket 0xd6dc30 // func - bool (int sock, int length, const void *data, netadr_t to by value in rcx:r8); refuses a socket or a local net id above 4 (Windows 0x1422b9240)
#define ADDR_dvar_cl_paused 0x118df830 // dvar_t * - cl_paused (hash 0x762396d1, an int)
#define ADDR_dvar_sv_paused 0x118dfa78 // dvar_t * - sv_paused (hash 0x6d2b2caf, an int)
#define ADDR_dvar_sv_maxclients 0x118df620 // dvar_t * - how many clients the server has room for (hash 0x6373a725, an int 1..18; the game's loops over its client table go that far) - name is a guess
// the server's clients (ADDR_svs_clients): the netadr at +0x2c, the 8-byte id at +0x55c8, the bot flag at +0xbb460
// (Windows +0xbb360)
// local client 0's connection state is the int at ADDR_CLIENT_CONNECTIONS + 8 (0x1078 bytes a local client); its
// connection data at *ADDR_CLIENT_GLOBALS (0x25780 bytes each): the server's netadr at +0x2455c, its 8-byte id at
// +0x24b68
// -- end coop pause
// All-around Enhancement's widgets on the first person weapon, the plugin's viewmodel_ui_anchor (aae_vmwidget.c)
#define ADDR_LUI_Startup 0xebeea9 // func - void (callback, callback), "LUI: Starting up...": makes the UI's Lua VM and registers the UI element methods of the list at ADDR_LUI_ElementMethods in it (Windows 0x14268c3d0, inlined here) - name is a guess
#define ADDR_LUI_ElementMethods 0x119c6b80 // {name, lua_CFunction, next} *: the list of the UI element methods, built by static initializers (Windows 0x159bf7e00) - name is a guess
#define ADDR_LUI_Generic3DMethod 0xb2c9278 // its entry of "setupGeneric3DWidget" (Windows 0x1435de138)
#define ADDR_Lua_SetupGeneric3DWidget 0x1300188 // func - int (lua_State *), element:setupGeneric3DWidget(x, y, z): the point to +0x78, ADDR_LUI_Generic3DUpdate to +0x110 (Windows 0x14279df10)
#define ADDR_LUI_Generic3DUpdate 0x131040f // func - void (int localClientNum, element *, root element *, int, int, lua_State *): an element's update (+0x110) that projects its point (+0x78), places it and runs its usual update (Windows 0x142771a00) - name is a guess
#define ADDR_LUI_ToElement 0xeb9f00 // func - element * (lua_State *, int index), NULL when that argument isn't a UI element (Windows 0x142694d40) - name is a guess
#define ADDR_CG_WorldToScreen 0x12fc923 // func - (int localClientNum, const float world[3], float screen[2]), the 3D widget's projection (Windows 0x1427a01b0) - name is a guess
#define ADDR_hks_obj_tolstring 0x146d28b // func - const char * (lua_State *, HksObject *, size_t *length): a number becomes its text in place, NULL for anything else but a string (Windows 0x141d3ec30)
#define ADDR_hksi_lua_createtable 0x14467c8 // func - void (lua_State *, int narr, int nrec), pushes a new table (Windows 0x141d46bf0)
#define ADDR_hksi_luaL_ref 0x1445261 // func - int (lua_State *, int table): pops the value, its reference or -1 for nil; 0x101474880 jumps to it (Windows 0x141d40710)
// -- client fields (clientfield_guard.c). The lookup, and the places that read through what it returns without
// checking it, each given as the instruction right after its call (where the lookup returns to). Windows places are
// those of the build T7Overcharged's names fit (BlackOps3.exe CheckSum 0x6531394); see wip/night/impl-clientfield.md
#define ADDR_ClientField_Find 0x13b95ed // func - ClientField *(int set, scr_string_t name, bool error_if_missing): the field of that name in the set (the thread's bgs + 8: 0x4018 bytes a set, count at +8, fields from +0x10; a field's name at +0x120, type byte at +0x12d), else NULL, after Com_Error(2, "No ClientField named %s found in set %s") when error_if_missing and com_clientFieldsDebug (0x1118df788, hash 0x92ac62) is on. Windows 0x140133ad0
#define ADDR_ClientField_SetNames 0x1b7a850 // const char *[14]: the sets' names in that message, "world" (0) .. "item" (13); 7 is "clientuimodel"
#define ADDR_Scr_Error 0x119f219 // func - void (scriptInstance_t inst, const char *message, bool terminal): keeps the message (unless one waits already) and throws it through Scr_ErrorInternal, which comes back only with developer_script on. Windows 0x1412ea450
#define ADDR_SL_ConvertToString 0x11b6940 // func - const char *(scr_string_t). Windows 0x1412d7180
#define ADDR_GScr_CodeGetClientField_Read 0x1293ef5 // in codegetclientfield (server) 0x1293e7d; Windows 0x141a5f94b, T7O ScriptError_GscClientFieldFault1
#define ADDR_GScr_CodeGetPlayerStateClientField_Read 0x1294286 // in 0x129421a; Windows 0x141a5fa5e, T7O ...Fault2
#define ADDR_GScr_CodeGetUIModelClientField_Read 0x1294592 // in 0x1294526; Windows 0x141a5fb5e, T7O ...Fault3 (DEIMOS 935's crash)
#define ADDR_GScr_CodeGetWorldClientField_Read 0x12947e3 // in 0x12947c4; Windows 0x141a5fbfd, T7O ...Fault4
#define ADDR_GScr_CodeIncrementPlayerStateClientField_Read 0x129466a // in 0x12945fb; Windows 0x141a5fe76, T7O ...Fault5
#define ADDR_GScr_CodeIncrementUIModelClientField_Read 0x129435e // in 0x12942ef; Windows 0x141a5ff86, T7O ...Fault6
#define ADDR_GScr_CodeIncrementWorldClientField_Read 0x1294877 // in 0x1294858; Windows 0x141a6003d, T7O ...Fault7
#define ADDR_GScr_CodeSetPlayerStateClientField_Read 0x1294168 // in 0x12940f9; Windows 0x141a602c7, T7O ...Fault8
#define ADDR_GScr_CodeSetWorldClientField_Read 0x1294730 // in 0x1294710; Windows 0x141a604c4, T7O NonExistentClientfield_CrashRip
#define ADDR_CScr_CodeGetClientField_Read 0xef0719 // in codegetclientfield (client) 0xef05e2; Windows 0x140c15954, no T7O name
#define ADDR_CScr_CodeGetPlayerStateClientField_Read 0xef07e9 // in 0xef07a6; Windows 0x140c15b80, T7O ClientFieldFault_Csc1
#define ADDR_CScr_CodeGetUIModelClientField_Read 0xef0896 // in 0xef0853; Windows 0x140c15c50, T7O ClientFieldFault_Csc2
#define ADDR_CScr_CodeGetWorldClientField_Read 0xef094f // in 0xef0900; Windows 0x140c18cf5, T7O ClientFieldFault_Csc3
#define ADDR_ClientField_GetInt_Read 0x13b9d75 // in the int value getter 0x13b9d62 (set, name, values); Windows 0x140133f31 in 0x140133f20, T7O ClientFieldFault_MissingRip
#define ADDR_ClientField_GetFloat_Read 0x13b9e24 // in the float value getter 0x13b9dfd; Windows 0x140133ec1 in 0x140133ea0, T7O ClientFieldFault_NullRip1
#define ADDR_ClientField_GetFloat_ReadBits 0x13b9e4c // its second lookup, for the bits; Windows 0x140133eeb, T7O ClientFieldFault_NullRip2
// -- end client fields
// -- END OFFSETS
