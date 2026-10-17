#include "pch.h"

#include <algorithm>
#include <vector>

#include "doctest/doctest.h"

namespace TEST_PLNTSUM {
    extern bool in_range[MAX_STARS];
    void Reset_();
} // namespace TEST_PLNTSUM

namespace {
    void Add_System(int16_t star_idx, int16_t first_planet, int rotation = 0) {
        MOX::star[star_idx].visited = 1 << MOX::PLAYER_NUM;
        for (int orbit = 0; orbit < 5; ++orbit) {
            int16_t planet_idx = first_planet + orbit;
            auto& planet = MOX::planet[planet_idx];
            planet.star_index = star_idx;
            planet.planet_type = PLANET_TYPE_PLANET;
            planet.mineral_class = (orbit + rotation) % 5;
            planet.gravity_class = PLANET_GRAVITY_NORMAL_G;
            planet.food_per_farmer = 2;
            MOX::star[star_idx].planet_index[orbit] = planet_idx;
        }
        MOX::planet_count = first_planet + 5;
    }

    void Set_Filters(int mask) {
        PLNTSUM::filter_out_enemy_controlled_planets = (mask & 1) != 0;
        PLNTSUM::filter_out_planets_out_of_range = (mask & 2) != 0;
        PLNTSUM::filter_out_mineral_scarcity_planets = (mask & 4) != 0;
    }

    void Check_List(std::vector<int16_t> expected) {
        PLNTSUM::Filter_Explored_Planets(&MOX::scroll_bar->total_rows);
        std::vector<int16_t> actual;
        for (int i = 0; i < MOX::scroll_bar->total_rows; ++i) {
            actual.push_back(PLNTSUM::planet_summary[i].explored);
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        DOCTEST_CHECK(actual == expected);
    }

    void Add_Ship(int16_t owner, int16_t location) {
        MOX::ship[0].owner = owner;
        MOX::ship[0].location = location;
        MOX::ship[0].status = 0;
        MOX::ship_stack_count = 1;
        MOX::ship_stack_start[0] = 0;
        MOX::ship_node[0].ship_idx = 0;
        MOX::ship_node[0].next_node = -1;
    }
} // namespace

DOCTEST_TEST_SUITE_BEGIN("planet summary filters");

DOCTEST_TEST_CASE("mixed minerals remain planet-local across filter permutations") {
    // Exercise each toggle order in both directions without resetting the displayed list.
    const int orders[][3] = {{1, 2, 4}, {1, 4, 2}, {2, 1, 4}, {2, 4, 1}, {4, 1, 2}, {4, 2, 1}};
    for (int rotation = 0; rotation < 5; ++rotation) {
        for (int sort = 0; sort < 3; ++sort) {
            for (int knowledge = 0; knowledge < 4; ++knowledge) {
                for (bool reachable : {false, true}) {
                    for (int presence = 0; presence < 7; ++presence) {
                        TEST_PLNTSUM::Reset_();
                        Add_System(0, 0, rotation);
                        PLNTSUM::sort_choice = sort;
                        TEST_PLNTSUM::in_range[0] = reachable;
                        // Visited, racial omniscience, Galactic Lore only, or unknown.
                        MOX::star[0].visited = knowledge == 0 ? 1 : 0;
                        MOX::player[0].traits[TRAIT_OMNISCIENCE] = knowledge == 1;
                        MOX::player[0].has_galactic_lore = knowledge == 2;
                        // Empty, enemy colony, own colony, enemy outpost, own ship,
                        // enemy ship in orbit, or enemy ship en route.
                        if (presence >= 1 && presence <= 3) {
                            MOX::planet[0].colony_index = 0;
                            MOX::colony[0].planet = 0;
                            MOX::colony[0].owner = presence == 2 ? 0 : 1;
                            MOX::colony[0].outpost_flag = presence == 3;
                        } else if (presence >= 4) {
                            Add_Ship(presence == 4 ? 0 : 1,
                                presence == 6 ? SHIP_LOCATION_MOVING_OFFSET : 0);
                        }
                        PLNTSUM::Set_Planet_Explored_Flags();
                        for (const auto& order : orders) {
                            int mask = 0;
                            for (int step = 0; step < 7; ++step) {
                                if (step > 0) mask ^= order[(step - 1) % 3];
                                DOCTEST_CAPTURE(rotation);
                                DOCTEST_CAPTURE(sort);
                                DOCTEST_CAPTURE(knowledge);
                                DOCTEST_CAPTURE(reachable);
                                DOCTEST_CAPTURE(presence);
                                DOCTEST_CAPTURE(mask);
                                Set_Filters(mask);
                                std::vector<int16_t> expected;
                                bool enemy = presence == 1 || presence == 3 || presence == 5;
                                bool system_visible = knowledge != 3 &&
                                    (!(mask & 1) || !enemy) &&
                                    (!(mask & 2) || reachable);
                                if (system_visible) {
                                    for (int16_t i = 0; i < 5; ++i) {
                                        if (i == 0 && (presence == 2 || presence == 3)) continue;
                                        int minerals = (i + rotation) % 5;
                                        if ((mask & 4) && (minerals == MINERAL_RESOURCE_POOR || minerals == MINERAL_RESOURCE_ULTRA_POOR)) continue;
                                        expected.push_back(i);
                                    }
                                }
                                Check_List(expected);
                            }
                        }
                    }
                }
            }
        }
    }
}

DOCTEST_TEST_CASE("destroyed occupants no longer hide mixed-mineral systems") {
    TEST_PLNTSUM::Reset_();
    Add_System(0, 0);
    MOX::planet[0].colony_index = 0;
    MOX::colony[0].owner = 1;
    MOX::colony[0].planet = 0;
    MOX::player[1].home_planet_id = -1;
    Add_Ship(1, 0);
    PLNTSUM::Set_Planet_Explored_Flags();
    Set_Filters(7);
    Check_List({});

    BOMB::Destroy_Colony(&MOX::colony[0], -1);
    DOCTEST_CHECK(MOX::planet[0].colony_index == -1);
    DOCTEST_CHECK(MOX::colony[0].owner == -1);
    DOCTEST_CHECK(MOX::star[0].visited == 1);
    Check_List({});
    // Model Kill_Ship's retirement, including a stack that has not yet been rebuilt.
    MOX::ship[0].status = 5;
    MOX::ship[0].location = -1;
    Check_List({2, 3, 4});
    MOX::ship_stack_count = 0;
    Check_List({2, 3, 4});
    // Reopening the screen rebuilds exploration flags after the destruction.
    PLNTSUM::Set_Planet_Explored_Flags();
    MOX::scroll_bar->total_rows = 0;
    Check_List({2, 3, 4});
}

DOCTEST_TEST_CASE("high planet indices and nonzero player visitation bits remain independent") {
    for (int16_t first : {int16_t{0}, int16_t{254}, int16_t{510}, int16_t{MAX_PLANETS - 5}}) {
        for (int16_t player : {int16_t{0}, int16_t{7}}) {
            TEST_PLNTSUM::Reset_();
            MOX::PLAYER_NUM = player;
            Add_System(MAX_STARS - 1, first);
            PLNTSUM::Set_Planet_Explored_Flags();
            Set_Filters(7);
            DOCTEST_CAPTURE(first);
            DOCTEST_CAPTURE(player);
            Check_List({int16_t(first + 2), int16_t(first + 3), int16_t(first + 4)});
        }
    }
}

DOCTEST_TEST_CASE("presence and range exclusions do not leak between systems") {
    TEST_PLNTSUM::Reset_();
    Add_System(0, 0);
    Add_System(1, 5);
    Add_System(2, 10);
    MOX::planet[0].colony_index = 0;
    MOX::colony[0].owner = 1;
    TEST_PLNTSUM::in_range[1] = false;
    PLNTSUM::Set_Planet_Explored_Flags();
    Set_Filters(7);
    Check_List({12, 13, 14});
    Set_Filters(6);
    Check_List({2, 3, 4, 12, 13, 14});
    Set_Filters(7);
    Check_List({12, 13, 14});
    Set_Filters(5);
    Check_List({7, 8, 9, 12, 13, 14});
    Set_Filters(7);
    Check_List({12, 13, 14});
    Set_Filters(3);
    Check_List({10, 11, 12, 13, 14});
    Set_Filters(7);
    Check_List({12, 13, 14});
}

DOCTEST_TEST_CASE("Galactic Lore keeps unvisited mixed-mineral systems visible without enemies") {
    TEST_PLNTSUM::Reset_();
    Add_System(0, 0);
    MOX::star[0].visited = 0;
    MOX::player[0].has_galactic_lore = 1;
    PLNTSUM::Set_Planet_Explored_Flags();
    Set_Filters(6);
    Check_List({2, 3, 4});
    Set_Filters(7);
    Check_List({2, 3, 4});
    Set_Filters(3);
    Check_List({0, 1, 2, 3, 4});

    MOX::star[0].visited = 1;
    PLNTSUM::Set_Planet_Explored_Flags();
    Set_Filters(7);
    Check_List({2, 3, 4});
}

DOCTEST_TEST_SUITE_END();