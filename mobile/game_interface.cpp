/*=============================================================================
	game_interface.cpp: Phase 1 stubs for the OpenTouch touch-UI <-> engine
	contract (Clibs_OpenTouch/game_interface.h).

	Phase 1 milestone is only "builds, launches, boots to the menu", so the
	only function that does real work is PortableInit(), which starts the
	engine by calling its main(). Everything else is an empty stub and
	PortableGetScreenMode() reports TS_GAME. Real touch input, menu detection
	and the per-tick action pump are Phase 2 (see the sibling UE1 module's
	fully-worked implementation).

	Note: isPlayerRunning / MouseButton / MouseMove / MouseMoveAbsolute are
	defined by Clibs_OpenTouch/android_jni_inc.cpp, not here.
=============================================================================*/

// SDL first: on Android SDL_main.h remaps main -> SDL_main, and Launch.cpp
// (the engine's real entry) is compiled with the same remap, so the extern
// declaration and the call below resolve to the same symbol.
#include "SDL2/SDL.h"
#include "game_interface.h"

#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <android/log.h>

// The engine's entry point (UnrealTournament/Src/Launch.cpp). Same main->SDL_main
// remap applies here because SDL2/SDL.h is included above.
extern int main(int argc, char **argv);

// Pump the read end of the stdout/stderr pipe into logcat, line by line.
static void *ut99_stdio_logger(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t pos = 0;
    ssize_t len;
    while ((len = read(fd, buf + pos, sizeof(buf) - 1 - pos)) > 0)
    {
        pos += (size_t)len;
        buf[pos] = 0;
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n')) != NULL)
        {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "UT99", line);
            line = nl + 1;
        }
        pos = strlen(line);
        memmove(buf, line, pos + 1);
    }
    return NULL;
}

// The engine reports critical errors (e.g. FailedBrowse, and the full guard
// history) through FOutputDeviceAnsiError, which prints to stdout/stderr - which
// go nowhere on Android. Redirect both to logcat so those are visible. Normal
// debugf output is separately mirrored by FOutputDeviceFile's own LOGI hook.
static void ut99_redirect_stdio_to_logcat()
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int pfd[2];
    if (pipe(pfd) != 0)
        return;
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    pthread_t t;
    if (pthread_create(&t, NULL, ut99_stdio_logger, (void *)(intptr_t)pfd[0]) == 0)
        pthread_detach(t);
}

void PortableInit(int argc, const char **argv)
{
    ut99_redirect_stdio_to_logcat();
    LOGI("PortableInit");
    // Runs the engine on this (the engine's own) thread; never returns until
    // the game exits.
    main(argc, (char **)argv);
}

void PortableBackButton(void) {}

int PortableKeyEvent(int state, int code, int unitcode) { return 0; }

void PortableAction(int state, int action) {}

void PortableMove(float fwd, float strafe) {}

void PortableMoveFwd(float fwd) {}

void PortableMoveSide(float strafe) {}

void PortableLookPitch(int mode, float pitch) {}

void PortableLookYaw(int mode, float yaw) {}

void PortableMouse(float dx, float dy) {}

void PortableMouseAbs(float x, float y) {}

void PortableMouseButton(int state, int button, float dx, float dy) {}

void PortableCommand(const char *cmd) {}

void PortableAutomapControl(float zoom, float x, float y) {}

int PortableShowKeyboard(void) { return 0; }

bool PortableSetAlwaysRun(bool run) { return run; }

touchscreemode_t PortableGetScreenMode() { return TS_GAME; }

void PortableSetMouseTapMode(int enable) {}

int PortableGetMouseTapMode(void) { return 0; }
