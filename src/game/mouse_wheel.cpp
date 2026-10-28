#include "pch.h"

#include "mouse_wheel.h"

#include <atomic>
#include <cmath>
#include <cstdlib>

namespace mouse_wheel {
    namespace {
        constexpr int32_t k_max_queued_steps = 8;
        // How far an arrow button may sit from the end of its scroll bar, and how far it may be
        // offset sideways (MOO2 screens put them 4-21 pixels above and below the bar).
        constexpr int32_t k_button_gap = 40;
        constexpr int32_t k_button_side_slack = 8;

        std::atomic<int32_t> queued_steps{0};
        float partial_steps;

        // Arrow buttons are small; list rows beside a bar are hidden fields too, but wide (the ship
        // design weapon list's rows end 7 pixels left of its bar).
        constexpr int32_t k_max_arrow_width = 32;

        bool Is_Arrow_Candidate(const s_field& field) {
            return (field.field_type == FIELD_TYPE_BUTTON || field.field_type == FIELD_TYPE_HIDDEN) && field.x_end - field.x < k_max_arrow_width;
        }

        // Distance from the pointer to a field's rectangle (0 inside it).
        int32_t Distance_To(const s_field& field, int32_t x, int32_t y) {
            const int32_t dx = x < field.x ? field.x - x : (x > field.x_end ? x - field.x_end : 0);
            const int32_t dy = y < field.y ? field.y - y : (y > field.y_end ? y - field.y_end : 0);
            return dx + dy;
        }

        // The arrow button directly above (up) or below (down) a vertical scroll bar, or 0.
        int16_t Arrow_For(const s_field& bar, bool up) {
            int16_t best = 0;
            int32_t best_gap = k_button_gap + 1;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field& field = fields::fields[i];
                if (!Is_Arrow_Candidate(field)) {
                    continue;
                }
                if (field.x_end < bar.x - k_button_side_slack || field.x > bar.x_end + k_button_side_slack) {
                    continue;
                }
                const int32_t gap = up ? bar.y - field.y_end : field.y - bar.y_end;
                if (gap < -4 || gap >= best_gap) {
                    continue;
                }
                best = i;
                best_gap = gap;
            }
            return best;
        }

        // Screens with arrow buttons but no scroll bar (the fleet box, the fleets screen): the
        // up/down pair nearest the pointer, found by the '-' and '+' hotkeys the original gives
        // list arrows. The '-' button must sit above the '+' one, which leaves out the galaxy
        // map's zoom buttons ('+' above '-').
        int16_t Hotkey_Arrow(int32_t x, int32_t y, bool up) {
            int16_t best = 0;
            int32_t best_distance = 0;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field& minus = fields::fields[i];
                if (minus.hotkey != '-' || !Is_Arrow_Candidate(minus)) {
                    continue;
                }
                for (int16_t j = 1; j < fields::fields_count; ++j) {
                    const s_field& plus = fields::fields[j];
                    if (plus.hotkey != '+' || !Is_Arrow_Candidate(plus) || plus.y <= minus.y_end) {
                        continue;
                    }
                    if (plus.x_end < minus.x - k_button_side_slack || plus.x > minus.x_end + k_button_side_slack) {
                        continue;
                    }
                    s_field pair = minus;
                    pair.x = SDL_min(minus.x, plus.x);
                    pair.x_end = SDL_max(minus.x_end, plus.x_end);
                    pair.y_end = plus.y_end;
                    const int32_t distance = Distance_To(pair, x, y);
                    if (best == 0 || distance < best_distance) {
                        best = up ? i : j;
                        best_distance = distance;
                    }
                }
            }
            return best;
        }
    } // namespace

    void __cdecl Add_Steps(float wheel_y) {
        // Smooth-scrolling devices send fractions of a notch.
        partial_steps += wheel_y;
        const int32_t whole = (int32_t)partial_steps;
        if (whole == 0) {
            return;
        }
        partial_steps -= (float)whole;
        int32_t current = queued_steps.load();
        int32_t next;
        do {
            next = SDL_clamp(current + whole, -k_max_queued_steps, k_max_queued_steps);
        } while (!queued_steps.compare_exchange_weak(current, next));
    }

    int16_t __cdecl Take_Field() {
        const int32_t steps = queued_steps.load();
        if (steps == 0) {
            return 0;
        }
        // The vertical scroll bar nearest the pointer is the list being scrolled.
        const int32_t x = mouse::Pointer_X();
        const int32_t y = mouse::Pointer_Y();
        const s_field* bar = nullptr;
        int32_t bar_distance = 0;
        for (int16_t i = 1; i < fields::fields_count; ++i) {
            const s_field& field = fields::fields[i];
            if (field.field_type != FIELD_TYPE_SCROLL || fields::Field_Data_Scroll(&field)->is_vertical == 0) {
                continue;
            }
            const int32_t distance = Distance_To(field, x, y);
            if (bar == nullptr || distance < bar_distance) {
                bar = &field;
                bar_distance = distance;
            }
        }
        int16_t arrow = bar != nullptr ? Arrow_For(*bar, steps > 0) : 0;
        if (arrow == 0) {
            arrow = Hotkey_Arrow(x, y, steps > 0);
        }
        if (arrow == 0) {
            queued_steps.store(0); // nothing to scroll here: don't replay these notches later
            return 0;
        }
        queued_steps.fetch_add(steps > 0 ? -1 : 1);
        return arrow;
    }
} // namespace mouse_wheel
