/*=============================================================================
	game_interface.cpp: OpenTouch <-> UT99 bridge.
	Ported from the sibling UE1 module; see CLAUDE.md "Gameplay input" for the
	design (rebind-proof Exec actions, reserved-key axis pump, threading rules).
	MouseButton/MouseMove etc are defined by Clibs_OpenTouch/android_jni_inc.cpp.
=============================================================================*/

// SDL first (main -> SDL_main remap); C system headers must precede Engine.h,
// whose clock() macro mangles <time.h>.
#include "SDL2/SDL.h"

#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <android/log.h>

#include "Engine.h"
#include "game_interface.h"

// Engine entry point (UnrealTournament/Src/Launch.cpp).
extern int main(int argc, char **argv);

// SDL's internal key injection: queues a real event, safe from any thread.
extern "C" int SDL_SendKeyboardKey(Uint8 state, SDL_Scancode scancode);

// Defined in NSDLDrv/Src/NSDLViewport.cpp (Android-only).
extern "C" int UT99_IsMenuActive();
extern "C" UViewport *UT99_GetViewport();

static void sendKey(int state, SDL_Scancode scancode)
{
    SDL_SendKeyboardKey(state ? SDL_PRESSED : SDL_RELEASED, scancode);
}

// Critical errors (appErrorf) print to stdout/stderr - invisible on Android,
// so pump both to logcat.
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
    // code is already an SDL_Scancode.
    sendKey(state, (SDL_Scancode)code);
    return 0;
}

// Reserved EInputKey slots no real device can generate. Two families:
//   *_AXIS keys are the analog path (virtual sticks / turn buttons) - bound to
//   the positive-Speed half of an Axis command and fed a signed magnitude via
//   IST_Axis each tick, so one key covers both directions and a soft push moves
//   proportionally slower, like a real analog joystick axis.
//   *_KEY keys are the digital path (D-pad forward/back/strafe buttons) - each
//   bound to a fixed-sign Axis command and pressed/released via Process()
//   (IST_Press/IST_Release) like a real key, letting UE1's own ReadInput() pump
//   apply IST_Hold at full Speed every tick. Same path a real WASD key takes,
//   one level below synthesizing a WASD scancode. Direction is baked into the
//   binding, so the digital family needs one key per direction.
enum
{
    RK_MOVE_AXIS        = IK_Unknown88, // analog: +forward / -back
    RK_TURN_AXIS        = IK_Unknown89, // analog: +right   / -left
    RK_STRAFE_AXIS      = IK_Unknown8A, // analog: +right   / -left
    RK_FWD_KEY          = IK_Unknown8B, // digital: forward
    RK_BACK_KEY         = IK_Unknown8C, // digital: back
    RK_STRAFE_LEFT_KEY  = IK_Unknown8D, // digital: strafe left
    RK_STRAFE_RIGHT_KEY = IK_Unknown8E, // digital: strafe right
};

static bool s_reservedKeysBound = false;

static void ensureReservedKeysBound(UViewport *vp)
{
    if (s_reservedKeysBound)
        return;

    // Same raw commands the ini's MoveForward/TurnRight/StrafeRight resolve to
    // (analog keys take the positive half - our own sign supplies the direction;
    // digital keys bake the sign into the binding).
    vp->Input->Bindings[RK_MOVE_AXIS]        = TEXT("Axis aBaseY Speed=+300.0");
    vp->Input->Bindings[RK_TURN_AXIS]        = TEXT("Axis aBaseX Speed=+150.0");
    vp->Input->Bindings[RK_STRAFE_AXIS]      = TEXT("Axis aStrafe Speed=+300.0");
    vp->Input->Bindings[RK_FWD_KEY]          = TEXT("Axis aBaseY Speed=+300.0");
    vp->Input->Bindings[RK_BACK_KEY]         = TEXT("Axis aBaseY Speed=-300.0");
    vp->Input->Bindings[RK_STRAFE_LEFT_KEY]  = TEXT("Axis aStrafe Speed=-300.0");
    vp->Input->Bindings[RK_STRAFE_RIGHT_KEY] = TEXT("Axis aStrafe Speed=+300.0");

    s_reservedKeysBound = true;
}

// Cross-thread state: written on the touch thread, read only by
// UT99_TickPortableActions() (engine thread).

// Analog movement/turn, -1..+1: set by the virtual sticks (and, for turn, the
// digital turn buttons), fed through the IST_Axis path each tick.
static volatile float s_moveAxis = 0.0f, s_turnAxis = 0.0f, s_strafeAxis = 0.0f;

// Digital forward/back/strafe buttons: held state, driven through the real-key
// press/release + IST_Hold path (RK_*_KEY above) at full keyboard speed.
static volatile bool s_wantFwd = false, s_wantBack = false;
static volatile bool s_wantStrafeLeft = false, s_wantStrafeRight = false;

// Held real buttons, diffed against the last-applied state each tick.
static volatile bool s_wantFire = false, s_wantAltFire = false, s_wantDuck = false;
static volatile bool s_wantWalk = false, s_wantStrafeMod = false;

// One-shot commands, single-producer / single-consumer ring buffer.
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

// Starting points, tune to taste.
static const float LOOK_MOUSE_YAW_SCALE = 3500.0f;
static const float LOOK_MOUSE_PITCH_SCALE = 1000.0f;
static const float LOOK_JOY_YAW_SCALE = 40.0f;
static const float LOOK_JOY_PITCH_SCALE = 30.0f;

void PortableAction(int state, int action)
{
    // Rebindable custom buttons: KP_1-KP_0 for 0-9, A-P for 10-25 (OpenJK scheme).
    if (action >= PORT_ACT_CUSTOM_0 && action <= PORT_ACT_CUSTOM_25)
    {
        if (action <= PORT_ACT_CUSTOM_9)
            sendKey(state, (SDL_Scancode)(SDL_SCANCODE_KP_1 + action - PORT_ACT_CUSTOM_0));
        else
            sendKey(state, (SDL_Scancode)(SDL_SCANCODE_A + action - PORT_ACT_CUSTOM_10));
    }
    else if (PortableGetScreenMode() == TS_MENU)
    {
        // Menu nav only fires while the menu is open (same split Delta Touch's
        // gzdoom_game_interface.cpp uses). UWindow reads raw EInputKeys
        // directly, so real SDL keys are correct here.
        switch (action)
        {
            case PORT_ACT_MENU_UP:      sendKey(state, SDL_SCANCODE_UP);     return;
            case PORT_ACT_MENU_DOWN:    sendKey(state, SDL_SCANCODE_DOWN);   return;
            case PORT_ACT_MENU_LEFT:    sendKey(state, SDL_SCANCODE_LEFT);   return;
            case PORT_ACT_MENU_RIGHT:   sendKey(state, SDL_SCANCODE_RIGHT);  return;
            case PORT_ACT_MENU_SELECT:  sendKey(state, SDL_SCANCODE_RETURN); return;
            case PORT_ACT_MENU_BACK:    sendKey(state, SDL_SCANCODE_ESCAPE); return;
        }
    }
    else
    {
        switch (action)
        {
            case PORT_ACT_CONSOLE:      sendKey(state, SDL_SCANCODE_GRAVE);   return; // hardcoded IK_Tilde in Console

            case PORT_ACT_MOUSE_LEFT:   MouseButton(state, BUTTON_PRIMARY);   return;
            case PORT_ACT_MOUSE_RIGHT:  MouseButton(state, BUTTON_SECONDARY); return;

            // Digital forward/back/strafe: real-key press/release (RK_*_KEY),
            // driven at full keyboard speed by UE1's ReadInput() IST_Hold pump -
            // the digital equivalent of a bound WASD key, not the analog axis.
            case PORT_ACT_FWD:         s_wantFwd = state != 0;         return;
            case PORT_ACT_BACK:        s_wantBack = state != 0;        return;
            case PORT_ACT_MOVE_LEFT:   s_wantStrafeLeft = state != 0;  return;
            case PORT_ACT_MOVE_RIGHT:  s_wantStrafeRight = state != 0; return;

            // Digital turn stays on the analog axis (frame-rate-independent).
            case PORT_ACT_LEFT:        s_turnAxis = state ? -1.0f : 0.0f;   return; // turn left
            case PORT_ACT_RIGHT:       s_turnAxis = state ? 1.0f : 0.0f;    return; // turn right

            // Held aliases.
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

            // One-shot commands, fire on press only.
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
}

// The touch stick reports roughly +-15/+-10 at default sensitivity, not -1..1 -
// normalise back to a fraction of full speed.
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

// Look: batched into one MouseMove() per tick. _mouse accumulates swipe deltas
// (zeroed each tick); _joy is a held magnitude until the stick recentres.
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
    // Menus use relative drag, not tap-to-position.
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

// Feed the current magnitude as an IST_Axis delta; 100*dt makes full deflection
// match a real held key's rate (IST_Axis applies 0.01*Delta*Speed).
static void applyAnalogAxis(UViewport *vp, int reservedKey, float magnitude, float deltaSeconds)
{
    if (magnitude == 0.0f)
        return;
    vp->Input->Process(*GLog, (EInputKey)reservedKey, IST_Axis, magnitude * 100.0f * deltaSeconds);
}

// Press/release a reserved key exactly like a real keyboard key, then let UE1's
// own ReadInput() pump apply IST_Hold at full Speed every tick while held - the
// digital movement path, one level below a WASD scancode. v400 split the
// KeyDownTable maintenance out of Process() into a separate PreProcess() (the
// engine's own dispatch, UEngine::InputEvent, calls both in order), so both
// must be called here - Process() alone never marks the key held and ReadInput
// never pumps it.
static void applyKeyHold(UViewport *vp, int reservedKey, bool want, bool &held)
{
    if (want == held)
        return;
    EInputAction state = want ? IST_Press : IST_Release;
    vp->Input->PreProcess((EInputKey)reservedKey, state, 0.0f);
    vp->Input->Process(*GLog, (EInputKey)reservedKey, state, 0.0f);
    held = want;
}

// Transition-on-change hold of a real alias (Fire/Duck/...), bracketed by the
// press/release input-action state Process() would set.
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

    static bool heldFwd = false, heldBack = false;
    static bool heldStrafeLeft = false, heldStrafeRight = false;

    applyKeyHold(vp, RK_FWD_KEY, s_wantFwd, heldFwd);
    applyKeyHold(vp, RK_BACK_KEY, s_wantBack, heldBack);
    applyKeyHold(vp, RK_STRAFE_LEFT_KEY, s_wantStrafeLeft, heldStrafeLeft);
    applyKeyHold(vp, RK_STRAFE_RIGHT_KEY, s_wantStrafeRight, heldStrafeRight);

    static bool heldFire = false, heldAltFire = false, heldDuck = false;
    static bool heldWalk = false, heldStrafeMod = false;

    applyAliasHold(vp, "Fire", s_wantFire, heldFire);
    applyAliasHold(vp, "AltFire", s_wantAltFire, heldAltFire);
    applyAliasHold(vp, "Duck", s_wantDuck, heldDuck);
    applyAliasHold(vp, "Walking", s_wantWalk, heldWalk);
    applyAliasHold(vp, "Strafe", s_wantStrafeMod, heldStrafeMod);

    // _joy is re-sent every tick, so scale by elapsed time (60Hz reference).
    float joyFrameScale = deltaSeconds * 60.0f;
    float yaw = s_lookYawMouse + s_lookYawJoy * joyFrameScale;
    float pitch = s_lookPitchMouse + -s_lookPitchJoy * joyFrameScale;
    if (yaw != 0.0f || pitch != 0.0f)
        MouseMove(yaw, pitch);
    s_lookYawMouse = 0.0f;
    s_lookPitchMouse = 0.0f;

    while (s_cmdUsed != s_cmdAvail)
    {
        // IST_Press required: alias-name commands (e.g. "Jump") only cascade
        // through Exec() while the input action is IST_Press.
        vp->Input->SetInputAction(IST_Press, 0.0f);
        vp->Exec(s_cmdQueue[s_cmdUsed & (CMD_QUEUE_LEN - 1)], *GLog);
        vp->Input->SetInputAction(IST_None, 0.0f);
        s_cmdUsed++;
    }
}
