#pragma once

// What the menus prepare for a race while their transition still runs (the race screen starts only at its end, and its
// load then did all of it, seconds on a slow device): the course's track and scene, the opponents' car files and their
// models. One RacePrep per chosen race: the menus start it (start_race_prep) and hand it to the App, the race screen's
// load takes it (App::take_race_prep), uses what matches its own choice, and lets the rest go with it.
//
// The opponents are worked out as the race screen will: the menus' field, or the arcade's draw from the event's pool
// with a copy of the race module's generator (race_info_random), so nothing it draws changes. A field, course or paint
// the race decides otherwise is built there as before.

#include "audio/engine_sound.h"
#include "base/bytes.h"
#include "base/game_random.h"
#include "formats/model/car_model.h"
#include "render/deferred_uploads.h"

#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gt3r {
class Vfs;
}
namespace gt3r::menu::flow {
struct GameRequest;
}
namespace gt3r::render {
class GpuContext;
}
namespace gt3r::race {
class CarVisual;
class CourseScene;
}
namespace gt3r::track {
class Track;
}

namespace gt3r::race_screen {

// The folder of the car models for a course's time of day, from its Env0 flags: "cars/eve/" (bit 19),
// "cars/night/" (bit 15), else "cars/day/".
std::string car_folder_for_course(ByteSpan course_file);

// An opponent's model built ahead, its GPU uploads still to run (on the GPU's thread, before it is drawn); no visual
// when it could not be built (`error`: why).
struct BuiltAhead {
    std::shared_ptr<race::CarVisual> visual;
    render::DeferredUploads uploads;
    std::string error;
};

// A car model to build on a worker thread (build_cars), from files read before (`rim`: the menus' rim,
// CarModelOptions::stand_in_rim). The menus' read-ahead and the race screen's load (load_opponent_cars) build with it.
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

class RacePrep {
public:
    ~RacePrep();

    // The course's label as the menus chose it (empty: nothing was prepared for it).
    std::string label;

    // The course's track (waits for it and its scene, built after it on the same thread: the race's load takes the scene
    // next; nullptr when it was not built).
    std::shared_ptr<const track::Track> take_track();
    // The course's scene, its uploads run (waits for it; nullptr when it was not built: sheen on only).
    std::unique_ptr<race::CourseScene> take_scene();
    // The car files read ahead, by path (nullptr: not read). Does not wait: a field still being read is read by the race.
    std::shared_ptr<const Bytes> file(const std::string& path) const;
    // An opponent's model built for this folder, model, paint and showroom setting (handed over once). Waits for the
    // models once the files are read (the race would otherwise build the same beside them); none while they are read.
    std::optional<BuiltAhead> take_built(const std::string& folder, const std::string& model, int paint,
                                         bool showroom_wheels);
    // The opponents' engine banks and the wind bank read ahead (nullptr: not yet, or none). Does not wait.
    const EngineBanks* banks() const;
    // Runs the GPU uploads of what was built ahead (the course's scene first, then the opponents' models) for about
    // `budget_ms` (at least one; most are 1-2 ms, the largest 30-60): the menus and the race screen call it on the frames
    // they spend waiting for the race's load (its transition's last ones, the screen nearly covered), so the load has
    // fewer to run. False when none was left.
    bool pump_upload(double budget_ms = 8);
    // Lets the files and models not taken go (the end of a race's load); the work still running ends on its own, and
    // file() and take_built() hand nothing over from then on.
    void drop();
    bool dropped = false;

    // Each from a thread of its own (std::async: empty when no thread could start; nullptr when nothing was made).
    struct CourseAhead;
    struct FieldFiles;
    using FieldModels = std::map<std::string, BuiltAhead>; // by built_key
    std::shared_future<std::shared_ptr<CourseAhead>> course;      // its track and scene
    std::shared_future<std::shared_ptr<const FieldFiles>> files; // the opponents' files and the models to build
    std::shared_future<std::shared_ptr<FieldModels>> cars;       // those models (waits for the files)
};

// Starts preparing the race `rq` on three threads: its course (when the request names a course file), its field's
// files, and the models built from them. `rng`: a copy of race_info_random() taken when the race was chosen. Without
// `gpu` nothing is built, only read. `showroom_wheels`: App::showroom_wheels().
std::unique_ptr<RacePrep> start_race_prep(const Vfs& vfs, render::GpuContext* gpu, bool showroom_wheels,
                                          const menu::flow::GameRequest& rq, GameRandom rng);

} // namespace gt3r::race_screen
