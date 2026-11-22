// The platform hooks (platform/hooks.h) in a browser (background_thread: hooks_default.cpp, which the tests link
// without the App this needs), the page's debug overlay and its file picker (platform/ios_files.h).

#ifdef __EMSCRIPTEN__

#include "platform/hooks.h"
#include "platform/ios_files.h"

#include "app/app.h"
#include "render/dynamic_scale.h"
#include "render/gpu_context.h"

#include <emscripten.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

// The page's debug overlay (web/debug.js) reads and changes the 3D settings here. Same steps
// as the Settings screen (config, apply, save), so a change persists like one made there.
static gt3r::App* g_web_app = nullptr;

extern "C" {
// What is in effect, as JSON: {"msaa":2,"levels":[1,2,4],"aniso":4,"scaleSetting":"auto","scale":1.00,
// "dynamic":false,"dynScale":0.00}. "scale" is the 3D scale now (the dynamic one while it is on).
EMSCRIPTEN_KEEPALIVE const char* gt3r_web_debug_get() {
    static std::string json;
    gt3r::App* a = g_web_app;
    if (!a || !a->gpu()) return "{}";
    gt3r::render::GpuContext* g = a->gpu();
    std::string levels;
    for (int l : g->msaa_levels()) levels += (levels.empty() ? "" : ",") + std::to_string(l);
    const gt3r::render::DynamicScale* d = a->dynamic_scale();
    int vsync = 0;
    if (a->renderer()) SDL_GetRenderVSync(a->renderer(), &vsync);
    char buf[360];
    std::snprintf(buf, sizeof buf,
                  "{\"msaa\":%d,\"levels\":[%s],\"aniso\":%d,\"scaleSetting\":\"%s\",\"scale\":%.2f,"
                  "\"dynamic\":%s,\"dynScale\":%.2f,\"vsync\":%d}",
                  g->msaa(), levels.c_str(), g->anisotropy(),
                  a->config().get("video", "render_scale").value_or("auto").c_str(), double(g->render_scale()),
                  a->dynamic_resolution() ? "true" : "false", d ? double(d->scale()) : 0.0, vsync);
    json = buf;
    return json.c_str();
}

// key: "msaa" (1, 2, 4, 8), "anisotropy" (1..16), "render_scale" ("auto" or 0.5..4), "dynamic" ("on"/"off"),
// "vsync" (1/0).
// Returns 1 when applied.
EMSCRIPTEN_KEEPALIVE int gt3r_web_debug_set(const char* key, const char* value) {
    gt3r::App* a = g_web_app;
    if (!a || !a->gpu() || !key || !value) return 0;
    const std::string k = key, v = value;
    if (k == "msaa") {
        const int n = std::atoi(value);
        a->config().set("video", "msaa", std::to_string(n));
        a->gpu()->set_msaa(n);
        a->save_config();
        return 1;
    }
    if (k == "anisotropy") {
        const int n = std::atoi(value);
        a->config().set("video", "anisotropy", std::to_string(n));
        a->gpu()->set_anisotropy(n);
        a->save_config();
        return 1;
    }
    if (k == "render_scale") {
        a->config().set("video", "render_scale", v);
        a->apply_render_scale();
        a->save_config();
        return 1;
    }
    if (k == "dynamic") {
        a->set_dynamic_resolution(v == "on");
        return 1;
    }
    if (k == "vsync" && a->renderer()) { // as the Settings screen
        a->config().set("video", "vsync", v == "0" ? "0" : "1");
        SDL_SetRenderVSync(a->renderer(), v == "0" ? 0 : 1);
        a->save_config();
        return 1;
    }
    return 0;
}
}

namespace gt3r::platform {

void on_start() {}

void on_app_created(App& app) { g_web_app = &app; }

// SDL_RenderPresent waits for nothing in a browser: a GPU that cannot keep up shows as time between
// iterations (the frame lands on a later requestAnimationFrame), not as present time, and the
// controller lowers the scale only for frames that wait on the GPU. Count what the frame took
// beyond the work of this iteration as waiting. (A frame that was slow in its own work is not.)
double gpu_wait_ms(double present_ms, double frame_ms, double work_ms) { return std::max(present_ms, frame_ms - work_ms); }

void on_race_start() {}

void on_quit() {
    // The page stays when the game quits (Quit in a menu): it saves the user data and says so
    // (web/start.js) rather than leaving a black canvas.
    EM_ASM({ if (Module.gt3rQuit) Module.gt3rQuit(); });
}

// Web: the page's file picker (web/start.js, Module.gt3rPickFile). The picked file is read
// where it is, as the disc image is (cmake/web/browser_pre.js): the page puts it at
// /webdisc/<name> and calls gt3r_web_picked with that path, or with "" when cancelled.
namespace {
std::function<void(std::string)> g_web_picked;
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void gt3r_web_picked(const char* path) {
    auto done = std::move(g_web_picked);
    g_web_picked = nullptr;
    if (done) done(path ? path : "");
}

// accept: the filters' extensions as ".iso,.chd" ("" = any file).
EM_JS(int, gt3r_web_pick_file, (const char* accept), {
    if (!Module.gt3rPickFile) return 0;
    Module.gt3rPickFile(UTF8ToString(accept));
    return 1;
});

bool show_document_picker(SDL_Window* window, const SDL_DialogFileFilter* filters, int count,
                          std::function<void(std::string path)> done) {
    (void)window;
    // The filters' extensions; "*" ("All files") is left out, as the browser's picker offers
    // every file next to the listed types anyway.
    std::string accept;
    for (int i = 0; i < count; ++i) {
        std::string_view pattern = filters[i].pattern ? filters[i].pattern : "";
        while (!pattern.empty()) {
            const size_t end = std::min(pattern.find(';'), pattern.size());
            const std::string_view ext = pattern.substr(0, end);
            if (!ext.empty() && ext != "*") accept += (accept.empty() ? "." : ",.") + std::string(ext);
            pattern.remove_prefix(std::min(end + 1, pattern.size()));
        }
    }
    g_web_picked = std::move(done);
    if (gt3r_web_pick_file(accept.c_str())) return true;
    g_web_picked = nullptr;
    return false;
}

} // namespace gt3r::platform

#endif
