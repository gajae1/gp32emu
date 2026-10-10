#include "input/gp32_sdl3_input_shared.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Gamepad thumbsticks span roughly -32768..32767; a stick that no longer
   returns to center must not drift the d-pad, so require about half travel. */
#define GP32_PAD_STICK_DEADZONE 16384
/* Triggers span 0..32767, so half travel counts as a press. */
#define GP32_PAD_TRIGGER_THRESHOLD 16384

static void set_device_name(gp32_sdl3_input_state_t *s, const char *name) {
    if (!s) return;
    snprintf(s->device_name, sizeof(s->device_name), "%s", (name && name[0]) ? name : "");
}

static void set_error(gp32_sdl3_input_state_t *s, const char *msg) {
    if (!s) return;
    if (!msg) msg = "SDL3 input error";
    snprintf(s->error, sizeof(s->error), "%s", msg);
}

void gp32_sdl3_input_state_init(gp32_sdl3_input_state_t *s, int enable_joystick, int enable_joystick_axis) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->enable_joystick = enable_joystick != 0;
    s->enable_joystick_axis = enable_joystick_axis != 0;
}

static void release_raw_joystick(gp32_sdl3_input_state_t *s) {
    if (s->joy) { SDL_CloseJoystick(s->joy); s->joy = NULL; }
    s->num_axes = s->num_buttons = s->num_hats = 0;
    memset(s->axis_centered, 0, sizeof(s->axis_centered));
    memset(s->axis_state, 0, sizeof(s->axis_state));
    memset(s->axis_pending, 0, sizeof(s->axis_pending));
    memset(s->axis_pending_count, 0, sizeof(s->axis_pending_count));
}

static void close_device(gp32_sdl3_input_state_t *s) {
    if (s->pad) { SDL_CloseGamepad(s->pad); s->pad = NULL; }
    release_raw_joystick(s);
    s->device_id = 0;
    s->device_name[0] = 0;
}

void gp32_sdl3_input_state_shutdown(gp32_sdl3_input_state_t *s) {
    if (!s) return;
    close_device(s);
}

/* A recognised controller replaces an open raw joystick, so a gamepad plugged
   in after start works even when the raw fallback already holds a device. */
static int open_gamepad_replacing_raw(gp32_sdl3_input_state_t *s, SDL_JoystickID id) {
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) return 0;
    release_raw_joystick(s);
    s->pad = pad;
    SDL_UpdateGamepads();
    set_device_name(s, SDL_GetGamepadName(pad));
    s->device_id = id;
    s->error[0] = 0;
    return 1;
}

/* Open a gamepad when SDL has a mapping for the device (DualShock/DualSense,
   Switch Pro, generic pads, ...), otherwise keep the old raw joystick path. */
static int open_device(gp32_sdl3_input_state_t *s, SDL_JoystickID id) {
    s->pad = SDL_OpenGamepad(id);
    if (s->pad) {
        SDL_UpdateGamepads();
        set_device_name(s, SDL_GetGamepadName(s->pad));
    } else {
        s->joy = SDL_OpenJoystick(id);
        if (!s->joy) return 0;
        SDL_UpdateJoysticks();
        s->num_axes = SDL_GetNumJoystickAxes(s->joy);
        s->num_buttons = SDL_GetNumJoystickButtons(s->joy);
        s->num_hats = SDL_GetNumJoystickHats(s->joy);
        for (int i = 0; i < 2; ++i) {
            if (i < s->num_axes) {
                int raw = (int)SDL_GetJoystickAxis(s->joy, i);
                if (raw > -8000 && raw < 8000) s->axis_centered[i] = 1;
            }
        }
        set_device_name(s, SDL_GetJoystickName(s->joy));
    }
    s->device_id = id;
    s->error[0] = 0;
    return 1;
}

void gp32_sdl3_input_open_first_joystick(gp32_sdl3_input_state_t *s) {
    if (!s || !s->enable_joystick || s->pad || s->joy) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids) {
        if (count > 0) open_device(s, ids[0]);
        SDL_free(ids);
    }
    if (s->pad) return;
    ids = SDL_GetJoysticks(&count);
    if (ids) {
        if (count > 0) open_device(s, ids[0]);
        SDL_free(ids);
    }
    if (!s->pad && !s->joy) {
        const char *err = SDL_GetError();
        if (err && err[0]) set_error(s, err);
    }
}

void gp32_sdl3_input_handle_event(gp32_sdl3_input_state_t *s, const SDL_Event *ev, int *quit_requested) {
    if (!ev) return;
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        if (quit_requested) *quit_requested = 1;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!ev->key.repeat) {
            if (ev->key.key == SDLK_ESCAPE) { if (quit_requested) *quit_requested = 1; }
            else if (ev->key.key == SDLK_F5 && s) s->pending_actions |= GP32_FRONTEND_ACTION_SAVE_STATE;
            else if (ev->key.key == SDLK_F8 && s) s->pending_actions |= GP32_FRONTEND_ACTION_LOAD_STATE;
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (s && s->enable_joystick && !s->pad && !open_gamepad_replacing_raw(s, ev->gdevice.which))
            set_error(s, SDL_GetError());
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (s && s->device_id && s->device_id == ev->gdevice.which) {
            close_device(s);
            gp32_sdl3_input_open_first_joystick(s); /* fall over to another connected pad */
        }
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        /* A mapped controller is announced as both joystick and gamepad, so
           open_device() picks the gamepad form here and the later gamepad
           event finds the device already open. */
        if (s && s->enable_joystick && !s->pad && !s->joy && !open_device(s, ev->jdevice.which))
            set_error(s, SDL_GetError());
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        if (s && s->device_id && s->device_id == ev->jdevice.which) {
            close_device(s);
            gp32_sdl3_input_open_first_joystick(s);
        }
        break;
    default:
        break;
    }
}

uint32_t gp32_sdl3_input_keyboard_buttons(void) {
    uint32_t mask = 0;
    const bool *keys = SDL_GetKeyboardState(NULL);
    if (!keys) return 0;
    if (keys[SDL_SCANCODE_LEFT])   mask |= GP32_BUTTON_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])  mask |= GP32_BUTTON_RIGHT;
    if (keys[SDL_SCANCODE_UP])     mask |= GP32_BUTTON_UP;
    if (keys[SDL_SCANCODE_DOWN])   mask |= GP32_BUTTON_DOWN;
    if (keys[SDL_SCANCODE_Z])      mask |= GP32_BUTTON_A;
    if (keys[SDL_SCANCODE_X])      mask |= GP32_BUTTON_B;
    if (keys[SDL_SCANCODE_A])      mask |= GP32_BUTTON_L;
    if (keys[SDL_SCANCODE_S])      mask |= GP32_BUTTON_R;
    if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE]) mask |= GP32_BUTTON_START;
    if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_C]) mask |= GP32_BUTTON_SELECT;
    return mask;
}

static int joy_button(const gp32_sdl3_input_state_t *s, int index) {
    return s && s->joy && index >= 0 && index < s->num_buttons && SDL_GetJoystickButton(s->joy, index);
}

static int pad_button(const gp32_sdl3_input_state_t *s, SDL_GamepadButton button) {
    return s && s->pad && SDL_GetGamepadButton(s->pad, button);
}

/* Standard gamepad layout: d-pad and left stick drive the GP32 directions,
   SOUTH/EAST are A/B, the shoulders are L/R, and the analog triggers read as
   L/R too once they pass half travel. The stick ignores enable_joystick_axis
   because gamepad axes are already deadzone-filtered here. */
static uint32_t gamepad_buttons(const gp32_sdl3_input_state_t *s) {
    uint32_t mask = 0;
    if (!s->pad) return 0;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  mask |= GP32_BUTTON_LEFT;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) mask |= GP32_BUTTON_RIGHT;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_DPAD_UP))    mask |= GP32_BUTTON_UP;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  mask |= GP32_BUTTON_DOWN;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_SOUTH)) mask |= GP32_BUTTON_A;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_EAST))  mask |= GP32_BUTTON_B;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  mask |= GP32_BUTTON_L;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) mask |= GP32_BUTTON_R;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_START)) mask |= GP32_BUTTON_START;
    if (pad_button(s, SDL_GAMEPAD_BUTTON_BACK))  mask |= GP32_BUTTON_SELECT;
    Sint16 x = SDL_GetGamepadAxis(s->pad, SDL_GAMEPAD_AXIS_LEFTX);
    Sint16 y = SDL_GetGamepadAxis(s->pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (x <= -GP32_PAD_STICK_DEADZONE) mask |= GP32_BUTTON_LEFT;
    else if (x >= GP32_PAD_STICK_DEADZONE) mask |= GP32_BUTTON_RIGHT;
    if (y <= -GP32_PAD_STICK_DEADZONE) mask |= GP32_BUTTON_UP;
    else if (y >= GP32_PAD_STICK_DEADZONE) mask |= GP32_BUTTON_DOWN;
    if (SDL_GetGamepadAxis(s->pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= GP32_PAD_TRIGGER_THRESHOLD) mask |= GP32_BUTTON_L;
    if (SDL_GetGamepadAxis(s->pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= GP32_PAD_TRIGGER_THRESHOLD) mask |= GP32_BUTTON_R;
    return mask;
}

static int filtered_axis_dir(gp32_sdl3_input_state_t *s, int axis) {
    const int center = 8000;
    const int press = 24000;
    const int release = 18000;
    int raw = 0;
    int next = 0;
    if (!s || !s->joy || axis < 0 || axis >= 2 || axis >= s->num_axes) return 0;
    raw = (int)SDL_GetJoystickAxis(s->joy, axis);
    if (!s->axis_centered[axis]) {
        if (raw > -center && raw < center) s->axis_centered[axis] = 1;
        else { s->axis_state[axis] = 0; s->axis_pending[axis] = 0; s->axis_pending_count[axis] = 0; return 0; }
    }
    if (s->axis_state[axis] < 0) next = (raw < -release) ? -1 : 0;
    else if (s->axis_state[axis] > 0) next = (raw > release) ? 1 : 0;
    else if (raw < -press) next = -1;
    else if (raw > press) next = 1;
    if (next != s->axis_state[axis]) {
        if (next == s->axis_pending[axis]) ++s->axis_pending_count[axis];
        else { s->axis_pending[axis] = next; s->axis_pending_count[axis] = 1; }
        if (s->axis_pending_count[axis] >= 2u) { s->axis_state[axis] = next; s->axis_pending_count[axis] = 0; }
    } else { s->axis_pending[axis] = next; s->axis_pending_count[axis] = 0; }
    return s->axis_state[axis];
}

uint32_t gp32_sdl3_input_joystick_buttons(gp32_sdl3_input_state_t *s) {
    uint32_t mask = 0;
    if (!s) return 0;
    if (s->pad) return gamepad_buttons(s);
    if (!s->joy) return 0;
    if (s->num_hats > 0) {
        Uint8 hat = SDL_GetJoystickHat(s->joy, 0);
        if (hat & SDL_HAT_LEFT)  mask |= GP32_BUTTON_LEFT;
        if (hat & SDL_HAT_RIGHT) mask |= GP32_BUTTON_RIGHT;
        if (hat & SDL_HAT_UP)    mask |= GP32_BUTTON_UP;
        if (hat & SDL_HAT_DOWN)  mask |= GP32_BUTTON_DOWN;
    }
    if (s->enable_joystick_axis) {
        int ax0 = filtered_axis_dir(s, 0);
        int ax1 = filtered_axis_dir(s, 1);
        if (ax0 < 0) mask |= GP32_BUTTON_LEFT;
        if (ax0 > 0) mask |= GP32_BUTTON_RIGHT;
        if (ax1 < 0) mask |= GP32_BUTTON_UP;
        if (ax1 > 0) mask |= GP32_BUTTON_DOWN;
    }
    if (joy_button(s, 0)) mask |= GP32_BUTTON_A;
    if (joy_button(s, 1)) mask |= GP32_BUTTON_B;
    if (joy_button(s, 4)) mask |= GP32_BUTTON_L;
    if (joy_button(s, 5)) mask |= GP32_BUTTON_R;
    if (joy_button(s, 7)) mask |= GP32_BUTTON_START;
    if (joy_button(s, 6)) mask |= GP32_BUTTON_SELECT;
    return mask;
}

uint32_t gp32_sdl3_input_take_actions(gp32_sdl3_input_state_t *s) {
    if (!s) return 0u;
    uint32_t a = s->pending_actions;
    s->pending_actions = 0u;
    return a;
}

gp32_status_t gp32_sdl3_input_poll(gp32_sdl3_input_state_t *s, int include_keyboard, uint32_t extra_buttons, uint32_t *out_buttons, int *out_quit) {
    if (!s || !out_buttons || !out_quit) return GP32_ERR_INVALID_ARGUMENT;
    int quit = 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) gp32_sdl3_input_handle_event(s, &ev, &quit);
    SDL_PumpEvents();
    if (s->pad) SDL_UpdateGamepads();
    else if (s->joy) SDL_UpdateJoysticks();
    uint32_t mask = extra_buttons;
    if (include_keyboard) mask |= gp32_sdl3_input_keyboard_buttons();
    mask |= gp32_sdl3_input_joystick_buttons(s);
    *out_buttons = mask;
    *out_quit = quit;
    return GP32_OK;
}

const char *gp32_sdl3_input_error(const gp32_sdl3_input_state_t *s) {
    return (s && s->error[0]) ? s->error : "";
}

const char *gp32_sdl3_input_device_name(const gp32_sdl3_input_state_t *s) {
    return (s && s->device_name[0]) ? s->device_name : "";
}
