#include "app/race_launch_field.h"

#include "game/data/car_database.h"
#include "game/language_rules.h"
#include "menu/flow/game_hooks.h"
#include "race/field.h"
#include "race/pool_draw.h"
#include "vfs/vfs.h"

#include <algorithm>

namespace gt3r::race_screen {

std::string launch_event_code(const menu::flow::GameRequest& rq) {
    if (rq.course.size() == 4 && rq.course.rfind("tt", 0) == 0) return "AT" + rq.course.substr(2) + "0001";
    return rq.event.empty() ? rq.race_code : rq.event;
}

std::string launch_course(const Vfs& vfs, const menu::flow::GameRequest& rq, const data::Event* ev) {
    std::string course = rq.course;
    if ((course.empty() || course_file_path(vfs, course).empty()) && ev) course = ev->course;
    if (course.empty() || course_file_path(vfs, course).empty()) course = "trialmt";
    return course;
}

std::string launch_base_car(const data::CarDatabase& db, const std::string& id) {
    for (const std::string& l : {id, "_" + id}) {
        if (db.car(l)) return l;
        if (const data::CarVariant* v = db.variant(l)) return v->car;
        for (const data::CarVariant& v : db.arcade_cars())
            if (v.label == l && db.car(v.car)) return v.car;
    }
    return {};
}

std::string launch_player_car(const data::CarDatabase& db, const menu::flow::GameRequest& rq, const data::Event* ev) {
    std::string car = launch_base_car(db, rq.car);
    if (car.empty() && ev && !ev->entries.empty())
        car = launch_base_car(db, rq.mode == "go_license" ? game::license_car(db, *ev, db.language()).entry : ev->entries[0]);
    return car.empty() ? "r34_gtr_v_99" : car;
}

std::vector<std::string> launch_opponents(const data::CarDatabase& db, const menu::flow::GameRequest& rq,
                                          const data::Event* ev, bool online, GameRandom& rng,
                                          std::vector<u32>* color_ids) {
    if (!rq.opponents.empty()) return rq.opponents; // GT mode: the field drawn by the menus; Quick race's custom race
    // Arcade Single Race: the field is drawn from the event's pool with the race module's generator (gt_mode §20.4),
    // slots 1 .. cars-1.
    if (rq.mode == "single_race" && ev && !online)
        return race::draw_opponents(db, *ev, std::max(int(ev->cars_in_race), 1) - 1, rng, color_ids);
    return {};
}

int launch_max_opponents(const data::Event* ev, bool own_paints) {
    int n = 5;
    if (ev)
        if (const race::EventStart es = race::event_start(*ev); es.cars > 1) n = std::min(es.cars - 1, 5);
    if (own_paints) n = 5; // (a custom race: its own field, not the event's)
    return n;
}

race::PaintEntry opponent_paint_entry(const race::CarColors& colors, const std::string& model, size_t i,
                                      const std::vector<std::string>& field, const data::Event* ev,
                                      const std::vector<u32>& color_ids, const std::vector<int>& online_paints,
                                      const std::vector<int>& grid_paints) {
    race::PaintEntry e{model};
    if (ev && field == ev->entries) e.id = race::pool_color_id(*ev, int(i)); // the event's own field: slot i is pool entry i
    if (i < color_ids.size()) e.id = color_ids[i];
    if (i < online_paints.size()) e.id = colors.id_at(model, online_paints[i]); // the colour that player chose
    if (i < grid_paints.size() && grid_paints[i] >= 0) e.id = colors.id_at(model, grid_paints[i]); // the grid's colour
    return e;
}

} // namespace gt3r::race_screen
