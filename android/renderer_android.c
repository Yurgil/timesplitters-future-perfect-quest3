#include "rhi.h"
#include "xr.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Window *game_window;
static pthread_mutex_t size_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t drawable_width, drawable_height;

static void refresh_drawable_size(void)
{
    if (tsfp_xr_enabled())
        return;
    int width, height;
    if (!SDL_GetWindowSizeInPixels(game_window, &width, &height) || width <= 0 || height <= 0)
        return;
    pthread_mutex_lock(&size_lock);
    drawable_width = (uint32_t)width;
    drawable_height = (uint32_t)height;
    pthread_mutex_unlock(&size_lock);
}

static int get_drawable_size(void *context, void *window, uint32_t *width, uint32_t *height)
{
    (void)context;
    if (window != game_window)
        return 0;
    if (SDL_IsMainThread())
        refresh_drawable_size();
    pthread_mutex_lock(&size_lock);
    *width = drawable_width;
    *height = drawable_height;
    pthread_mutex_unlock(&size_lock);
    return *width && *height;
}

static void pump_events(void *context)
{
    (void)context;
    if (!SDL_IsMainThread())
        return;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
             event.window.windowID == SDL_GetWindowID(game_window)))
            exit(0);
    }
    refresh_drawable_size();
}

void *tsfp_native_renderer_window(unsigned width, unsigned height)
{
    if (game_window)
        return game_window;
    if (!SDL_IsMainThread()) {
        fprintf(stderr, "[TSFP] Vulkan window requested outside the SDL video thread\n");
        return NULL;
    }
    game_window = SDL_CreateWindow("TimeSplitters: Future Perfect", (int)width, (int)height,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!game_window) {
        fprintf(stderr, "[TSFP] Vulkan window creation failed: %s\n", SDL_GetError());
        return NULL;
    }
    RhiHost host = { .pump_events = pump_events,
        .drawable_size = tsfp_xr_enabled() ? NULL : get_drawable_size };
    refresh_drawable_size();
    rhi_set_host(&host);
    fprintf(stderr, "[TSFP] SDL Android Vulkan window created\n");
    return game_window;
}
