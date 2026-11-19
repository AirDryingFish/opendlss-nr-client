// Loaded only into the test client. Emulate display vblank on an Xvfb display
// whose SDL software renderer has no physical display clock.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef struct SDL_Renderer SDL_Renderer;

void SDL_RenderPresent(SDL_Renderer* renderer) {
    static void (*present)(SDL_Renderer*);
    static int64_t origin, interval;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t nanos = now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
    if (!present) {
        present = (void (*)(SDL_Renderer*))dlsym(RTLD_NEXT, "SDL_RenderPresent");
        if (!present) abort();
        origin = nanos;
        interval = 1000000000 / atoi(getenv("LIVE_TEST_REFRESH_HZ"));
    }
    const int64_t next = origin + ((nanos - origin) / interval + 1) * interval;
    const struct timespec tick = {next / 1000000000, next % 1000000000};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tick, NULL);
    present(renderer);
}
