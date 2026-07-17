/*=============================================================================
	game_interface.cpp: OpenTouch <-> UT99 bridge.

	Ported from the sibling UE1 module's game_interface.cpp - UT99 is a later
	UE1 generation with the same UInput/UViewport architecture, so the input
	mechanism is identical; only a few v400 details differ (Exec/Process take
	FOutputDevice& not *, output device is *GLog). The UT99-specific stdout/
	stderr->logcat pump and PortableInit stay from the Phase-1 stub.

	Menu navigation, the on-screen keyboard, the generic custom buttons and
	mouse-look are synthesized as real SDL key/mouse events (sendKey/MouseMove/
	MouseButton) - exactly what NSDLViewport would see from real hardware. That's
	correct and rebind-proof for those: UT's menu/console reads raw EInputKey
	codes directly (bypassing Bindings), and the custom buttons are *meant* to be
	rebindable.

	Movement/turn/fire/jump/weapons DO go through the player-editable
	Bindings[key]->Aliases[] table when driven by a real key, so to stay correct
	no matter what the player rebinds they call the engine's real action directly
	via UViewport::Exec() with the exact alias/raw command a bound key resolves to
	(e.g. "Fire", "Axis aBaseY Speed=+300.0"). Continuous movement/turn additionally
	need UE1's per-tick input pump (UInput::Process against a real EInputKey), so
	those go through reserved, otherwise-unreachable key slots (IK_UnknownXX; no
	real device can generate them) bound once to the raw axis command.

	Threading: the engine runs single-threaded on its own thread (PortableInit ->
	main() -> MainLoop(), never returns) while Portable* is called from the Android
	touch thread. sendKey/MouseButton/MouseMove queue SDL events and are thread-
	safe. Everything that calls Process()/Exec() directly runs ONLY from
	UT99_TickPortableActions() (called once per tick from MainLoop, Launch.cpp) on
	the engine thread; the Portable* setters only ever write volatile flags/floats
	or push onto a ring buffer.

	Note: isPlayerRunning / MouseButton / MouseMove / MouseMoveAbsolute are
	defined by Clibs_OpenTouch/android_jni_inc.cpp, not here.
=============================================================================*/

// SDL first: on Android SDL_main.h remaps main -> SDL_main, and Launch.cpp
// (the engine's real entry) is compiled with the same remap. The C system
// headers below (esp. <pthread.h> -> <time.h>) must precede Engine.h, whose
// Core UnFile.h defines a function-like clock() macro that mangles <time.h>'s
// clock() declaration.
#include "SDL2/SDL.h"

#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <android/log.h>

#include "Engine.h"
#include "game_interface.h"

// The engine's entry point (UnrealTournament/Src/Launch.cpp). Same main->SDL_main
// remap applies here because SDL2/SDL.h is included above.
extern int main(int argc, char **argv);

// SDL's internal keyboard injection: queues a real key event under SDL's lock,
// safe from the touch thread, consumed on the engine thread in
// NSDLViewport::TickInput().
extern "C" int SDL_SendKeyboardKey(Uint8 state, SDL_Scancode scancode);

// Defined in NSDLDrv/Src/NSDLViewport.cpp (Android-only).
extern "C" int UT99_IsMenuActive();
extern "C" UViewport *UT99_GetViewport();

static void sendKey(int state, SDL_Scancode scancode)
{
    SDL_SendKeyboardKey(state ? SDL_PRESSED : SDL_RELEASED, scancode);
}

// --- stdout/stderr -> logcat pump -------------------------------------------
//
// UT99's critical errors (appErrorf, guard-history dumps) go through
// FOutputDeviceAnsiError, which printf()s to stdout/stderr - invisible on
// Android. Redirect both to logcat so startup failures are visible. Normal
// debugf output is separately mirrored by FOutputDeviceFile's own LOGI hook.

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

void PortableBackButton(void)
{
    // Escape toggles the UWindow menu open/closed.
    sendKey(1, SDL_SCANCODE_ESCAPE);
    sendKey(0, SDL_SCANCODE_ESCAPE);
}

int PortableKeyEvent(int state, int code, int unitcode)
{
    // code is already an SDL_Scancode (on-screen keyboard, hardware keys, and
    // the menu/back button all route here).
    sendKey(state, (SDL_Scancode)code);
    return 0;
}

// --- RESERVED KEYS -----------------------------------------------------------
//
// EInputKey slots no real keyboard/mouse/gamepad can generate (NSDLViewport's
// SDL_Scancode -> EInputKey table never targets them) and which never appear in
// UT's "press a key to bind" control options - so nothing the player does can
// retarget or interfere with them. Each is bound to a *signed* raw Axis command
// and driven with a signed magnitude every tick (positive = forward/right,
// negative = the opposite) - one key does both directions.
enum
{
    RK_MOVE_AXIS   = IK_Unknown88, // +forward / -back
    RK_TURN_AXIS   = IK_Unknown89, // +right   / -left
    RK_STRAFE_AXIS = IK_Unknown8A, // +right   / -left
};

static bool s_reservedKeysBound = false;

static void ensureReservedKeysBound(UViewport *vp)
{
    if (s_reservedKeysBound)
        return;

    // Same raw commands MoveForward/TurnRight/StrafeRight resolve to in the ini
    // (positive-Speed half only - our own sign supplies the direction), so these
    // keep tracking the ini speeds if ever tuned, same as a real key would.
    vp->Input->Bindings[RK_MOVE_AXIS]   = TEXT("Axis aBaseY Speed=+300.0");
    vp->Input->Bindings[RK_TURN_AXIS]   = TEXT("Axis aBaseX Speed=+150.0");
    vp->Input->Bindings[RK_STRAFE_AXIS] = TEXT("Axis aStrafe Speed=+300.0");

    s_reservedKeysBound = true;
}

// --- Cross-thread state ------------------------------------------------------
//
// Set from PortableAction/PortableMove*/PortableLook* (touch thread). Only ever
// read from UT99_TickPortableActions() (engine thread).

// Movement/turn, each -1..+1. Digital (D-pad) input sets a fixed +-1; analog
// (joystick) sets the actual stick fraction, so a soft push moves slower.
static volatile float s_moveAxis = 0.0f, s_turnAxis = 0.0f, s_strafeAxis = 0.0f;

// Held real buttons, diffed against the last-applied state each tick.
static volatile bool s_wantFire = false, s_wantAltFire = false, s_wantDuck = false;
static volatile bool s_wantWalk = false, s_wantStrafeMod = false;

// One-shot commands (Jump, weapon switches, ...). Single-producer (touch
// thread) / single-consumer (engine thread) ring buffer.
#define CMD_QUEUE_LEN 32
static char s_cmdQueue[CMD_QUEUE_LEN][32];
static volatile int s_cmdAvail = 0;
static volatile int s_cmdUsed = 0;

static void postCommand(const char *cmd)
{
    if (s_cmdAvail >= s_cmdUsed + CMD_QUEUE_LEN)
        return; // full, drop
    int slot = s_cmdAvail & (CMD_QUEUE_LEN - 1);
    strncpy(s_cmdQueue[slot], cmd, sizeof(s_cmdQueue[0]) - 1);
    s_cmdQueue[slot][sizeof(s_cmdQueue[0]) - 1] = 0;
    s_cmdAvail++;
}

// Starting points, in the same ballpark as TFE/OpenJK's mouse scales. Tune to taste.
static const float LOOK_MOUSE_YAW_SCALE = 3500.0f;
static const float LOOK_MOUSE_PITCH_SCALE = 1000.0f;
static const float LOOK_JOY_YAW_SCALE = 40.0f;
static const float LOOK_JOY_PITCH_SCALE = 30.0f;

void PortableAction(int state, int action)
{
    // Generic user-bindable buttons: KP_1-KP_0 for 0-9, A-P for 10-25 (same
    // scheme OpenJK uses). Meant to be rebindable via UT's own control options,
    // so they go through real SDL keys.
    if (action >= PORT_ACT_CUSTOM_0 && action <= PORT_ACT_CUSTOM_25)
    {
        if (action <= PORT_ACT_CUSTOM_9)
            sendKey(state, (SDL_Scancode)(SDL_SCANCODE_KP_1 + action - PORT_ACT_CUSTOM_0));
        else
            sendKey(state, (SDL_Scancode)(SDL_SCANCODE_A + action - PORT_ACT_CUSTOM_10));
        return;
    }

    switch (action)
    {
        // --- Menu navigation: UT's menu/console reads these EInputKeys
        // directly, bypassing Bindings - real SDL keys are correct here.
        case PORT_ACT_MENU_UP:      sendKey(state, SDL_SCANCODE_UP);      return;
        case PORT_ACT_MENU_DOWN:    sendKey(state, SDL_SCANCODE_DOWN);    return;
        case PORT_ACT_MENU_LEFT:    sendKey(state, SDL_SCANCODE_LEFT);    return;
        case PORT_ACT_MENU_RIGHT:   sendKey(state, SDL_SCANCODE_RIGHT);   return;
        case PORT_ACT_MENU_SELECT:  sendKey(state, SDL_SCANCODE_RETURN);  return;
        case PORT_ACT_MENU_CONFIRM: sendKey(state, SDL_SCANCODE_Y);       return;
        case PORT_ACT_MENU_BACK:
        case PORT_ACT_MENU_ABORT:
        case PORT_ACT_MENU_SHOW:    sendKey(state, SDL_SCANCODE_ESCAPE);  return;
        case PORT_ACT_CONSOLE:      sendKey(state, SDL_SCANCODE_GRAVE);   return; // hardcoded IK_Tilde in Console

        case PORT_ACT_MOUSE_LEFT:   MouseButton(state, BUTTON_PRIMARY);   return;
        case PORT_ACT_MOUSE_RIGHT:  MouseButton(state, BUTTON_SECONDARY); return;

        // --- Digital movement/turn: same -1/0/+1 axis PortableMove*/analog
        // sticks use, so both input styles reach the engine the same way.
        case PORT_ACT_FWD:         s_moveAxis = state ? 1.0f : 0.0f;    return;
        case PORT_ACT_BACK:        s_moveAxis = state ? -1.0f : 0.0f;   return;
        case PORT_ACT_LEFT:        s_turnAxis = state ? -1.0f : 0.0f;   return; // turn left
        case PORT_ACT_RIGHT:       s_turnAxis = state ? 1.0f : 0.0f;    return; // turn right
        case PORT_ACT_MOVE_LEFT:   s_strafeAxis = state ? -1.0f : 0.0f; return;
        case PORT_ACT_MOVE_RIGHT:  s_strafeAxis = state ? 1.0f : 0.0f;  return;

        // --- Held real buttons/aliases ---
        case PORT_ACT_STRAFE:      s_wantStrafeMod = state != 0;   return; // hold: Left/Right turn -> strafe
        case PORT_ACT_ATTACK:      s_wantFire = state != 0;        return;
        case PORT_ACT_ALT_ATTACK:
        case PORT_ACT_ALT_FIRE:    s_wantAltFire = state != 0;     return;
        case PORT_ACT_DOWN:
        case PORT_ACT_CROUCH:      s_wantDuck = state != 0;        return;
        case PORT_ACT_SPEED:
        case PORT_ACT_SPRINT:
        case PORT_ACT_SMART_TOGGLE_RUN:
        case PORT_ACT_ALWAYS_RUN:  s_wantWalk = state != 0;        return; // UT runs by default; this walks instead

        // --- One-shot real commands, fire on press only ---
        case PORT_ACT_JUMP:
        case PORT_ACT_UP:          if (state) postCommand("Jump");            return;
        case PORT_ACT_NEXT_WEP:    if (state) postCommand("NextWeapon");      return;
        case PORT_ACT_PREV_WEP:    if (state) postCommand("PrevWeapon");      return;
        case PORT_ACT_WEAP0:       if (state) postCommand("SwitchWeapon 10"); return;
        case PORT_ACT_WEAP1:       if (state) postCommand("SwitchWeapon 1");  return;
        case PORT_ACT_WEAP2:       if (state) postCommand("SwitchWeapon 2");  return;
        case PORT_ACT_WEAP3:       if (state) postCommand("SwitchWeapon 3");  return;
        case PORT_ACT_WEAP4:       if (state) postCommand("SwitchWeapon 4");  return;
        case PORT_ACT_WEAP5:       if (state) postCommand("SwitchWeapon 5");  return;
        case PORT_ACT_WEAP6:       if (state) postCommand("SwitchWeapon 6");  return;
        case PORT_ACT_WEAP7:       if (state) postCommand("SwitchWeapon 7");  return;
        case PORT_ACT_WEAP8:       if (state) postCommand("SwitchWeapon 8");  return;
        case PORT_ACT_WEAP9:       if (state) postCommand("SwitchWeapon 9");  return;
        case PORT_ACT_INVUSE:      if (state) postCommand("ActivateItem");    return;
        case PORT_ACT_INVNEXT:     if (state) postCommand("NextItem");        return;
        case PORT_ACT_INVPREV:     if (state) postCommand("PrevItem");        return;
        case PORT_ACT_QUICKSAVE:   if (state) postCommand("QuickSave");       return;
        case PORT_ACT_QUICKLOAD:   if (state) postCommand("QuickLoad");       return;
        case PORT_ACT_SHOW_KBRD:   if (state) postCommand("Talk");            return; // chat text entry
        case PORT_ACT_DATAPAD:     if (state) postCommand("ActivateTranslator"); return;
    }
}

// --- Analog movement ---------------------------------------------------------
//
// touch_interface_base.cpp scales its analog stick up to roughly +-15 (fwd) /
// +-10 (strafe) at default sensitivity, not a normalised -1..1, so bring it back
// to a roughly -1..1 fraction of full speed, matching what a real analog axis
// reports (and what s_moveAxis/s_strafeAxis expect).
static const float FWD_STICK_RANGE = 5.0f;
static const float STRAFE_STICK_RANGE = 3.0f;

void PortableMoveFwd(float fwd)
{
    s_moveAxis = fwd / FWD_STICK_RANGE;
}

void PortableMoveSide(float strafe)
{
    s_strafeAxis = strafe / STRAFE_STICK_RANGE;
}

void PortableMove(float fwd, float strafe)
{
    PortableMoveFwd(fwd);
    PortableMoveSide(strafe);
}

// --- Look / mouse ------------------------------------------------------------
//
// Genuinely analog either way (a real mouse axis isn't in UT's rebinding UI at
// all), so this stays on the thread-safe MouseMove() injection, batched into one
// MouseMove() per tick in UT99_TickPortableActions(). _mouse accumulates per-
// swipe deltas, drained & zeroed each tick; _joy is a held magnitude, kept until
// the touch layer sends a new value (goes to 0 on its own once recentred).
static volatile float s_lookYawMouse = 0.0f, s_lookPitchMouse = 0.0f;
static volatile float s_lookYawJoy = 0.0f, s_lookPitchJoy = 0.0f;

void PortableLookPitch(int mode, float pitch)
{
    if (mode == LOOK_MODE_JOYSTICK)
        s_lookPitchJoy = pitch * LOOK_JOY_PITCH_SCALE;
    else
        s_lookPitchMouse += pitch * LOOK_MOUSE_PITCH_SCALE;
}

void PortableLookYaw(int mode, float yaw)
{
    if (mode == LOOK_MODE_JOYSTICK)
        s_lookYawJoy = yaw * LOOK_JOY_YAW_SCALE;
    else
        s_lookYawMouse += yaw * LOOK_MOUSE_YAW_SCALE;
}

void PortableMouse(float dx, float dy)
{
    // A direct touch swipe (no virtual stick) is treated as mouse-mode look.
    s_lookYawMouse += dx * LOOK_MOUSE_YAW_SCALE;
    s_lookPitchMouse += dy * LOOK_MOUSE_PITCH_SCALE;
}

void PortableMouseAbs(float x, float y)
{
    // Menu mouse uses relative cursor drag (touch_interface_ut99.cpp), not
    // tap-to-position, so nothing to do here.
}

void PortableMouseButton(int state, int button, float dx, float dy)
{
    MouseButton(state, button);
}

void PortableCommand(const char *cmd)
{
    postCommand(cmd);
}

void PortableAutomapControl(float zoom, float x, float y)
{
    // UT99 has no automap.
}

int PortableShowKeyboard(void)
{
    return 0;
}

bool PortableSetAlwaysRun(bool run)
{
    return run;
}

touchscreemode_t PortableGetScreenMode()
{
    return UT99_IsMenuActive() ? TS_MENU : TS_GAME;
}

void PortableSetMouseTapMode(int enable)
{
}

int PortableGetMouseTapMode(void)
{
    return 0;
}

// --- Engine-thread drain (called once per tick from MainLoop(), Launch.cpp) ---

// Drives a reserved axis key exactly like NSDLViewport::TickInput() drives a
// real analog joystick axis: every tick, while non-zero, feed the current
// magnitude in as an IST_Axis delta scaled by real elapsed time. 100.0 makes a
// full-deflection magnitude (+-1) match the rate of a real held key (IST_Hold
// applies DeltaSeconds*Speed; IST_Axis applies 0.01*Delta*Speed - so Delta must
// be ~100*DeltaSeconds for the two to match), scaling down proportionally.
static void applyAnalogAxis(UViewport *vp, int reservedKey, float magnitude, float deltaSeconds)
{
    if (magnitude == 0.0f)
        return;
    vp->Input->Process(*GLog, (EInputKey)reservedKey, IST_Axis, magnitude * 100.0f * deltaSeconds);
}

// Same transition-on-change idea for a real alias name (Fire/AltFire/Duck/
// Walking/Strafe) - simple flags with no continuous per-tick behaviour, so
// Exec() is called directly, bracketed by the same press/release input-action
// state Process() would set.
static void applyAliasHold(UViewport *vp, const char *aliasName, bool want, bool &held)
{
    if (want == held)
        return;
    vp->Input->SetInputAction(want ? IST_Press : IST_Release, 0.0f);
    vp->Exec(aliasName, *GLog);
    vp->Input->SetInputAction(IST_None, 0.0f);
    held = want;
}

extern "C" void UT99_TickPortableActions()
{
    UViewport *vp = UT99_GetViewport();
    if (!vp || !vp->Input)
        return;
    ensureReservedKeysBound(vp);

    static DOUBLE lastTime = 0.0;
    DOUBLE now = appSeconds();
    FLOAT deltaSeconds = lastTime > 0.0 ? (FLOAT)(now - lastTime) : 0.0f;
    lastTime = now;

    applyAnalogAxis(vp, RK_MOVE_AXIS, s_moveAxis, deltaSeconds);
    applyAnalogAxis(vp, RK_TURN_AXIS, s_turnAxis, deltaSeconds);
    applyAnalogAxis(vp, RK_STRAFE_AXIS, s_strafeAxis, deltaSeconds);

    static bool heldFire = false, heldAltFire = false, heldDuck = false;
    static bool heldWalk = false, heldStrafeMod = false;

    applyAliasHold(vp, "Fire", s_wantFire, heldFire);
    applyAliasHold(vp, "AltFire", s_wantAltFire, heldAltFire);
    applyAliasHold(vp, "Duck", s_wantDuck, heldDuck);
    applyAliasHold(vp, "Walking", s_wantWalk, heldWalk);
    applyAliasHold(vp, "Strafe", s_wantStrafeMod, heldStrafeMod);

    // _joy is a held magnitude re-sent every tick (not a discrete swipe delta
    // like _mouse), so scale it by elapsed time (normalised to 60Hz) or turning
    // speed would track the frame rate.
    float joyFrameScale = deltaSeconds * 60.0f;
    float yaw = s_lookYawMouse + s_lookYawJoy * joyFrameScale;
    float pitch = s_lookPitchMouse + s_lookPitchJoy * joyFrameScale;
    if (yaw != 0.0f || pitch != 0.0f)
        MouseMove(yaw, pitch);
    s_lookYawMouse = 0.0f;
    s_lookPitchMouse = 0.0f;

    while (s_cmdUsed != s_cmdAvail)
    {
        // IST_Press matters even for one-shots: a command that is itself an
        // alias (e.g. "Jump", whose ini command is "Jump | Axis aUp Speed=+300.0")
        // only cascades through Exec() while the input action is IST_Press;
        // otherwise UInput::Exec()'s own alias lookup swallows it. A real keypress
        // always has IST_Press set for the call, so match it.
        vp->Input->SetInputAction(IST_Press, 0.0f);
        vp->Exec(s_cmdQueue[s_cmdUsed & (CMD_QUEUE_LEN - 1)], *GLog);
        vp->Input->SetInputAction(IST_None, 0.0f);
        s_cmdUsed++;
    }
}
