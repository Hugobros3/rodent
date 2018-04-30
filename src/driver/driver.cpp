#include <SDL2/SDL.h>

#include "common.h"

void setup_cpu_interface(size_t, size_t);

static bool handle_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return true;
            default:
                break;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    size_t width  = 1024;
    size_t height = 1024;

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        error("Cannot initialize SDL.");

    SDL_Window* window = SDL_CreateWindow(
        "Rodent",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        width,
        height,
        0);
    if (!window)
        error("Cannot create window.");

    setup_cpu_interface(width, height);

    bool done = false;
    while (!done) {
        done = handle_events();
        //render();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
