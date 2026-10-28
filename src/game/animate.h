#pragma once
#include <cstdint>

struct s_animation_header;

namespace animate {
    extern s_palette_header* palette_header;

    extern s_animation_header* animation_header;

    extern int16_t clipping_flag;
    extern int16_t dot_stencil_flag;

    extern char s_Screen_Background_cant_be_clipp[35];
    extern char s_Refresh_Mask_cant_be_drawn[28];

    /** Returns whether the animation header enables the full-store flag. */
    short __cdecl Get_Full_Store_Flag(s_animation_header* header);

    /** Sets the current frame, clamping to the last available frame.
     *  @param anim Animation header to update.
     *  @param frame Requested frame index.
     */
    void __cdecl Set_Animation_Frame(s_animation_header* anim, int32_t frame);

    /** Resets the current frame to the first frame.
     *  @param anim Animation header to update.
     */
    void __cdecl Reset_Animation_Frame(s_animation_header* anim);

    /** Returns the number of frames stored in the animation.
     *  @param anim Animation header to query.
     */
    int16_t __cdecl Get_Animation_Frame_Count(s_animation_header* anim);

    /** Returns the currently selected frame index.
     *  @param animation Animation header to query.
     */
    int16_t __cdecl Get_Current_Frame(s_animation_header* animation);

    /** Returns the animation width in pixels.
     *  @param anim Animation header to query.
     */
    int16_t __cdecl Get_Width(s_animation_header* anim);

    /** Returns the animation height in pixels.
     *  @param anim Animation header to query.
     */
    int16_t __cdecl Get_Height(s_animation_header* anim);

    /** Draws the current frame and advances the animation state.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param anim Animation header to draw.
     */
    void __cdecl Draw(int16_t x, int16_t y, s_animation_header* anim);

    /** Draws the current frame through the remap table and advances the animation state.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param anim Animation header to draw.
     */
    void __cdecl Remap_Draw(int16_t x, int16_t y, s_animation_header* anim);
    void __cdecl Remap_Draw_Body(int16_t x, int16_t y, s_animation_header* anim);

    /** Draws an animated sprite frame using the remap table.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param frame_ptr Packed frame data.
     */
    void __cdecl Remap_Draw_Animated_Sprite(int16_t x, int16_t y, uint8_t* frame_ptr);

    /** Draws a clipped animated sprite frame using the remap table.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param frame_ptr Packed frame data.
     */
    void __cdecl Remap_Clipped_Draw_Animated_Sprite(int16_t x, int16_t y, uint8_t* frame_ptr);

    /** Draws the current frame without glass remapping.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param header Animation header to draw.
     */
    void __cdecl Draw_No_Glass(int16_t x, int16_t y, s_animation_header* header);

    /** Draws a packed animated sprite frame without glass remapping.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param frame_data Packed frame data.
     */
    void __cdecl Draw_No_Glassed_Animated_Sprite(int16_t x, int16_t y, uint8_t* frame_data);

    /** Draws a bitmap sprite without glass remapping.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param bitmap Bitmap pixel data.
     */
    void __cdecl Draw_No_Glassed_Bitmap_Sprite(int16_t x, int16_t y, uint8_t* bitmap);

    /** Applies the animation palette payload to the current palette. */
    void __cdecl Set_Animation_Palette();

    /** Draws a packed frame and mirrors the result into the background-diff buffers.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param frame_data Packed frame data.
     */
    void __cdecl Draw_Animated_Sprite_Back_Diff(int16_t x, int16_t y, uint8_t* frame_data);

    /** Draws a packed animated sprite frame with glass remapping.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param frame_data Packed frame data.
     */
    void __cdecl Draw_Glassed_Animated_Sprite(int16_t x, int16_t y, uint8_t* frame_data);

    /** Draws a bitmap sprite with glass remapping.
     *  @param x Destination x coordinate.
     *  @param y Destination y coordinate.
     *  @param sprite Bitmap pixel data.
     */
    void __cdecl Draw_Glassed_Bitmap_Sprite(int16_t x, int16_t y, uint8_t* sprite);

    /** Draws a glass-remapped packed background sprite to the render buffer.
     *  @param sprite Packed background data.
     */
    void __cdecl Draw_Glassed_Screen_Background_Off(uint8_t* sprite);

    /** Draws a packed screen-background sprite to the render buffer.
     *  @param sprite_data Packed background data.
     */
    void __cdecl Draw_Screen_Background_Off(uint8_t* sprite_data);

    /** Applies the embedded animation palette when the resource carries one.
     *  @param header Animation header to inspect.
     */
    void __cdecl Draw_Palette(s_animation_header* header);
} // namespace animate