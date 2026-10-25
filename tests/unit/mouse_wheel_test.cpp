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
