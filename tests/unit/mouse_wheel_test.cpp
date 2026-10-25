#include "pch.h"

#include "doctest/doctest.h"

#include "fakes/fake_test_support.h"

namespace {
    struct Wheel_Test_Reset {
        Wheel_Test_Reset() {
            test::fakes::Reset_All_();
            fields::Clear_Fields();
            test::fakes::g_mouse_pointer_x = 200;
            test::fakes::g_mouse_pointer_y = 200;
        }

        ~Wheel_Test_Reset() {
            while (mouse_wheel::Take_Field() != 0) {
            }
            test::fakes::Reset_All_();
        }
    };

    struct Planet_List {
        int16_t value = 0;
        int16_t bar = 0;
        int16_t up = 0;
        int16_t down = 0;
    };

    // The planets screen (plntsum.cpp): a 15x407 bar at (418,37), arrows above and below it.
    Planet_List Add_Planet_List_() {
        Planet_List list;
        list.up = fields::Add_Hidden_Field(422, 16, 436, 33, "-", 40);
        list.down = fields::Add_Hidden_Field(421, 448, 436, 465, "+", 40);
        list.bar = fields::Add_Scroll_Field(418, 37, 0, 407, 0, 407, 15, 407, &list.value, "", 0);
        return list;
    }
} // namespace

DOCTEST_TEST_CASE("wheel notches press the arrow above or below the scroll bar") {
    Wheel_Test_Reset reset;
    const Planet_List list = Add_Planet_List_();

    mouse_wheel::Add_Steps(1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == list.up);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == 0);

    mouse_wheel::Add_Steps(-2.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == list.down);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == list.down);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == 0);
}

DOCTEST_TEST_CASE("wheel notches with no list on screen are dropped, not replayed later") {
    Wheel_Test_Reset reset;
    (void)fields::Add_Hidden_Field(10, 10, 50, 50, "a", 0);

    mouse_wheel::Add_Steps(3.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == 0);

    (void)Add_Planet_List_();
    DOCTEST_CHECK(mouse_wheel::Take_Field() == 0);
}

DOCTEST_TEST_CASE("fractional wheel movement adds up to whole notches") {
    Wheel_Test_Reset reset;
    const Planet_List list = Add_Planet_List_();

    mouse_wheel::Add_Steps(-0.5f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == 0);
    mouse_wheel::Add_Steps(-0.5f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == list.down);
}

DOCTEST_TEST_CASE("with two lists the wheel scrolls the one nearest the pointer") {
    Wheel_Test_Reset reset;
    int16_t left_value = 0;
    const int16_t left_up = fields::Add_Hidden_Field(100, 20, 114, 34, "", 0);
    (void)fields::Add_Hidden_Field(100, 250, 114, 264, "", 0);
    (void)fields::Add_Scroll_Field(100, 40, 0, 200, 0, 200, 14, 200, &left_value, "", 0);
    const Planet_List right = Add_Planet_List_();

    test::fakes::g_mouse_pointer_x = 60;
    mouse_wheel::Add_Steps(1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == left_up);

    test::fakes::g_mouse_pointer_x = 500;
    mouse_wheel::Add_Steps(1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == right.up);
}

DOCTEST_TEST_CASE("horizontal sliders are not scrolled by the wheel") {
    Wheel_Test_Reset reset;
    int16_t value = 0;
    (void)fields::Add_Hidden_Field(90, 10, 110, 30, "", 0);
    (void)fields::Add_Scroll_Field(100, 40, 0, 200, 0, 200, 200, 10, &value, "", 0);

    mouse_wheel::Add_Steps(1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == 0);
}

DOCTEST_TEST_CASE("get input returns the arrow pressed by a wheel notch") {
    Wheel_Test_Reset reset;
    const Planet_List list = Add_Planet_List_();

    mouse_wheel::Add_Steps(-1.0f);
    DOCTEST_CHECK(fields::Get_Input() == list.down);
}

DOCTEST_TEST_CASE("wide list rows beside the bar are not taken for arrows") {
    // The ship design weapon list (desbox.cpp): ten 570-pixel rows ending 7 pixels left of a
    // 9x120 bar, the arrow buttons above and below it.
    Wheel_Test_Reset reset;
    int16_t value = 0;
    for (int16_t i = 0; i < 10; ++i) {
        (void)fields::Add_Hidden_Field(48, 79 + i * 14, 618, 91 + i * 14, "", 41);
    }
    const int16_t up = fields::Add_Hidden_Field(622, 61, 636, 75, "-", 40);
    (void)fields::Add_Scroll_Field(625, 80, 0, 120, 0, 120, 9, 120, &value, "", 0);
    const int16_t down = fields::Add_Hidden_Field(622, 222, 636, 236, "+", 40);
    test::fakes::g_mouse_pointer_x = 300;
    test::fakes::g_mouse_pointer_y = 150;

    mouse_wheel::Add_Steps(-1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == down);
    mouse_wheel::Add_Steps(1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == up);
}

DOCTEST_TEST_CASE("arrow buttons without a scroll bar are found by their hotkeys") {
    // The fleet box (fleetpop.cpp): '-' and '+' buttons, no bar.
    Wheel_Test_Reset reset;
    const int16_t up = fields::Add_Hidden_Field(191, 42, 205, 56, "-", 40);
    const int16_t down = fields::Add_Hidden_Field(191, 174, 205, 188, "+", 40);

    mouse_wheel::Add_Steps(1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == up);
    mouse_wheel::Add_Steps(-1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == down);
}

DOCTEST_TEST_CASE("the galaxy map zoom buttons are not scrolled") {
    // mainscr.cpp: zoom in '+' above zoom out '-'.
    Wheel_Test_Reset reset;
    (void)fields::Add_Hidden_Field(244, 428, 268, 452, "+", 40);
    (void)fields::Add_Hidden_Field(244, 455, 268, 479, "-", 40);

    mouse_wheel::Add_Steps(1.0f);
    DOCTEST_CHECK(mouse_wheel::Take_Field() == 0);
}
