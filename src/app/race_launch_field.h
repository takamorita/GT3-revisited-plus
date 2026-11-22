#pragma once

// What a race launched from the menus races against, decided in one place: the race screen's launch
// (RaceScreen::resolve_launch, build_vehicle, load_opponent_cars) and the menus' read-ahead
// (race_prep.h, which works it out with a copy of the race module's generator) both call these,
// so the read-ahead cannot drift from what the race does.

#include "base/bytes.h"
#include "base/game_random.h"
#include "race/paint.h"

#include <string>
#include <vector>

namespace gt3r {
class Vfs;
}
namespace gt3r::data {
class CarDatabase;
struct Event;
} // namespace gt3r::data
namespace gt3r::menu::flow {
struct GameRequest;
}

namespace gt3r::race_screen {

// The race code a request names: its event, else its race code; the arcade time trial's courses tt01..tt10 are the
// time-attack events AT010001..AT100001.
std::string launch_event_code(const menu::flow::GameRequest& rq);

// The course a launch races on: the menus' choice, else the event's course, else Trial Mountain.
std::string launch_course(const Vfs& vfs, const menu::flow::GameRequest& rq, const data::Event* ev);

// A menu car id as a database car: the label (with or without its leading '_'), a variant's or an arcade car's base
// car; empty when none.
std::string launch_base_car(const data::CarDatabase& db, const std::string& id);

// The player's car: the menus' choice, else the event's entry 0 (a licence test's licence car), else the default.
std::string launch_player_car(const data::CarDatabase& db, const menu::flow::GameRequest& rq, const data::Event* ev);

// The opponents of a launch: the menus' own field (GT mode, Quick race), or the arcade single race's draw from the
// event's pool with `rng` (`color_ids` then gets each one's pool colour, race::draw_opponents). Empty for launches that
// have no such field.
std::vector<std::string> launch_opponents(const data::CarDatabase& db, const menu::flow::GameRequest& rq,
                                          const data::Event* ev, bool online, GameRandom& rng,
                                          std::vector<u32>* color_ids);

// How many opponents the event's race takes (FieldConfig::max_opponents): its cars less the player, at most 5; a field
// with its own paints (a custom race) 5.
int launch_max_opponents(const data::Event* ev, bool own_paints);

// Opponent i's paint entry (load_opponent_cars): its pool colour when the field is the event's own, its draw's colour,
// the colour an online player chose, the colour the grid gives it; else unset (drawn).
race::PaintEntry opponent_paint_entry(const race::CarColors& colors, const std::string& model, size_t i,
                                      const std::vector<std::string>& field, const data::Event* ev,
                                      const std::vector<u32>& color_ids, const std::vector<int>& online_paints,
                                      const std::vector<int>& grid_paints);

} // namespace gt3r::race_screen
