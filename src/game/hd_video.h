#pragma once

#include <stdint.h>

// HD cutscenes on the parallel 4x screen (see hd_sprites.h).
//
// A Smacker video `<NAME>.LBX` has HD frames when the VFS provides hd/VIDEO/<NAME>/video.json
// (width, height, frames, scale, hd_frames) plus one <NNNNNN>.webp per HD frame. With
// hd_frames == 2 * frames, HD frame 2k is source frame k and 2k+1 lies halfway to k+1; those
// in-between frames are published on their own, halfway between two source frames, so the
// video plays at twice its frame rate. Audio, timing and the 8-bit page are unchanged.
namespace hd_video {

    // A Smacker video was opened (smckplay::Open_Smacker_Animation).
    void Open(const char* file_name, int32_t width, int32_t height, int32_t frame_count);
    // Source frame `frame` was just written at (left, top) of the off page.
    void Frame(int32_t left, int32_t top, int32_t frame);
    // Before waiting for the next source frame, due at `next_frame_ns` (SDL_GetTicksNS):
    // publishes the in-between frame at its time when it is ready.
    void Between_Frames(uint64_t next_frame_ns, uint64_t frame_duration_ns);
    void Close();

} // namespace hd_video
