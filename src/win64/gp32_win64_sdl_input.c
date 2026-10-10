#include "gp32_win64_sdl_input.h"
#include "input/gp32_sdl3_input_shared.h"

#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gp32_win64_sdl_input {
    int initialized;
    SDL_InitFlags flags;
    char status[128];
    gp32_sdl3_input_state_t shared;
};

/* This frontend owns the keyboard through its own window procedure, which
   implements the documented Esc contract (leave fullscreen, never quit).
   Drop SDL key events so Esc and the F-key actions cannot bypass it, while
   controller, device and quit events keep flowing. */
static bool SDLCALL win64_sdl_event_filter(void *userdata, SDL_Event *ev) {
    (void)userdata;
    return ev->type != SDL_EVENT_KEY_DOWN && ev->type != SDL_EVENT_KEY_UP;
}

static void win64_sdl_update_status(gp32_win64_sdl_input_t *s) {
    const char *name = gp32_sdl3_input_device_name(&s->shared);
    snprintf(s->status, sizeof(s->status), "%s", name[0] ? name : "no controller");
}

gp32_win64_sdl_input_t *gp32_win64_sdl_input_create(int enable_joystick_axis) {
    gp32_win64_sdl_input_t *s = (gp32_win64_sdl_input_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    strcpy(s->status, "disabled");
    SDL_SetMainReady();
    /* The gamepad API gives every mapped controller the standard layout; fall
       back to the joystick subsystem alone when it cannot start. */
    SDL_InitFlags flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
    if (!SDL_InitSubSystem(flags)) {
        flags = SDL_INIT_JOYSTICK | SDL_INIT_EVENTS;
        if (!SDL_InitSubSystem(flags)) {
            const char *err = SDL_GetError();
            snprintf(s->status, sizeof(s->status), "SDL3 input init failed: %s", err ? err : "unknown");
            return s;
        }
    }
    s->flags = flags;
    s->initialized = 1;
    SDL_SetEventFilter(win64_sdl_event_filter, NULL);
    SDL_PumpEvents(); /* apply the filter to key events already queued */
    gp32_sdl3_input_state_init(&s->shared, 1, enable_joystick_axis);
    gp32_sdl3_input_open_first_joystick(&s->shared);
    win64_sdl_update_status(s);
    return s;
}

void gp32_win64_sdl_input_destroy(gp32_win64_sdl_input_t *s) {
    if (!s) return;
    if (s->initialized) {
        gp32_sdl3_input_state_shutdown(&s->shared);
        SDL_QuitSubSystem(s->flags);
    }
    free(s);
}

int gp32_win64_sdl_input_poll(gp32_win64_sdl_input_t *s, uint32_t keyboard_buttons, uint32_t *out_buttons, uint32_t *out_actions, int *quit_requested) {
    if (!s || !out_buttons || !out_actions || !quit_requested) return -1;
    uint32_t buttons = keyboard_buttons;
    int q = 0;
    if (s->initialized) {
        if (gp32_sdl3_input_poll(&s->shared, 0, keyboard_buttons, &buttons, &q) != GP32_OK) return -1;
        win64_sdl_update_status(s);
    }
    *out_buttons = buttons;
    *out_actions = gp32_sdl3_input_take_actions(&s->shared);
    if (q) *quit_requested = 1;
    return 0;
}

const char *gp32_win64_sdl_input_status(const gp32_win64_sdl_input_t *s) { return s ? s->status : "disabled"; }
