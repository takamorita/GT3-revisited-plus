#pragma once

// What a platform (the browser: platform/web/) does differently from the others, in one place. The default
// (hooks_default.cpp) does nothing; each platform's own file compiles only there.

namespace gt3r {
class App;
}

namespace gt3r::platform {

// The start and the end (app/main.cpp): first in SDL_AppInit, before anything else; once the App exists; last in
// SDL_AppQuit, after the App is gone.
void on_start();
void on_app_created(App& app);
void on_quit();

// Called first in a worker thread (reading ahead, building ahead, decoding): a platform may place its workers here
// (where a thread starts on its creator's core, a worker takes the main thread's time). Nothing elsewhere.
void background_thread();

// The time this frame waited for the GPU, for the dynamic resolution: `present_ms` in SDL_RenderPresent, the frame
// took `frame_ms`, `work_ms` of which this iteration's own work. Called every frame after the present, also while the
// dynamic resolution is off. The default: the present.
double gpu_wait_ms(double present_ms, double frame_ms, double work_ms);

// A live run starts (RaceScreen::restart: the race has loaded, or Restart / Retry; not a replay): a platform may
// raise its clocks for the run's first seconds (the grid, the countdown's close-ups and dissolves draw the most).
// Nothing elsewhere.
void on_race_start();

} // namespace gt3r::platform
