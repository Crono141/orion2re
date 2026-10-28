#pragma once

#include <cstdint>

// Mouse wheel scrolling for list screens. The original game has no wheel support: lists scroll
// with the arrow buttons at the ends of their scroll bar. A wheel notch is turned into a press
// of the matching arrow button, so every screen's own scrolling code runs unchanged.
namespace mouse_wheel {
    /** Queues wheel movement from the platform thread (positive = away from the user, up). */
    void __cdecl Add_Steps(float wheel_y);

    /** Returns the field index of the arrow button one queued notch should press on the current
     *  screen, or 0 when there is nothing to press (the queued notches are then dropped). */
    int16_t __cdecl Take_Field();
} // namespace mouse_wheel
