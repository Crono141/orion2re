#pragma once

#include <stdint.h>

struct s_animation_header;

namespace video {
    struct s_palette_color;
}

// Present-time replacement of game sprites with higher-resolution art.
//
// The engine keeps drawing 8-bit pixels into its 640x480 pages. Alongside each page, an owner
// plane records which sprite draw wrote each pixel and the value it wrote. At present time,
// every source pixel whose page value still equals the recorded value, and whose sprite has
// HD art, is replaced by the matching block of HD pixels. Anything that later overwrote the
// pixel (text, fills, glass, remaps, captures) no longer matches and falls back to the normal
// scaler output, so only the sprite blitters need to record ownership.
//
// HD art is read from the VFS under hd/<LBX>/<EEE>/ (sprite.json plus one PNG per frame,
// grey = palette index, alpha = coverage), normally supplied by a mod package's data/ tree.
// Indices are looked up in the live palette, so fades and palette cycling keep working.
namespace hd_sprites {

#define HD_SPRITES_SCALE 4

    // Enables the feature when the VFS provides hd/pack.json. Safe to call more than once.
    void Initialize();
    bool Enabled();

    // Called for every LBX entry loaded into memory.
    void Register_Load(const void* data, int32_t size, const char* lbx_name, int32_t entry);

    // Brackets one sprite draw; blitters stamp ownership for the current draw in between.
    void Begin_Draw(const s_animation_header* anim, int16_t x, int16_t y);
    void End_Draw();

    // Brackets one glyph draw (fonts::Print_Character and variants). (x, y) is the unclipped
    // top-left of the glyph; colors are the 8 style colours the glyph's roles map to.
    void Begin_Glyph(const char* font_file, int16_t font_index, uint8_t code, int16_t x, int16_t y, const uint8_t* colors);

    // Off-screen images (bitmap-mode animations built with bitmap::*) carry their own owner
    // layer, so sprites and text composed into them keep their HD art when the image is
    // later drawn to the screen.
    void Bitmap_Reset(const void* bitmap, int32_t width, int32_t height);
    // Source `src` drawn with its top-left at (x, y) of `target_bitmap`.
    void Begin_Draw_To_Bitmap(const s_animation_header* src, int16_t x, int16_t y, const void* target_bitmap);
    // Pixel `index` of `bitmap` was written with `value` by the current draw.
    void Stamp_Bitmap(const void* bitmap, int32_t index, uint8_t value);
    // Same-size copy of src's owner layer to dst (bitmap::Copy_Bitmap_To_Bitmap etc.).
    void Bitmap_Copy(const void* dst, const void* src);
    // Owner layer of a rectangle of the current page captured into `bitmap`.
    void Bitmap_Capture(const void* bitmap, int32_t x1, int32_t y1, int32_t width, int32_t height);

    // A glass pixel was written at `dst`: the page held `background_value` before the glass
    // remap replaced it. The owner of what was underneath is kept so the compositor can tint
    // the HD art below in true colour.
    void Stamp_Glass(const uint8_t* dst, uint8_t background_value);

    // Records that `count` pixels at `dst` (inside the current video page) were written.
    void Stamp_Run(const uint8_t* dst, int32_t count);

    // Page plumbing: owner planes follow page allocation, copies and publishing.
    void Allocate_Planes(int32_t width, int32_t height);
    void Free_Planes();
    void Copy_Plane(const uint8_t* src_page, const uint8_t* dst_page);
    void Publish_Plane(const uint8_t* published_page_pixels);

    struct s_compose_input {
        const uint8_t* page;                   // published 8-bit page
        const video::s_palette_color* palette; // palette submitted with the page
        const uint32_t* base;                  // scaler output, ARGB8888
        int32_t base_stride;                   // in pixels
        int32_t base_factor;                   // scaler factor (1-4)
        int32_t width;                         // source page size
        int32_t height;
        int32_t cursor_x; // source-space cursor rectangle to keep, or -1
        int32_t cursor_y;
        int32_t cursor_size;
    };

    // Writes the 4x composited frame (width*4 x height*4 ARGB8888) to `output`.
    // Returns the number of source pixels replaced with HD art.
    int32_t Compose(const s_compose_input* input, uint32_t* output);

} // namespace hd_sprites
