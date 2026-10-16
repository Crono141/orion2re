#include "pch.h"

#include "doctest/doctest.h"

namespace {
    s_player players[MAX_PLAYERS];
    s_planet_data planets[MAX_PLANETS];
    s_star_data stars[MAX_STARS];
    s_colony colonies[2];
    s_ship_data ships[2];
    s_scroll_bar scroll;
    s_plntsum_planet summary[MAX_PLANETS];
    s_plntsum_planet temporary[MAX_PLANETS];
    bool allocation_pushed;
} // namespace

namespace MOX {
    s_player* player = players;
    s_planet_data* planet = planets;
    s_star_data* star = stars;
    s_colony* colony = colonies;
    s_ship_data* ship = ships;
    s_leader_data* leaders = nullptr;
    s_scroll_bar* scroll_bar = &scroll;
    s_animation_header* screen_seg = nullptr;
    int16_t PLAYER_NUM;
    int16_t planet_count;
    int16_t scanned_field;
    int16_t ship_stack_count;
    int16_t ship_stack_start[MAX_SHIPS];
    s_ship_node ship_node[MAX_SHIPS];
} // namespace MOX

namespace TEST_PLNTSUM {
    bool in_range[MAX_STARS];

    void Reset_() {
        memset(players, 0, sizeof(players));
        memset(planets, 0, sizeof(planets));
        memset(stars, 0, sizeof(stars));
        memset(colonies, 0, sizeof(colonies));
        memset(ships, 0, sizeof(ships));
        memset(&scroll, 0, sizeof(scroll));
        memset(summary, 0xFF, sizeof(summary));
        memset(MOX::ship_stack_start, 0xFF, sizeof(MOX::ship_stack_start));
        memset(MOX::ship_node, 0xFF, sizeof(MOX::ship_node));
        for (auto& planet : planets) {
            planet.colony_index = -1;
            planet.planet_type = PLANET_TYPE_ASTEROID;
        }
        for (auto& star : stars) {
            memset(star.officer_index, 0xFF, sizeof(star.officer_index));
            for (int orbit = 0; orbit < 5; ++orbit) {
                star.planet_index[orbit] = -1;
            }
        }
        for (bool& reachable : in_range) {
            reachable = true;
        }
        MOX::PLAYER_NUM = 0;
        MOX::planet_count = 0;
        MOX::ship_stack_count = 0;
        MOX::scanned_field = -1;
        PLNTSUM::planet_summary = summary;
        PLNTSUM::sort_choice = 0;
        PLNTSUM::filter_out_enemy_controlled_planets = 0;
        PLNTSUM::filter_out_planets_out_of_range = 0;
        PLNTSUM::filter_out_mineral_scarcity_planets = 0;
        PLNTSUM::filter_out_adverse_gravity_planets = 0;
        PLNTSUM::filter_out_hostile_environment_planets = 0;
        PLNTSUM::Reset_Explored_Planets();
        DOCTEST_REQUIRE_FALSE(allocation_pushed);
    }
} // namespace TEST_PLNTSUM

namespace allocate {
    void __cdecl Push_Block(void*) {
        DOCTEST_REQUIRE_FALSE(allocation_pushed);
        allocation_pushed = true;
    }

    void* __cdecl Allocate_Next_Block(void*, uint32_t size) {
        DOCTEST_REQUIRE(allocation_pushed);
        DOCTEST_REQUIRE(size <= sizeof(temporary));
        return temporary;
    }

    void __cdecl Pop_Block(void*) {
        DOCTEST_REQUIRE(allocation_pushed);
        allocation_pushed = false;
    }
} // namespace allocate

namespace RUSS {
    void __cdecl Mox_Update() {}
} // namespace RUSS

namespace OFFICER {
    void __cdecl Move_Officer_To_Pool(int16_t) {
        DOCTEST_FAIL("Unexpected officer in colony destruction fixture");
    }

    void __cdecl Kill_Officer(int16_t) {
        DOCTEST_FAIL("Unexpected officer in colony destruction fixture");
    }

    bool __cdecl One_Leader_With_Galactic_Lore(int16_t player_idx) {
        return MOX::player[player_idx].has_galactic_lore != 0;
    }
} // namespace OFFICER

namespace SHIPMOVE {
    bool __cdecl Star_In_Extended_Range_Of_Player(s_star_data* star, int16_t) {
        return TEST_PLNTSUM::in_range[star - MOX::star];
    }
} // namespace SHIPMOVE

namespace HACCESS {
    int16_t __cdecl Ship_Owner(int16_t ship_idx) {
        return MOX::ship[ship_idx].owner;
    }
} // namespace HACCESS

namespace COLCALC {
    void __cdecl Compute_Star_Colony_Stuff(s_star_data*) {}

    uint8_t minerals_per_mine[8] = {1, 2, 3, 5, 8};

    int16_t __cdecl Planet_Max_Population_For_Player(s_planet_data*, int16_t) {
        return 10;
    }
} // namespace COLCALC

namespace EVENTS {
    void __cdecl Kill_All_Events_At_Colony(int16_t) {}
} // namespace EVENTS

namespace COLBLDG {
    void __cdecl Clear_Colony_Queue(int16_t) {}
} // namespace COLBLDG

namespace MSG_ {
    void __cdecl Add_Msg(s_MSG, int16_t) {}
} // namespace MSG_

namespace invasion {
    void __cdecl Assign_New_Home_Planet(int8_t, int16_t) {
        DOCTEST_FAIL("Unexpected homeworld in colony destruction fixture");
    }
} // namespace invasion