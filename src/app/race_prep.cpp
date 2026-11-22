#include "app/race_prep.h"

#include "formats/model/car_model.h"
#include "race/race_scene.h"

#include <algorithm>
#include <atomic>
#include <system_error>
#include <thread>
#include <vector>

namespace gt3r::race_screen {

namespace {

// `b`'s model, its GPU uploads left in its BuiltAhead.
BuiltAhead build_car(render::GpuContext& gpu, const CarBuild& b) {
    BuiltAhead out;
    render::DeferredUploads::Scope scope(out.uploads);
    try {
        out.visual = std::make_shared<race::CarVisual>(
            gpu, *b.car, b.model, b.ws, b.paint, b.wheel ? ByteSpan(*b.wheel) : ByteSpan{}, b.bm,
            b.showroom_car ? ByteSpan(*b.showroom_car) : ByteSpan{},
            b.showroom_wheel ? ByteSpan(*b.showroom_wheel) : ByteSpan{}, [rim = b.rim] { return rim; });
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

} // namespace

std::vector<BuiltAhead> build_cars(render::GpuContext& gpu, const std::vector<CarBuild>& builds) {
    std::vector<BuiltAhead> out(builds.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t k; (k = next.fetch_add(1)) < builds.size();) out[k] = build_car(gpu, builds[k]);
    };
    std::vector<std::thread> helpers;
    for (size_t t = 1; t < std::min<size_t>(builds.size(), 4); ++t) {
        try {
            helpers.emplace_back(work);
        } catch (const std::system_error&) {
            break; // (no more threads: this one and those started do the rest)
        }
    }
    work();
    for (std::thread& t : helpers) t.join();
    return out;
}

} // namespace gt3r::race_screen
