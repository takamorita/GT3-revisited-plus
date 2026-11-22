#pragma once

// Car models built on worker threads (build_cars), their GPU uploads left for the GPU's thread.

#include "base/bytes.h"
#include "formats/model/car_model.h"
#include "render/deferred_uploads.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gt3r::render {
class GpuContext;
}
namespace gt3r::race {
class CarVisual;
}

namespace gt3r::race_screen {

// An opponent's model built ahead, its GPU uploads still to run (on the GPU's thread, before it is drawn); no visual
// when it could not be built (`error`: why).
struct BuiltAhead {
    std::shared_ptr<race::CarVisual> visual;
    render::DeferredUploads uploads;
    std::string error;
};

// A car model to build on a worker thread (build_cars), from files read before (`rim`: the menus' rim,
// CarModelOptions::stand_in_rim). The race screen's load (load_opponent_cars) builds with it.
struct CarBuild {
    std::string model;
    int paint = 0;
    WheelSpec ws;
    CarBodyMod bm;
    std::shared_ptr<const Bytes> car, showroom_car;
    std::optional<Bytes> wheel, showroom_wheel, rim;
};
// The models of `builds`, in their order, built on this thread and up to three more (an index taken in turn), their GPU
// uploads left for the GPU's thread (render::DeferredUploads).
std::vector<BuiltAhead> build_cars(render::GpuContext& gpu, const std::vector<CarBuild>& builds);

} // namespace gt3r::race_screen
