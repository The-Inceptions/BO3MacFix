#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <Foundation/Foundation.h>
#include "../source/start_mod.h"

// BO3MacFix_PrepatchedIsCurrent and BO3MacFix_BuildPrepatched, see source/prepatch.h
typedef int (*prepatch_fn)(const char *original, const char *copy, char *message, size_t message_size);

// reads "Name = value" from BO3MacFix's settings.ini, like read_int_setting in bo3macfix.c
static int read_setting(NSString *path, const char *name, int fallback) {
    FILE *fp = fopen(path.fileSystemRepresentation, "r");
    if (fp == NULL)
        return fallback;
    int value = fallback;
    size_t name_len = strlen(name);
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line + strspn(line, " \t");
        if (strncmp(p, name, name_len) != 0)
            continue;
        p += name_len;
        p += strspn(p, " \t");
        if (*p != '=')
            continue;
        p++;
        p += strspn(p, " \t\"");
        // "Name =" with nothing (or no number) after it keeps the default, like the dylib's parser
        char *end;
        long parsed = strtol(p, &end, 10);
        if (end != p)
            value = (int)parsed;
    }
    fclose(fp);
    return value;
}

// runs /usr/bin/codesign with these arguments, returns whether it succeeded
static BOOL codesign(NSArray<NSString *> *arguments) {
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/codesign"];
    task.arguments = arguments;
    task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![task launchAndReturnError:nil])
        return NO;
    [task waitUntilExit];
    return task.terminationStatus == 0;
}

// removes what a launcher that was stopped while making the copy left behind (CoDBlkOps3_Exe.bo3macfix.tmp<pid>, and
// codesign's .cstemp of it), 37 MB each. Another launcher that's making one right now keeps its own.
static void remove_stale_temp_files(NSURL *gameDir, FILE *log) {
    NSString *prefix = @"CoDBlkOps3_Exe.bo3macfix.tmp";
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:gameDir.path error:nil]) {
        if (![name hasPrefix:prefix])
            continue;
        pid_t pid = (pid_t)strtol([name substringFromIndex:prefix.length].UTF8String, NULL, 10);
        if (pid <= 0 || pid == getpid() || (kill(pid, 0) != 0 && errno == ESRCH)) {
            fprintf(log, "removing %s, left over from an earlier launch\n", name.UTF8String);
            unlink([gameDir URLByAppendingPathComponent:name].path.fileSystemRepresentation);
        }
    }
}

// makes sure the file is on disk before it's renamed into place, so a crash or power cut can't leave a copy that's
// up to date by its header but not all there
static BOOL flush_to_disk(NSString *path) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0)
        return NO;
    BOOL flushed = fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
    close(fd);
    return flushed;
}

// The pre-patched executable is a copy of the game's with BO3MacFix's code changes made in the file, so they don't
// have to be made in memory, which makes Rosetta run the whole game through its slower JIT. Returns the copy,
// making it first when it's missing or out of date (after a game or BO3MacFix update), or nil to start the original.
static NSURL *prepatched_executable(NSURL *gameDir, NSURL *dylibUrl, FILE *log) {
    NSString *original = [gameDir URLByAppendingPathComponent:@"CoDBlkOps3_Exe"].path;
    NSString *copy = [gameDir URLByAppendingPathComponent:@"CoDBlkOps3_Exe.bo3macfix"].path;
    NSString *temp = [copy stringByAppendingFormat:@".tmp%d", (int)getpid()];
    char message[512];
    remove_stale_temp_files(gameDir, log);

    // BO3MacFix makes the copy, it knows what to change
    void *macfix = dlopen(dylibUrl.path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
    prepatch_fn is_current = macfix != NULL ? (prepatch_fn)dlsym(macfix, "BO3MacFix_PrepatchedIsCurrent") : NULL;
    prepatch_fn build = macfix != NULL ? (prepatch_fn)dlsym(macfix, "BO3MacFix_BuildPrepatched") : NULL;
    if (is_current == NULL || build == NULL) {
        fprintf(log, "couldn't load BO3MacFix to make the pre-patched executable: %s\n", macfix == NULL ? dlerror() : "it's too old");
        return nil;
    }
    if (is_current(original.fileSystemRepresentation, copy.fileSystemRepresentation, message, sizeof(message)) == 0) {
        fprintf(log, "%s\n", message);
        return [NSURL fileURLWithPath:copy];
    }
    fprintf(log, "%s, making it\n", message);
    unlink(temp.fileSystemRepresentation);
    if (build(original.fileSystemRepresentation, temp.fileSystemRepresentation, message, sizeof(message)) != 0) {
        fprintf(log, "couldn't make the pre-patched executable: %s\n", message);
        unlink(temp.fileSystemRepresentation);
        return nil;
    }
    fprintf(log, "%s\n", message);
    // changing the executable broke Aspyr's signature, so sign the copy ad hoc under the same identifier
    if (!codesign(@[ @"--force", @"--sign", @"-", @"--identifier", @"CoDBlkOps3_Exe", temp ]) ||
        !codesign(@[ @"--verify", temp ])) {
        fprintf(log, "couldn't sign the pre-patched executable\n");
        unlink(temp.fileSystemRepresentation);
        return nil;
    }
    if (!flush_to_disk(temp)) {
        fprintf(log, "couldn't write the pre-patched executable to disk\n");
        unlink(temp.fileSystemRepresentation);
        return nil;
    }
    if (rename(temp.fileSystemRepresentation, copy.fileSystemRepresentation) != 0) {
        fprintf(log, "couldn't put the pre-patched executable in place\n");
        unlink(temp.fileSystemRepresentation);
        return nil;
    }
    return [NSURL fileURLWithPath:copy];
}

// StartWithMod (source/start_mod.c): the mod that was loaded when the game was last quit, which BO3MacFix notes in
// BO3MacFix/lastmod.txt, goes on the game's command line as "+set fs_game <mod>", as the test runner has always
// started the game: it then loads the mod before its first menu instead of the player loading it from MODS. The game
// is told what was done (BO3MACFIX_STARTWITHMOD_LAUNCH), for its log and for a notice when the mod was left out for a
// reason the player has to know. Not with NoCodePatches, under which the library does none of its part. A dry run
// writes only into a folder of its own (BO3MACFIX_STARTWITHMOD_DIR).
static start_mod_plan_t start_with_mod(NSURL *gameDir, NSURL *macfixUrl, NSString *settingsPath, BOOL noCodePatches,
                                       BOOL dryRun, int argc, char **argv, NSMutableArray *arguments,
                                       NSMutableDictionary *environment, FILE *log, start_mod_paths_t *paths) {
    static char folder[PATH_MAX], game[PATH_MAX], workshop[PATH_MAX];
    const char *env = getenv("BO3MACFIX_STARTWITHMOD"), *ownFolder = getenv("BO3MACFIX_STARTWITHMOD_DIR");
    BOOL hasOwnFolder = ownFolder != NULL && ownFolder[0] != '\0';
    int enabled = env != NULL ? atoi(env) : read_setting(settingsPath, "StartWithMod", 1);
    strlcpy(folder, hasOwnFolder ? ownFolder : macfixUrl.URLByStandardizingPath.path.fileSystemRepresentation,
            sizeof(folder));
    strlcpy(game, [gameDir URLByAppendingPathComponent:@"../../.."].URLByStandardizingPath.path
                      .fileSystemRepresentation, sizeof(game));
    strlcpy(workshop, [gameDir URLByAppendingPathComponent:@"../../../../../workshop"].URLByStandardizingPath.path
                          .fileSystemRepresentation, sizeof(workshop));
    paths->folder = folder;
    paths->game = game;
    paths->workshop = workshop;
    char mod[Start_Mod_Name_Size], note[64];
    BOOL write = !dryRun || hasOwnFolder;
    start_mod_plan_t plan = start_mod_launch(paths, enabled != 0 && !noCodePatches, argc, argv, write, mod);
    if (plan == Start_Mod_Given)
        [arguments addObjectsFromArray:@[ @"+set", @"fs_game", @(mod) ]];
    start_mod_note(plan, mod, note, sizeof(note));
    environment[@Start_Mod_Launch_Env] = @(note);
    fprintf(log, "StartWithMod: %s%s%s (lastmod.txt in %s%s)\n", mod, mod[0] != '\0' ? ": " : "",
            enabled != 0 && noCodePatches ? "off, since NoCodePatches is on" : start_mod_plan_text(plan), folder,
            write ? "" : "; a dry run, so nothing written");
    return plan;
}

int main(int argc, char **argv) {
    NSTask *gameProc = [[NSTask alloc] init];
    // copy our current working directory and pass it onto the child process
    NSProcessInfo *currentProcess = [NSProcessInfo processInfo];
    NSString *pwd = [currentProcess arguments][0];
    // a dry run for tests (BO3MACFIX_LAUNCHER_DRYRUN=<the game's MacOS folder>): this launcher acts as if it were in
    // that folder, writes nothing there (no launcher.log, no pre-patched copy; StartWithMod only into
    // BO3MACFIX_STARTWITHMOD_DIR), prints what it would start and exits without starting it
    const char *dryRunFolder = getenv("BO3MACFIX_LAUNCHER_DRYRUN");
    BOOL dryRun = dryRunFolder != NULL && dryRunFolder[0] != '\0';
    if (dryRun)
        gameProc.currentDirectoryURL = [NSURL fileURLWithPath:@(dryRunFolder) isDirectory:YES];
    else
        gameProc.currentDirectoryURL = [[NSURL fileURLWithPath:pwd] URLByDeletingLastPathComponent];
    // set the game executable path
    gameProc.executableURL = [gameProc.currentDirectoryURL URLByAppendingPathComponent:@"CoDBlkOps3_Exe"];

    // add the fix dylib to DYLD_INSERT_LIBRARIES of the new process
    NSURL *dylibUrl = [gameProc.currentDirectoryURL URLByAppendingPathComponent:@"../../../BO3MacFix/BO3MacFix.dylib"];
    NSMutableDictionary *newEnvironment = [[currentProcess environment] mutableCopy];
    // if we already have dyld_insert_libraries set (by Steam), append our dylib to that
    if ([[currentProcess environment] objectForKey:@"DYLD_INSERT_LIBRARIES"]) {
        newEnvironment[@"DYLD_INSERT_LIBRARIES"] = [NSString stringWithFormat:@"%@:%@",[dylibUrl path],[currentProcess environment][@"DYLD_INSERT_LIBRARIES"]];
    } else { // otherwise just set it by itself
        newEnvironment[@"DYLD_INSERT_LIBRARIES"] = [dylibUrl path];
    }
    newEnvironment[@"WINEPREFIX"] = [[gameProc.currentDirectoryURL URLByAppendingPathComponent:@"../../../BO3MacFix/pfx"] path];
    newEnvironment[@"SteamAppId"] = @"311210";
    // parse our arguments and figure out which ones we send to the new process vs which ones we
    NSMutableArray *newArguments = [[NSMutableArray alloc] init];
    for (int i = 0; i < argc; i++) {
        if (strcasecmp(argv[i], "--password") == 0 && i + 1 < argc)
            newEnvironment[@"BO3MACFIX_NETWORKPASSWORD"] = [NSString stringWithUTF8String:argv[++i]];
        else if (strcasecmp(argv[i], "--playername") == 0 && i + 1 < argc)
            newEnvironment[@"BO3MACFIX_PLAYERNAME"] = [NSString stringWithUTF8String:argv[++i]];
        else if (strcasecmp(argv[i], "--winepath") == 0 && i + 1 < argc)
            newEnvironment[@"BO3MACFIX_WINEPATH"] = [NSString stringWithUTF8String:argv[++i]];
        else if (strcasecmp(argv[i], "--friendsonly") == 0)
            newEnvironment[@"BO3MACFIX_FRIENDSONLY"] = @"true";
        else
            [newArguments addObject:[NSString stringWithUTF8String:argv[i]]];
    }

    // What happened goes in BO3MacFix/launcher.log (a dry run prints it)
    NSURL *macfixUrl = [gameProc.currentDirectoryURL URLByAppendingPathComponent:@"../../../BO3MacFix"];
    NSString *settingsPath = [macfixUrl URLByAppendingPathComponent:@"settings.ini"].path;
    FILE *log = dryRun ? stdout
                       : fopen([macfixUrl URLByAppendingPathComponent:@"launcher.log"].path.fileSystemRepresentation, "w");
    if (log == NULL)
        log = fopen("/dev/null", "w");
    BOOL noCodePatches = getenv("BO3MACFIX_NOCODEPATCHES") != NULL ? atoi(getenv("BO3MACFIX_NOCODEPATCHES")) != 0
                                                                  : read_setting(settingsPath, "NoCodePatches", 0) != 0;
    start_mod_paths_t startModPaths;
    start_mod_plan_t startMod = start_with_mod(gameProc.currentDirectoryURL, macfixUrl, settingsPath, noCodePatches,
                                               dryRun, argc, argv, newArguments, newEnvironment, log, &startModPaths);
    gameProc.environment = newEnvironment;
    gameProc.arguments = newArguments;

    // start the pre-patched executable unless PrePatchedExe = 0 in settings.ini, or NoCodePatches is on (which
    // wants the game's code as shipped)
    const char *prepatched_env = getenv("BO3MACFIX_PREPATCHEDEXE");
    int prepatched = prepatched_env != NULL ? atoi(prepatched_env) : read_setting(settingsPath, "PrePatchedExe", 1);
    if (prepatched == 0) {
        fprintf(log, "PrePatchedExe is off\n");
    } else if (noCodePatches) {
        fprintf(log, "NoCodePatches is on, so the game's code stays as shipped\n");
    } else if (dryRun) {
        fprintf(log, "dry run: the pre-patched executable is neither checked nor made\n");
    } else {
        NSURL *copyUrl = prepatched_executable(gameProc.currentDirectoryURL, dylibUrl, log);
        if (copyUrl != nil)
            gameProc.executableURL = copyUrl;
    }
    if (dryRun) {
        printf("dry run: would start %s in %s\n", gameProc.executableURL.lastPathComponent.UTF8String,
               gameProc.currentDirectoryURL.path.fileSystemRepresentation);
        for (NSUInteger i = 0; i < newArguments.count; i++)
            printf("argument %lu: %s\n", (unsigned long)i + 1, [newArguments[i] UTF8String]);
        printf("environment: %s=%s\n", Start_Mod_Launch_Env, [newEnvironment[@Start_Mod_Launch_Env] UTF8String]);
        return 0;
    }
    fprintf(log, "starting %s\n", gameProc.executableURL.lastPathComponent.UTF8String);
    fflush(log);

    //NSLog(@"Launching game...\n");
    //NSLog(@"  Executable: %@\n", gameProc.executableURL);
    //NSLog(@"  Working Directory: %@\n", gameProc.currentDirectoryURL);
    //NSLog(@"  DYLD_INSERT_LIBRARIES: %@\n", newEnvironment[@"DYLD_INSERT_LIBRARIES"]);
    //NSLog(@"  WINEPREFIX: %@\n", newEnvironment[@"WINEPREFIX"]);
    // launch the game executable, or the original if the pre-patched copy won't start (it lost its execute permission,
    // say). An NSTask only runs once, so that takes a new one.
    NSError *error = nil;
    BOOL launched = [gameProc launchAndReturnError:&error];
    if (!launched && ![gameProc.executableURL.lastPathComponent isEqualToString:@"CoDBlkOps3_Exe"]) {
        fprintf(log, "couldn't start %s (%s), starting CoDBlkOps3_Exe\n", gameProc.executableURL.lastPathComponent.UTF8String,
                error.localizedDescription.UTF8String);
        NSTask *original = [[NSTask alloc] init];
        original.currentDirectoryURL = gameProc.currentDirectoryURL;
        original.executableURL = [gameProc.currentDirectoryURL URLByAppendingPathComponent:@"CoDBlkOps3_Exe"];
        original.environment = gameProc.environment;
        original.arguments = gameProc.arguments;
        gameProc = original;
        error = nil;
        launched = [gameProc launchAndReturnError:&error];
    }
    if (!launched) {
        fprintf(log, "couldn't start %s: %s\n", gameProc.executableURL.lastPathComponent.UTF8String,
                error.localizedDescription.UTF8String);
        // no game ran, so its marker mustn't make the next start look like one with the mod that failed
        if (startMod == Start_Mod_Given && !start_mod_launch_abandoned(&startModPaths))
            fprintf(log, "StartWithMod: couldn't remove lastmod.pending\n");
        fclose(log);
        return 1;
    }
    fclose(log);
    [gameProc waitUntilExit];
    return 0;
}
