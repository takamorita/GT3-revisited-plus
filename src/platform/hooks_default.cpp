// The platform hooks (platform/hooks.h) on the platforms without their own (platform/web/): nothing.
// background_thread also in a browser (the tests link it, without the web hooks' App).

#include "platform/hooks.h"

namespace gt3r::platform {

void background_thread() {}

#if !defined(__EMSCRIPTEN__)
void on_start() {}
void on_app_created(App&) {}
void on_quit() {}

double gpu_wait_ms(double present_ms, double, double) { return present_ms; }

void on_race_start() {}
#endif

} // namespace gt3r::platform
