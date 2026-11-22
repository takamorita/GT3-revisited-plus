#include "app/race_prep.h"

#include "app/race_launch_field.h"

#include "base/log.h"
#include "formats/course/course_file.h"
#include "formats/model/car_model.h"
#include "formats/model/wheel_spec.h"
#include "game/data/car_database.h"
#include "menu/flow/game_hooks.h"
#include "race/exact_setup.h"
#include "race/paint.h"
#include "race/race_scene.h"
#include "track/track.h"
#include "vfs/vfs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <system_error>
#include <thread>
#include <vector>

namespace gt3r::race_screen {

struct RacePrep::CourseAhead {
    std::shared_ptr<const track::Track> track;
    std::unique_ptr<race::CourseScene> scene;
    render::DeferredUploads uploads; // the scene's, not run yet
};

struct RacePrep::FieldFiles {
    std::map<std::string, std::shared_ptr<const Bytes>> files; // by path
    EngineBanks banks;                                         // the opponents' engine banks and the wind bank
    std::map<std::string, CarBuild> builds;                    // the models to build, by built_key (rims not read yet)
};

namespace {

std::string built_key(const std::string& folder, const std::string& model, int paint, bool showroom_wheels) {
    return folder + model + "|" + std::to_string(paint) + (showroom_wheels ? "|s" : "|");
}

template <class F> bool ready(const F& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// `job` on a thread of its own (its result nullptr, nothing prepared, when it throws). No thread (out of them): an empty
// future, and the race screen does that part itself.
template <class F> auto on_thread(const char* what, F job) -> std::shared_future<decltype(job())> {
    try {
        return std::async(std::launch::async, [job = std::move(job)]() -> decltype(job()) {
                   try {
                       return job();
                   } catch (...) {
                       return nullptr;
                   }
               }).share();
    } catch (const std::system_error& e) {
        LOG_WARN("Race prep: no thread for the %s (%s)", what, e.what());
        return {};
    }
}

// The course: its exact data (race::exact_course keeps it for the race's set-up), its track and its scene (all but the
// GPU uploads).
std::shared_ptr<RacePrep::CourseAhead> prepare_course(const Vfs& vfs, render::GpuContext* gpu, const std::string& path,
                                                      const std::string& label) {
    std::shared_ptr<const sim::exact::ProgramData> d = race::exact_program_data(vfs);
    if (!d) return nullptr;
    std::optional<Bytes> file = vfs.read(path);
    if (!file) return nullptr;
    race::exact_course(*file, label, *d);
    auto c = std::make_shared<RacePrep::CourseAhead>();
    try {
        c->track = track::Track::from_course_file(*file, label);
    } catch (const std::exception&) {
        // the race's own build reports it
    }
    if (!gpu) return c;
    try {
        render::DeferredUploads::Scope scope(c->uploads);
        c->scene = std::make_unique<race::CourseScene>(*gpu, *file, label, true);
    } catch (const std::exception& e) {
        LOG_WARN("Course %s: not built ahead: %s", label.c_str(), e.what());
    }
    return c;
}

// The field: its opponents' car files read and (`build`: with a GPU) the models to build from them.
std::shared_ptr<const RacePrep::FieldFiles> prepare_field(const Vfs& vfs, bool build, bool showroom_wheels,
                                                          const menu::flow::GameRequest& rq, GameRandom rng) {
    data::CarDatabase db;
    if (!db.load(vfs)) return nullptr;
    // The field as the race screen decides it (race_launch_field.h): its opponents (else the event's own entries, as
    // the race field takes them), cut to the race's count.
    const data::Event* ev = db.event(launch_event_code(rq));
    const bool race = (rq.mode == "single_race" || rq.mode == "go_race") && ev;
    std::vector<u32> color_ids;
    std::vector<std::string> labels = launch_opponents(db, rq, ev, false, rng, &color_ids);
    if (labels.empty() && race) labels = ev->entries;
    const int max_opp = launch_max_opponents(race ? ev : nullptr, !rq.opponent_paints.empty());
    if (int(labels.size()) > max_opp) labels.resize(size_t(std::max(0, max_opp)));
    if (labels.empty()) return nullptr;
    const std::string course_file = course_file_path(vfs, launch_course(vfs, rq, ev));
    std::string folder = "cars/day/";
    if (!course_file.empty())
        if (std::optional<Bytes> c = vfs.read(course_file)) folder = car_folder_for_course(*c);
    // Each opponent's model in field order (RaceField::opponent_model), and its engine banks (OpponentEngines::load's).
    auto files = std::make_shared<RacePrep::FieldFiles>();
    std::vector<std::string> field_models, models;
    for (const std::string& l : labels) {
        const data::Car* c = db.car(race::exact_base_car(db, l));
        const std::string m = c ? db.model_of(*c) : "";
        field_models.push_back(m);
        if (!m.empty() && std::find(models.begin(), models.end(), m) == models.end()) models.push_back(m);
        if (c) {
            const data::CarDatabase::EngineSound es = db.engine_sound(*c);
            if (es.id) EngineSound::read_banks(vfs, es.id, es.variant, files->banks);
        }
    }
    EngineSound::read_bank(vfs, EngineUnit::kWindBank, files->banks);
    auto keep = [&](const std::string& path, std::optional<Bytes> bytes) {
        if (!bytes) return false;
        files->files[path] = std::make_shared<const Bytes>(std::move(*bytes));
        return true;
    };
    // (RaceScreen::read_car: the course's folder, else the day folder; read_showroom: the menus' model.)
    for (const std::string& m : models) {
        bool have = false;
        if (folder != "cars/day/")
            if (std::optional<Bytes> f = vfs.read(folder + m); f && !f->empty()) have = keep(folder + m, std::move(f));
        if (!have) keep("cars/day/" + m, vfs.read("cars/day/" + m));
        keep("menu/cars/" + m, vfs.read("menu/cars/" + m));
    }
    LOG_INFO("Race: %zu opponents' files read ahead (%zu models)", files->files.size(), models.size());
    if (!build) return files;

    // Their paints as RaceScreen::load_opponent_cars draws them: the player first (the menus' car and colour), then each
    // opponent (its pool colour, the menus' paint), the unset ones drawn in order with the generator as the field draw
    // left it.
    race::CarColors colors;
    if (!colors.load(vfs)) return files;
    std::string player_model = rq.car_model;
    if (player_model.empty())
        if (const data::Car* c = db.car(launch_player_car(db, rq, ev))) player_model = db.model_of(*c);
    std::vector<race::PaintEntry> entries;
    entries.push_back({player_model, colors.id_at(player_model, std::max(0, rq.car_color))});
    for (size_t i = 0; i < field_models.size(); ++i)
        entries.push_back(opponent_paint_entry(colors, field_models[i], i, labels, ev, color_ids, {}, rq.opponent_paints));
    const std::vector<int> paints = race::assign_paints(colors, entries, rng);
    // The models to build, as load_opponent_cars builds them (mod cars' own bodies are left to it).
    auto file = [&files](const std::string& p) -> std::shared_ptr<const Bytes> {
        auto it = files->files.find(p);
        return it == files->files.end() ? nullptr : it->second;
    };
    for (size_t i = 0; i < field_models.size(); ++i) {
        const std::string& m = field_models[i];
        const int paint = std::max(0, i + 1 < paints.size() ? paints[i + 1] : 0);
        if (m.empty() || db.mod_car(race::exact_base_car(db, labels[i]))) continue;
        const std::string key = built_key(folder, m, paint, showroom_wheels);
        if (files->builds.count(key)) continue;
        CarBuild b;
        b.model = m, b.paint = paint;
        b.car = file(folder + m);
        if (!b.car) b.car = file("cars/day/" + m);
        if (showroom_wheels) b.showroom_car = file("menu/cars/" + m);
        if (!b.car || b.car->empty()) continue;
        if (auto w = wheel_spec_from_database(db, m)) b.ws = *w;
        files->builds[key] = std::move(b);
    }
    return files;
}

// The field's models (FieldFiles::builds) built on up to four threads, their rims read first.
std::shared_ptr<RacePrep::FieldModels> build_field(const Vfs& vfs, render::GpuContext& gpu,
                                                   const RacePrep::FieldFiles& f) {
    std::vector<std::string> keys;
    std::vector<CarBuild> builds;
    for (const auto& [key, b] : f.builds) {
        keys.push_back(key), builds.push_back(b);
        builds.back().rim = menu_rim([&vfs](const std::string& p) { return vfs.read(p); }, b.model, "");
    }
    std::vector<BuiltAhead> out = build_cars(gpu, builds);
    auto cars = std::make_shared<RacePrep::FieldModels>();
    for (size_t k = 0; k < builds.size(); ++k)
        if (out[k].visual) (*cars)[keys[k]] = std::move(out[k]);
        else LOG_WARN("Race: %s not built ahead: %s", builds[k].model.c_str(), out[k].error.c_str());
    LOG_INFO("Race: %zu opponents' models built ahead", cars->size());
    return cars;
}

// The course built ahead (waits for it; the wait logged).
std::shared_ptr<RacePrep::CourseAhead> wait_course(const std::shared_future<std::shared_ptr<RacePrep::CourseAhead>>& course,
                                                   const std::string& label) {
    const Uint64 w0 = SDL_GetTicksNS();
    const bool waited = !ready(course);
    std::shared_ptr<RacePrep::CourseAhead> c = course.get();
    if (waited) LOG_INFO("Course %s: waited %.0f ms for the build ahead", label.c_str(), double(SDL_GetTicksNS() - w0) / 1e6);
    return c;
}

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

std::string car_folder_for_course(ByteSpan course) {
    try {
        const course::CourseFile cf = course::CourseFile::parse(course);
        const ByteSpan env = cf.section(course, course::kSlotEnv);
        if (env.size() > 0x14 && std::memcmp(env.data(), "Env0", 4) == 0) {
            const u32 fl = rd32(env, 0x10);
            return (fl & 0x80000) ? "cars/eve/" : (fl & 0x8000) ? "cars/night/" : "cars/day/";
        }
    } catch (const std::exception&) {
    }
    return "cars/day/";
}

std::unique_ptr<RacePrep> start_race_prep(const Vfs& vfs, render::GpuContext* gpu, bool showroom_wheels,
                                          const menu::flow::GameRequest& rq, GameRandom rng) {
    auto prep = std::make_unique<RacePrep>();
    // The course as the race screen picks it when the menu's id names a course file (screen_race_launch.cpp); otherwise
    // nothing is done ahead for it.
    if (std::string path = rq.course.empty() ? "" : course_file_path(vfs, rq.course); !path.empty()) {
        prep->label = rq.course;
        prep->course = on_thread("course", [&vfs, gpu, path, label = rq.course] {
            return prepare_course(vfs, gpu, path, label);
        });
    }
    prep->files = on_thread("field", [&vfs, build = gpu != nullptr, showroom_wheels, rq, rng] {
        return prepare_field(vfs, build, showroom_wheels, rq, rng);
    });
    if (gpu && prep->files.valid())
        prep->cars = on_thread("models", [&vfs, gpu, files = prep->files]() -> std::shared_ptr<RacePrep::FieldModels> {
            const std::shared_ptr<const RacePrep::FieldFiles>& f = files.get();
            return f ? build_field(vfs, *gpu, *f) : nullptr;
        });
    return prep;
}

RacePrep::~RacePrep() {
    // The threads first (they write into what drop lets go), then what they made, here on the GPU's thread.
    if (course.valid()) course.wait();
    if (files.valid()) files.wait();
    if (cars.valid()) cars.wait();
    drop();
}

std::shared_ptr<const track::Track> RacePrep::take_track() {
    if (!course.valid()) return nullptr;
    const std::shared_ptr<CourseAhead> c = wait_course(course, label);
    return c ? c->track : nullptr;
}

std::unique_ptr<race::CourseScene> RacePrep::take_scene() {
    if (!course.valid()) return nullptr;
    const std::shared_ptr<CourseAhead> c = wait_course(course, label);
    if (!c || !c->scene) return nullptr;
    c->uploads.run();
    LOG_INFO("Course %s: built ahead", label.c_str());
    return std::move(c->scene);
}

std::shared_ptr<const Bytes> RacePrep::file(const std::string& path) const {
    if (dropped || !ready(files)) return nullptr;
    const std::shared_ptr<const FieldFiles>& f = files.get();
    if (!f) return nullptr;
    auto it = f->files.find(path);
    return it == f->files.end() ? nullptr : it->second;
}

std::optional<BuiltAhead> RacePrep::take_built(const std::string& folder, const std::string& model, int paint,
                                               bool showroom) {
    if (dropped || !ready(files) || !cars.valid()) return std::nullopt;
    // (Not one of the models being built: the race builds it.)
    const std::string key = built_key(folder, model, paint, showroom);
    if (!files.get() || !files.get()->builds.count(key)) return std::nullopt;
    if (!ready(cars)) {
        const Uint64 w0 = SDL_GetTicksNS();
        cars.wait();
        LOG_INFO("Race: waited %.0f ms for %s built ahead", double(SDL_GetTicksNS() - w0) / 1e6, model.c_str());
    }
    const std::shared_ptr<FieldModels>& m = cars.get();
    if (!m) return std::nullopt;
    auto it = m->find(key);
    if (it == m->end()) return std::nullopt;
    BuiltAhead b = std::move(it->second);
    m->erase(it);
    return b;
}

bool RacePrep::pump_upload(double budget_ms) {
    const Uint64 t0 = SDL_GetTicksNS();
    bool any = false;
    auto one = [this] {
        if (ready(course))
            if (const std::shared_ptr<CourseAhead>& c = course.get(); c && c->scene && c->uploads.run_one()) return true;
        if (ready(cars))
            if (const std::shared_ptr<FieldModels>& m = cars.get())
                for (auto& [key, b] : *m)
                    if (b.uploads.run_one()) return true;
        return false;
    };
    while (double(SDL_GetTicksNS() - t0) / 1e6 < budget_ms && one()) any = true;
    return any;
}

void RacePrep::drop() {
    // The models here, on the GPU's thread (their futures' states may go last on a thread that made them); what is still
    // being made is let go with the prep (its threads write into it; dropping the last reference to an unfinished
    // std::async future would wait for it).
    dropped = true;
    if (ready(course)) {
        if (const std::shared_ptr<CourseAhead>& c = course.get()) c->scene.reset();
        course = {};
    }
    if (ready(files)) files = {};
    if (ready(cars)) {
        if (const std::shared_ptr<FieldModels>& m = cars.get()) m->clear();
        cars = {};
    }
}

const EngineBanks* RacePrep::banks() const {
    if (!ready(files)) return nullptr;
    const std::shared_ptr<const FieldFiles>& f = files.get();
    return f ? &f->banks : nullptr;
}

} // namespace gt3r::race_screen
