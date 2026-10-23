#include "pch.h"

#include "game/hd_sprites.h"

#include <algorithm>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "yyjson.h"

namespace hd_sprites {

#define HD_MAX_ASSETS 16384
#define HD_GLASS_FIRST 0xF0
#define HD_MAX_GLYPH_FONTS 64

    namespace {

        enum e_frame_state {
            FRAME_ORIGINAL = 0,
            FRAME_ART = 1,
            FRAME_SAME_AS = 2,
        };

        struct s_hd_frame {
            e_frame_state state;
            int32_t same_as;
            int32_t origin_x;
            int32_t origin_y;
            int32_t width;
            int32_t height;
            uint8_t* index;
            uint8_t* alpha;
        };

        struct s_hd_asset {
            std::string directory;
            bool loaded;
            bool has_source_hash;
            uint32_t source_hash; // FNV-1a of the LBX entry the art was made from
            bool glassed;
            int32_t scale;
            int32_t frame_count;
            s_hd_frame* frames;
        };

        struct s_registration {
            int32_t asset;
            uint32_t data_hash;
            uint8_t header[12];
            int32_t size;
        };

        // HD glyphs for one font size of one font file: hd/<FILE>/<font>/<code>.png, LA with
        // L = colour role (1..8) and A = coverage, at HD_SPRITES_SCALE x the glyph cell.
        struct s_hd_glyph_font {
            std::string directory;
            bool present;
            uint8_t state[256]; // 0 = not tried, 1 = loaded, 2 = missing
            s_hd_frame glyphs[256];
        };

        bool initialized;
        bool enabled;

        // Assets are appended and never moved or freed.
        s_hd_asset* assets[HD_MAX_ASSETS];
        int32_t asset_count;
        std::unordered_map<std::string, int32_t> asset_by_key;
        std::unordered_map<const void*, s_registration> registrations;

        s_hd_glyph_font* glyph_fonts[HD_MAX_GLYPH_FONTS];
        int32_t glyph_font_count;
        std::unordered_map<std::string, int32_t> glyph_font_by_key;

        // ORION2RE_HD_STATS: count draws that had no HD art, by asset, and log them
        // periodically. Debug aid for finding coverage gaps.
        bool stats_enabled;
        std::unordered_map<const void*, std::string> stats_names;
        std::unordered_map<std::string, uint32_t> stats_missing;
        uint32_t stats_draws;
        uint32_t stats_interval = 20000;
        uint32_t stats_glyph_missing_art;
        uint32_t stats_publishes;
        uint32_t stats_settled_pixels;
        // Game-thread time per stage (ORION2RE_HD_STATS), reported with the settle count.
        enum e_timer { TIMER_RENDER,
            TIMER_COPY,
            TIMER_PUBLISH,
            TIMER_COUNT };
        uint64_t stats_timer_ns[TIMER_COUNT];

        struct s_scoped_timer {
            e_timer timer;
            uint64_t start;
            explicit s_scoped_timer(e_timer which) : timer(which), start(stats_enabled ? SDL_GetTicksNS() : 0) {}
            ~s_scoped_timer() {
                if (stats_enabled) {
                    stats_timer_ns[timer] += SDL_GetTicksNS() - start;
                }
            }
        };
        // ORION2RE_HD_DEBUG=sd: tint blocks that fell back to the enlarged original because
        // unhooked code changed the page pixel.
        bool debug_sd;
        // ORION2RE_HD_PROBE=x,y: log every HD write to that page pixel (debug aid).
        int32_t probe_x = -1;
        int32_t probe_y = -1;

        // ORION2RE_HD_CAPTURE=<dir>: write the live palette the first time each LBX sprite is
        // drawn under it, as <dir>/<LBX>_<EEE>_<hash>.pal (256 x RGB, 8-bit). The HD pipeline uses
        // these instead of palettes inferred from the source code.
        std::string capture_dir;
        std::unordered_map<std::string, bool> captured;

        void Capture_Palette(const std::string& name) {
            uint8_t rgb[768];
            uint32_t hash = 2166136261u;
            for (int32_t i = 0; i < 256; ++i) {
                const s_palette_entry& entry = video::current_palette[i];
                rgb[i * 3 + 0] = (uint8_t)((entry.r & 0x3F) * 255 / 63);
                rgb[i * 3 + 1] = (uint8_t)((entry.g & 0x3F) * 255 / 63);
                rgb[i * 3 + 2] = (uint8_t)((entry.b & 0x3F) * 255 / 63);
            }
            for (uint8_t byte : rgb) {
                hash = (hash ^ byte) * 16777619u;
            }
            char file[160];
            std::string base = name;
            std::replace(base.begin(), base.end(), '#', '_');
            snprintf(file, sizeof(file), "%s_%08x.pal", base.c_str(), hash);
            if (!captured.emplace(file, true).second) {
                return;
            }
            const std::string path = capture_dir + "/" + file;
            SDL_IOStream* out = SDL_IOFromFile(path.c_str(), "wb");
            if (out != nullptr) {
                (void)SDL_WriteIO(out, rgb, sizeof(rgb));
                SDL_CloseIO(out);
            }
        }

        void Stats_Missing(const std::string& key) {
            stats_missing[key]++;
            if (++stats_draws % stats_interval != 0) {
                return;
            }
            std::vector<std::pair<uint32_t, std::string>> top;
            for (const auto& item : stats_missing) {
                top.emplace_back(item.second, item.first);
            }
            std::sort(top.rbegin(), top.rend());
            std::string line;
            for (size_t i = 0; i < top.size() && i < 25; ++i) {
                line += " " + top[i].second + "=" + std::to_string(top[i].first);
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "hd stats: draws without HD art:%s", line.c_str());
        }

        bool Read_Vfs_File(const char* path, std::vector<uint8_t>* bytes) {
            s_vfs_file* file = vfs::fopen(path, "rb");
            if (file == nullptr) {
                return false;
            }
            const int32_t size = vfs::fsize(file);
            bool ok = size > 0;
            if (ok) {
                bytes->resize((size_t)size);
                ok = vfs::fread(bytes->data(), 1, bytes->size(), file) == bytes->size();
            }
            vfs::fclose(file);
            return ok;
        }

        bool Load_Frame_Png(const std::string& path, s_hd_frame* frame) {
            std::vector<uint8_t> bytes;
            if (!Read_Vfs_File(path.c_str(), &bytes)) {
                return false;
            }
            SDL_IOStream* stream = SDL_IOFromConstMem(bytes.data(), bytes.size());
            SDL_Surface* loaded = stream != nullptr ? SDL_LoadPNG_IO(stream, true) : nullptr;
            if (loaded == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: unable to decode %s: %s", path.c_str(), SDL_GetError());
                return false;
            }
            SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(loaded);
            if (rgba == nullptr) {
                return false;
            }
            if (frame->width == 0 && frame->height == 0) {
                frame->width = rgba->w;
                frame->height = rgba->h;
            }
            if (rgba->w != frame->width || rgba->h != frame->height) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: %s is %dx%d, sprite.json says %dx%d", path.c_str(), rgba->w, rgba->h, frame->width, frame->height);
                SDL_DestroySurface(rgba);
                return false;
            }
            const size_t count = (size_t)frame->width * (size_t)frame->height;
            frame->index = (uint8_t*)malloc(count);
            frame->alpha = (uint8_t*)malloc(count);
            if (frame->index == nullptr || frame->alpha == nullptr) {
                SDL_DestroySurface(rgba);
                return false;
            }
            for (int32_t y = 0; y < frame->height; ++y) {
                const uint8_t* row = (const uint8_t*)rgba->pixels + (size_t)y * (size_t)rgba->pitch;
                for (int32_t x = 0; x < frame->width; ++x) {
                    // Grey PNGs expand to R = G = B = palette index.
                    frame->index[(size_t)y * frame->width + x] = row[x * 4 + 0];
                    frame->alpha[(size_t)y * frame->width + x] = row[x * 4 + 3];
                }
            }
            SDL_DestroySurface(rgba);
            return true;
        }

        bool Read_Int_Pair(yyjson_val* array, int32_t* a, int32_t* b) {
            if (!yyjson_is_arr(array) || yyjson_arr_size(array) != 2) {
                return false;
            }
            *a = (int32_t)yyjson_get_int(yyjson_arr_get(array, 0));
            *b = (int32_t)yyjson_get_int(yyjson_arr_get(array, 1));
            return true;
        }

        // Loads sprite.json and every frame PNG of an asset. Runs on the game thread the first
        // time the asset is drawn.
        void Load_Asset(s_hd_asset* asset) {
            asset->loaded = true;
            std::vector<uint8_t> json;
            const std::string meta_path = asset->directory + "/sprite.json";
            if (!Read_Vfs_File(meta_path.c_str(), &json)) {
                return;
            }
            yyjson_doc* document = yyjson_read((const char*)json.data(), json.size(), 0);
            yyjson_val* root = document != nullptr ? yyjson_doc_get_root(document) : nullptr;
            yyjson_val* frames = root != nullptr ? yyjson_obj_get(root, "frames") : nullptr;
            if (!yyjson_is_arr(frames)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: invalid %s", meta_path.c_str());
                yyjson_doc_free(document);
                return;
            }
            asset->scale = (int32_t)yyjson_get_int(yyjson_obj_get(root, "scale"));
            asset->glassed = yyjson_get_bool(yyjson_obj_get(root, "glassed"));
            yyjson_val* source_hash = yyjson_obj_get(root, "source_hash");
            asset->has_source_hash = yyjson_is_uint(source_hash) || yyjson_is_int(source_hash);
            asset->source_hash = asset->has_source_hash ? (uint32_t)yyjson_get_uint(source_hash) : 0;
            if (asset->scale != HD_SPRITES_SCALE) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: %s has scale %d, only %d is supported", meta_path.c_str(), asset->scale, HD_SPRITES_SCALE);
                yyjson_doc_free(document);
                return;
            }

            const int32_t count = (int32_t)yyjson_arr_size(frames);
            s_hd_frame* loaded = (s_hd_frame*)calloc((size_t)count, sizeof(s_hd_frame));
            if (loaded == nullptr) {
                yyjson_doc_free(document);
                return;
            }
            size_t index = 0;
            size_t max = 0;
            yyjson_val* item = nullptr;
            yyjson_arr_foreach(frames, index, max, item) {
                const int32_t frame_number = (int32_t)yyjson_get_int(yyjson_obj_get(item, "frame"));
                if (frame_number < 0 || frame_number >= count) {
                    continue;
                }
                s_hd_frame* frame = &loaded[frame_number];
                yyjson_val* same_as = yyjson_obj_get(item, "same_as");
                if (same_as != nullptr) {
                    frame->state = FRAME_SAME_AS;
                    frame->same_as = (int32_t)yyjson_get_int(same_as);
                    continue;
                }
                if (!Read_Int_Pair(yyjson_obj_get(item, "origin"), &frame->origin_x, &frame->origin_y) ||
                    !Read_Int_Pair(yyjson_obj_get(item, "size"), &frame->width, &frame->height)) {
                    continue;
                }
                char name[16];
                snprintf(name, sizeof(name), "/%03d.png", frame_number);
                if (Load_Frame_Png(asset->directory + name, frame)) {
                    frame->state = FRAME_ART;
                }
            }
            yyjson_doc_free(document);
            asset->frames = loaded;
            asset->frame_count = count;
        }

        const s_hd_frame* Resolve_Frame(const s_hd_asset* asset, int32_t frame_number) {
            for (int32_t hops = 0; hops < 4; ++hops) {
                if (asset->frames == nullptr || frame_number < 0 || frame_number >= asset->frame_count) {
                    return nullptr;
                }
                const s_hd_frame* frame = &asset->frames[frame_number];
                if (frame->state == FRAME_ART) {
                    return frame;
                }
                if (frame->state != FRAME_SAME_AS) {
                    return nullptr;
                }
                frame_number = frame->same_as;
            }
            return nullptr;
        }

        int32_t Find_Or_Create_Asset(const char* lbx_name, int32_t entry) {
            char key[64];
            snprintf(key, sizeof(key), "%s/%03d", lbx_name, (int)entry);
            for (char* c = key; *c != '\0'; ++c) {
                if (*c >= 'a' && *c <= 'z') {
                    *c = (char)(*c - 'a' + 'A');
                }
            }
            const auto found = asset_by_key.find(key);
            if (found != asset_by_key.end()) {
                return found->second;
            }
            int32_t asset_index = -1;
            const std::string directory = std::string("hd/") + key;
            if (asset_count < HD_MAX_ASSETS && vfs::File_Exists((directory + "/sprite.json").c_str()) != 0) {
                s_hd_asset* asset = new s_hd_asset();
                asset->directory = directory;
                assets[asset_count] = asset;
                asset_index = asset_count++;
            }
            asset_by_key.emplace(key, asset_index);
            return asset_index;
        }

        int32_t Find_Glyph_Font(const char* font_file, int16_t font_index) {
            char key[96];
            snprintf(key, sizeof(key), "%s/%d", font_file, (int)font_index);
            for (char* c = key; *c != '\0'; ++c) {
                if (*c >= 'a' && *c <= 'z') {
                    *c = (char)(*c - 'a' + 'A');
                }
            }
            const auto found = glyph_font_by_key.find(key);
            if (found != glyph_font_by_key.end()) {
                return found->second;
            }
            int32_t slot = -1;
            std::string file_key(key, strchr(key, '/') - key);
            if (glyph_font_count < HD_MAX_GLYPH_FONTS &&
                vfs::File_Exists(("hd/" + file_key + "/font.json").c_str()) != 0) {
                s_hd_glyph_font* font = new s_hd_glyph_font();
                font->directory = std::string("hd/") + key;
                font->present = true;
                glyph_fonts[glyph_font_count] = font;
                slot = glyph_font_count++;
            }
            glyph_font_by_key.emplace(key, slot);
            return slot;
        }

        const s_hd_frame* Glyph_Frame(int32_t slot, int32_t code) {
            if (slot < 0 || slot >= glyph_font_count || code < 0 || code > 255) {
                return nullptr;
            }
            const s_hd_glyph_font* font = glyph_fonts[slot];
            return font->state[code] == 1 ? &font->glyphs[code] : nullptr;
        }

        // ------------------------------------------------------------------ HD pixels
        //
        // Palette form: foreground index (bits 0-7) over background index (bits 8-15) with
        // coverage (bits 16-23); HD_CLEAR marks a transparent background (inside images).
        // Literal form: HD_LITERAL | RGB.

        constexpr int32_t S = HD_SPRITES_SCALE;
        constexpr uint32_t HD_LITERAL = 0x80000000u;
        constexpr uint32_t HD_CLEAR = 0x01000000u;
        constexpr uint16_t SHADOW_NONE = 0x100;

        inline uint32_t Indexed(uint8_t index) {
            return index | (255u << 16);
        }

        inline uint32_t Literal(uint32_t rgb) {
            return HD_LITERAL | (rgb & 0xFFFFFFu);
        }

        inline uint32_t Mix(uint32_t over, uint32_t under, uint32_t alpha) {
            const uint32_t inv = 255u - alpha;
            const uint32_t r = (((over >> 16) & 0xFF) * alpha + ((under >> 16) & 0xFF) * inv) / 255u;
            const uint32_t g = (((over >> 8) & 0xFF) * alpha + ((under >> 8) & 0xFF) * inv) / 255u;
            const uint32_t b = ((over & 0xFF) * alpha + (under & 0xFF) * inv) / 255u;
            return (r << 16) | (g << 8) | b;
        }

        // Colour of a palette index in the screen's palette before fading (6-bit VGA values).
        inline uint32_t Base_Rgb(uint8_t index) {
            const s_palette_entry& entry = video::current_palette[index];
            return ((uint32_t)((entry.r & 0x3F) * 255 / 63) << 16) | ((uint32_t)((entry.g & 0x3F) * 255 / 63) << 8) |
                (uint32_t)((entry.b & 0x3F) * 255 / 63);
        }

        uint32_t Base_Color(uint32_t pixel) {
            if ((pixel & HD_LITERAL) != 0) {
                return pixel & 0xFFFFFFu;
            }
            const uint32_t alpha = (pixel >> 16) & 0xFF;
            const uint32_t fg = Base_Rgb((uint8_t)pixel);
            if (alpha >= 255) {
                return fg;
            }
            const uint32_t bg = (pixel & HD_CLEAR) != 0 ? 0u : Base_Rgb((uint8_t)(pixel >> 8));
            return Mix(fg, bg, alpha);
        }

        // Palette index `index` with coverage `alpha` over `dest`.
        uint32_t Over_Index(uint32_t dest, uint8_t index, uint32_t alpha) {
            if (alpha >= 255) {
                return Indexed(index);
            }
            if ((dest & HD_LITERAL) != 0) {
                return Literal(Mix(Base_Rgb(index), dest, alpha));
            }
            // One level of blending is kept: the destination's dominant colour becomes the
            // background, or its own background when it is mostly uncovered.
            if (((dest >> 16) & 0xFF) >= 128) {
                return index | ((dest & 0xFF) << 8) | (alpha << 16);
            }
            return index | (dest & 0xFF00u) | (alpha << 16) | (dest & HD_CLEAR);
        }

        uint32_t Over_Rgb(uint32_t dest, uint32_t rgb, uint32_t alpha) {
            if (alpha >= 255) {
                return Literal(rgb);
            }
            return Literal(Mix(rgb, Base_Color(dest), alpha));
        }

        // Glass of level `level` with coverage `alpha` over `dest`, tinted in true colour.
        uint32_t Glass_Tint(uint32_t dest, int32_t level, uint32_t alpha) {
            const auto& glass = video::glass_colors[level & 15];
            const uint32_t under = Base_Color(dest);
            uint32_t tinted = under;
            if (glass.intensity != 0) {
                const uint32_t tint = ((uint32_t)((glass.r & 0x3F) * 255 / 63) << 16) |
                    ((uint32_t)((glass.g & 0x3F) * 255 / 63) << 8) | (uint32_t)((glass.b & 0x3F) * 255 / 63);
                tinted = Mix(tint, under, glass.intensity >= 100 ? 255u : (uint32_t)glass.intensity * 255u / 100u);
            }
            return Literal(alpha >= 255 ? tinted : Mix(tinted, under, alpha));
        }

        // --------------------------------------------------------------- HD surfaces

        struct s_surface {
            int32_t width = 0; // in page pixels
            int32_t height = 0;
            std::vector<uint32_t> hd;       // (width * S) x (height * S); empty: no HD (images)
            std::vector<uint16_t> shadow;   // page value each block was made for
            std::vector<uint32_t> mark;     // draw that last stamped each pixel
            std::vector<uint8_t> row_dirty; // pages: rows changed since last copied onward
        };

        s_surface page_off;
        s_surface page_back;
        s_surface page_published;
        s_surface page_present;
        // Factor (0-256) applied to literal pixels so they follow palette fades.
        uint32_t published_fade = 256;
        uint32_t present_fade = 256;
        // Off-screen images keyed by their header. Nodes stay put, so pointers remain valid.
        std::unordered_map<const void*, s_surface> images;

        void Ensure_Hd(s_surface& surface) {
            if (!surface.hd.empty() || surface.width <= 0 || surface.height <= 0) {
                return;
            }
            const size_t count = (size_t)surface.width * (size_t)surface.height;
            surface.hd.assign(count * S * S, HD_CLEAR);
            surface.shadow.assign(count, SHADOW_NONE);
            surface.mark.assign(count, 0u);
        }

        void Allocate_Page(s_surface& surface, int32_t width, int32_t height) {
            surface.width = width;
            surface.height = height;
            surface.hd.assign((size_t)width * height * S * S, Indexed(0));
            surface.shadow.assign((size_t)width * height, SHADOW_NONE);
            surface.mark.assign((size_t)width * height, 0u);
            surface.row_dirty.assign((size_t)height, 1);
        }

        void Free_Surface(s_surface& surface) {
            surface = s_surface();
        }

        inline uint32_t* Block(s_surface& surface, int32_t x, int32_t y) {
            return &surface.hd[(size_t)y * S * surface.width * S + (size_t)x * S];
        }

        inline void Mark_Row(s_surface& surface, int32_t y) {
            if (!surface.row_dirty.empty()) {
                surface.row_dirty[(size_t)y] = 1;
            }
        }

        void Fill_Block(s_surface& surface, int32_t x, int32_t y, uint32_t pixel) {
            uint32_t* block = Block(surface, x, y);
            const size_t stride = (size_t)surface.width * S;
            for (int32_t sy = 0; sy < S; ++sy) {
                for (int32_t sx = 0; sx < S; ++sx) {
                    block[sy * stride + sx] = pixel;
                }
            }
            Mark_Row(surface, y);
        }

        s_surface* Page_Surface(const uint8_t* page) {
            if (page == nullptr || page_off.hd.empty()) {
                return nullptr;
            }
            if (page == video::g_page_off.pixels) {
                return &page_off;
            }
            if (page == video::g_page_back.pixels) {
                return &page_back;
            }
            return nullptr;
        }

        // Blocks whose page pixel was changed by code without hooks are replaced by the original
        // pixel enlarged. Runs over each publish, and over a draw's area before HD art is blended
        // into it, so soft edges never blend over leftovers of an earlier screen.
        uint32_t Settle(s_surface& surface, const uint8_t* page, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

        void Settle_Draw_Area(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
            if (page_off.hd.empty()) {
                return;
            }
            (void)Settle(page_off, video::g_page_off.pixels, x0, y0, x1, y1);
            (void)Settle(page_back, video::g_page_back.pixels, x0, y0, x1, y1);
        }

        // ------------------------------------------------------------- current draw

        enum e_source {
            SOURCE_NONE,  // no HD art: written pixels show the original enlarged
            SOURCE_ART,   // a sprite, glyph or file animation frame with HD art
            SOURCE_IMAGE, // an off-screen image with an HD surface
        };

        struct s_draw {
            e_source source = SOURCE_NONE;
            const s_hd_frame* frame = nullptr;
            bool glyph = false;
            bool glassed = false;
            int32_t x = 0; // where the sprite's top-left lands in the target
            int32_t y = 0;
            bool remapped = false;
            uint8_t remap[256];
            uint8_t colors[8];
            std::vector<uint8_t> shade; // brightness percent per sprite pixel, empty = none
            int32_t shade_width = 0;
            int32_t shade_height = 0;
            const s_surface* image = nullptr;
            int32_t image_x = 0;
            int32_t image_y = 0;
            std::string name; // for the probe
        };

        struct s_stamp {
            s_surface* target;
            int32_t offset;
            uint8_t value;
            int8_t level; // glass level, -1 for a plain pixel
        };

        s_draw draw;
        uint32_t generation = 1;
        // Art draws are rendered when the draw ends, once every written pixel is known.
        std::vector<s_stamp> pending;

        void Probe(const s_surface& target, int32_t offset, uint8_t value, const char* what) {
            if (probe_x < 0 || target.width <= 0 || offset % target.width != probe_x || offset / target.width != probe_y) {
                return;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "hd probe (%d,%d) %s: value=%u source=%d (%s) at %d,%d %s", probe_x, probe_y, what, value, (int)draw.source, draw.name.c_str(), draw.x, draw.y, &target == &page_off ? "off" : &target == &page_back ? "back"
                                                                                                                                                                                                                                                           : "image");
        }

        // Darkens `rgb` by the shade mask sampled bilinearly at sub-pixel (sx, sy) of sprite pixel
        // (px, py), which turns the stepped 1x shadow edge into a smooth gradient.
        uint32_t Shade(uint32_t rgb, int32_t px, int32_t py, int32_t sx, int32_t sy) {
            const int32_t width = draw.shade_width;
            const int32_t height = draw.shade_height;
            const int32_t fx = SDL_clamp(px * S + sx - S / 2, 0, (width - 1) * S);
            const int32_t fy = SDL_clamp(py * S + sy - S / 2, 0, (height - 1) * S);
            const int32_t x0 = fx / S;
            const int32_t y0 = fy / S;
            const int32_t x1 = SDL_min(x0 + 1, width - 1);
            const int32_t y1 = SDL_min(y0 + 1, height - 1);
            const int32_t wx = fx - x0 * S;
            const int32_t wy = fy - y0 * S;
            const uint8_t* b = draw.shade.data();
            const int32_t top = b[y0 * width + x0] * (S - wx) + b[y0 * width + x1] * wx;
            const int32_t bottom = b[y1 * width + x0] * (S - wx) + b[y1 * width + x1] * wx;
            const uint32_t percent = (uint32_t)((top * (S - wy) + bottom * wy) / (S * S));
            if (percent >= 100) {
                return rgb;
            }
            return ((((rgb >> 16) & 0xFF) * percent / 100u) << 16) | ((((rgb >> 8) & 0xFF) * percent / 100u) << 8) |
                ((rgb & 0xFF) * percent / 100u);
        }

        // The current draw's HD art at sub-pixel (sx, sy) of target pixel (x, y), over `dest`.
        uint32_t Apply_Art(uint32_t dest, int32_t x, int32_t y, int32_t sx, int32_t sy) {
            const s_hd_frame* frame = draw.frame;
            const int32_t lx = (x - draw.x - frame->origin_x) * S + sx;
            const int32_t ly = (y - draw.y - frame->origin_y) * S + sy;
            if (lx < 0 || ly < 0 || lx >= frame->width || ly >= frame->height) {
                return dest;
            }
            const size_t i = (size_t)ly * frame->width + lx;
            const uint32_t alpha = frame->alpha[i];
            if (alpha == 0) {
                return dest;
            }
            uint8_t index = frame->index[i];
            if (draw.glyph) {
                // Glyph art stores colour roles; the style supplies the indices.
                if (index < 1 || index > 8) {
                    return dest;
                }
                return Over_Index(dest, draw.colors[index - 1], alpha);
            }
            if (draw.glassed && index >= HD_GLASS_FIRST) {
                return Glass_Tint(dest, index - HD_GLASS_FIRST, alpha);
            }
            if (draw.remapped) {
                index = draw.remap[index];
            }
            if (!draw.shade.empty()) {
                return Over_Rgb(dest, Shade(Base_Rgb(index), x - draw.x, y - draw.y, sx, sy), alpha);
            }
            return Over_Index(dest, index, alpha);
        }

        void Apply_Art_Block(s_surface& target, int32_t x, int32_t y) {
            uint32_t* block = Block(target, x, y);
            const size_t stride = (size_t)target.width * S;
            for (int32_t sy = 0; sy < S; ++sy) {
                for (int32_t sx = 0; sx < S; ++sx) {
                    block[sy * stride + sx] = Apply_Art(block[sy * stride + sx], x, y, sx, sy);
                }
            }
            Mark_Row(target, y);
        }

        // A pixel written by a draw without HD art (or an image draw), rendered right away.
        void Render_Now(s_surface& target, int32_t offset, uint8_t value, int32_t level) {
            if (target.hd.empty()) {
                return;
            }
            const int32_t x = offset % target.width;
            const int32_t y = offset / target.width;
            target.shadow[(size_t)offset] = value;
            if (level >= 0) {
                // Glass over whatever HD pixels are underneath.
                uint32_t* block = Block(target, x, y);
                const size_t stride = (size_t)target.width * S;
                for (int32_t sy = 0; sy < S; ++sy) {
                    for (int32_t sx = 0; sx < S; ++sx) {
                        block[sy * stride + sx] = Glass_Tint(block[sy * stride + sx], level, 255u);
                    }
                }
                Mark_Row(target, y);
                return;
            }
            if (draw.source == SOURCE_IMAGE) {
                const s_surface& image = *draw.image;
                const int32_t ix = x - draw.image_x;
                const int32_t iy = y - draw.image_y;
                if (!image.hd.empty() && ix >= 0 && iy >= 0 && ix < image.width && iy < image.height &&
                    image.shadow[(size_t)iy * image.width + ix] == value) {
                    const uint32_t* src = &image.hd[(size_t)iy * S * image.width * S + (size_t)ix * S];
                    const size_t src_stride = (size_t)image.width * S;
                    uint32_t* block = Block(target, x, y);
                    const size_t stride = (size_t)target.width * S;
                    for (int32_t sy = 0; sy < S; ++sy) {
                        for (int32_t sx = 0; sx < S; ++sx) {
                            const uint32_t pixel = src[sy * src_stride + sx];
                            uint32_t& out = block[sy * stride + sx];
                            if ((pixel & HD_CLEAR) != 0 && (pixel & HD_LITERAL) == 0) {
                                // Partly covered over the image's transparent background.
                                if (((pixel >> 16) & 0xFF) != 0) {
                                    out = Over_Index(out, (uint8_t)pixel, (pixel >> 16) & 0xFF);
                                }
                            } else {
                                out = pixel;
                            }
                        }
                    }
                    Mark_Row(target, y);
                    return;
                }
            }
            Fill_Block(target, x, y, Indexed(value));
        }

        bool Marked(const s_surface& target, int32_t x, int32_t y) {
            return x >= 0 && y >= 0 && x < target.width && y < target.height &&
                target.mark[(size_t)y * target.width + x] == generation;
        }

        // Renders the pending stamps of an art draw into one target.
        void Render_Pending_Target(s_surface& target) {
            int32_t x0 = target.width;
            int32_t y0 = target.height;
            int32_t x1 = -1;
            int32_t y1 = -1;
            for (const s_stamp& stamp : pending) {
                if (stamp.target != &target) {
                    continue;
                }
                const int32_t x = stamp.offset % target.width;
                const int32_t y = stamp.offset / target.width;
                x0 = SDL_min(x0, x);
                y0 = SDL_min(y0, y);
                x1 = SDL_max(x1, x);
                y1 = SDL_max(y1, y);
                // Inside the sprite the art goes over the original pixel; at its edges over the
                // HD pixels already there, so the art's own soft outline shows.
                const bool interior = stamp.level < 0 && Marked(target, x - 1, y) && Marked(target, x + 1, y) &&
                    Marked(target, x, y - 1) && Marked(target, x, y + 1);
                if (interior) {
                    Fill_Block(target, x, y, Indexed(stamp.value));
                }
                Apply_Art_Block(target, x, y);
                target.shadow[(size_t)stamp.offset] = stamp.value;
            }
            if (x1 < 0 || target.row_dirty.empty()) {
                return; // images get no fringe: their transparent pixels are never drawn
            }
            // Fringe: the art's soft edge past the original outline, over unwritten neighbours.
            int32_t clip_x1 = 0;
            int32_t clip_y1 = 0;
            int32_t clip_x2 = target.width - 1;
            int32_t clip_y2 = target.height - 1;
            if (animate::clipping_flag == 1) {
                clip_x1 = SDL_max(clip_x1, (int32_t)video::screen_window_x1);
                clip_y1 = SDL_max(clip_y1, (int32_t)video::screen_window_y1);
                clip_x2 = SDL_min(clip_x2, (int32_t)video::screen_window_x2);
                clip_y2 = SDL_min(clip_y2, (int32_t)video::screen_window_y2);
            }
            for (int32_t y = SDL_max(y0 - 1, clip_y1); y <= SDL_min(y1 + 1, clip_y2); ++y) {
                for (int32_t x = SDL_max(x0 - 1, clip_x1); x <= SDL_min(x1 + 1, clip_x2); ++x) {
                    if (Marked(target, x, y)) {
                        continue;
                    }
                    if (Marked(target, x - 1, y) || Marked(target, x + 1, y) || Marked(target, x, y - 1) ||
                        Marked(target, x, y + 1)) {
                        Apply_Art_Block(target, x, y);
                    }
                }
            }
        }

        void Flush_Pending() {
            if (pending.empty()) {
                return;
            }
            s_scoped_timer timer(TIMER_RENDER);
            while (!pending.empty()) {
                s_surface* target = pending.front().target;
                Render_Pending_Target(*target);
                pending.erase(std::remove_if(pending.begin(), pending.end(), [target](const s_stamp& stamp) { return stamp.target == target; }),
                    pending.end());
            }
        }

        void Start_Draw() {
            Flush_Pending();
            draw.source = SOURCE_NONE;
            draw.frame = nullptr;
            draw.glyph = false;
            draw.glassed = false;
            draw.remapped = false;
            draw.shade.clear();
            draw.image = nullptr;
            draw.name.clear();
            if (++generation == 0) {
                generation = 1;
            }
        }

        void Stamp(s_surface& target, int32_t offset, uint8_t value, int32_t level) {
            if (probe_x >= 0) {
                Probe(target, offset, value, "stamp");
            }
            if (draw.source == SOURCE_ART) {
                Ensure_Hd(target);
                target.mark[(size_t)offset] = generation;
                pending.push_back({&target, offset, value, (int8_t)level});
                return;
            }
            Render_Now(target, offset, value, level);
        }

        // Settles the area the current art draw can touch, fringe included.
        void Settle_Art_Area() {
            const s_hd_frame* frame = draw.frame;
            const int32_t left = draw.x + frame->origin_x;
            const int32_t top = draw.y + frame->origin_y;
            Settle_Draw_Area(left - 1, top - 1, left + (frame->width + S - 1) / S + 1, top + (frame->height + S - 1) / S + 1);
        }

        // Sets up the current draw for `anim` drawn with its top-left at (x, y).
        void Begin_Source(const s_animation_header* anim, int16_t x, int16_t y) {
            Start_Draw();
            if (!enabled || anim == nullptr) {
                return;
            }
            if (!capture_dir.empty() || probe_x >= 0 || stats_enabled) {
                const auto name = stats_names.find(anim);
                if (name != stats_names.end()) {
                    draw.name = name->second;
                    if (!capture_dir.empty()) {
                        Capture_Palette(name->second);
                    }
                }
            }
            const auto image = images.find(anim);
            if (image != images.end() && !image->second.hd.empty() && image->second.width == anim->width &&
                image->second.height == anim->height) {
                draw.source = SOURCE_IMAGE;
                draw.image = &image->second;
                draw.image_x = x;
                draw.image_y = y;
                Settle_Draw_Area(x, y, x + anim->width, y + anim->height);
                return;
            }
            const auto found = registrations.find(anim);
            if (found == registrations.end()) {
                if (stats_enabled) {
                    Stats_Missing(!draw.name.empty() ? draw.name
                                                     : "unregistered:" + std::to_string(anim->width) + "x" + std::to_string(anim->height));
                }
                return;
            }
            // The buffer may have been reused for other data since it was loaded; the header
            // (dimensions, frame count, flags) must still match, apart from the frame counter.
            uint8_t header[12];
            memcpy(header, anim, sizeof(header));
            memcpy(header + 4, found->second.header + 4, 2);
            if (memcmp(header, found->second.header, sizeof(header)) != 0) {
                return;
            }
            s_hd_asset* asset = assets[found->second.asset];
            if (!asset->loaded) {
                Load_Asset(asset);
            }
            if (asset->has_source_hash && asset->source_hash != found->second.data_hash) {
                if (stats_enabled) {
                    Stats_Missing("source-mismatch:" + draw.name);
                }
                return;
            }
            const s_hd_frame* frame = Resolve_Frame(asset, anim->current_frame);
            if (frame == nullptr) {
                return;
            }
            draw.source = SOURCE_ART;
            draw.frame = frame;
            draw.x = x;
            draw.y = y;
            draw.glassed = asset->glassed || (anim->flags & ANIMATION_FLAG_GLASSED) != 0;
            Settle_Art_Area();
        }

        uint32_t Settle(s_surface& surface, const uint8_t* page, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
            if (surface.hd.empty() || page == nullptr) {
                return 0;
            }
            x0 = SDL_max(x0, 0);
            y0 = SDL_max(y0, 0);
            x1 = SDL_min(x1, surface.width);
            y1 = SDL_min(y1, surface.height);
            uint32_t settled = 0;
            for (int32_t y = y0; y < y1; ++y) {
                const size_t row = (size_t)y * surface.width;
                for (int32_t x = x0; x < x1; ++x) {
                    if (surface.shadow[row + x] != page[row + x]) {
                        const uint8_t value = page[row + x];
                        surface.shadow[row + x] = value;
                        Fill_Block(surface, x, y, debug_sd ? Literal(Mix(0xFF0000u, Base_Rgb(value), 96)) : Indexed(value));
                        settled++;
                    }
                }
            }
            return settled;
        }

    } // namespace

    void Initialize() {
        if (initialized) {
            return;
        }
        initialized = true;
        enabled = vfs::File_Exists("hd/pack.json") != 0;
        const char* stats = SDL_getenv("ORION2RE_HD_STATS");
        stats_enabled = enabled && stats != nullptr;
        const char* debug = SDL_getenv("ORION2RE_HD_DEBUG");
        debug_sd = enabled && debug != nullptr && (strcmp(debug, "sd") == 0 || strcmp(debug, "stale") == 0);
        const char* capture = SDL_getenv("ORION2RE_HD_CAPTURE");
        if (capture != nullptr && capture[0] != '\0') {
            capture_dir = capture;
            (void)SDL_CreateDirectory(capture_dir.c_str());
        }
        const char* probe = SDL_getenv("ORION2RE_HD_PROBE");
        if (probe != nullptr) {
            (void)sscanf(probe, "%d,%d", &probe_x, &probe_y);
        }
        if (stats_enabled && atoi(stats) > 1) {
            stats_interval = (uint32_t)atoi(stats);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "hd: %s", enabled ? "HD sprite pack found" : "no HD sprite pack");
        if (enabled && video::g_page_off.pixels != nullptr) {
            Allocate_Planes(video::g_page_off.width, video::g_page_off.height);
        }
    }

    bool Enabled() {
        return enabled;
    }

    void Register_Load(const void* data, int32_t size, const char* lbx_name, int32_t entry) {
        if (!initialized) {
            Initialize();
        }
        if (!enabled || data == nullptr || lbx_name == nullptr || size < (int32_t)sizeof(s_registration::header)) {
            return;
        }
        if (stats_enabled || !capture_dir.empty() || probe_x >= 0) {
            char key[64];
            snprintf(key, sizeof(key), "%s#%03d", lbx_name, (int)entry);
            for (char* c = key; *c != '\0'; ++c) {
                if (*c >= 'a' && *c <= 'z') {
                    *c = (char)(*c - 'a' + 'A');
                }
            }
            stats_names[data] = key;
        }
        s_registration registration = {};
        registration.asset = Find_Or_Create_Asset(lbx_name, entry);
        registration.size = size;
        // Fingerprint of the loaded bytes: a mod that replaces this entry with different art
        // must not get HD art made from the original.
        uint32_t hash = 2166136261u;
        const uint8_t* bytes = (const uint8_t*)data;
        for (int32_t i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 16777619u;
        }
        registration.data_hash = hash;
        memcpy(registration.header, data, sizeof(registration.header));
        if (registration.asset < 0) {
            registrations.erase(data);
            return;
        }
        registrations[data] = registration;
    }

    void Begin_Draw(const s_animation_header* anim, int16_t x, int16_t y) {
        Begin_Source(anim, x, y);
    }

    void Begin_Remap_Draw(const s_animation_header* anim, int16_t x, int16_t y, const uint8_t* remap) {
        Begin_Source(anim, x, y);
        if (remap == nullptr) {
            return;
        }
        if (draw.source == SOURCE_IMAGE) {
            draw.source = SOURCE_NONE; // remapped images keep their original look
            return;
        }
        if (draw.source == SOURCE_ART) {
            memcpy(draw.remap, remap, sizeof(draw.remap));
            draw.remapped = true;
        }
    }

    void Begin_Masked_Remap_Draw(const s_animation_header* anim, int16_t x, int16_t y, const uint8_t* remap, const int8_t* mask, const int8_t* gray_percentages, int32_t level_count) {
        Begin_Remap_Draw(anim, x, y, remap);
        if (draw.source != SOURCE_ART || mask == nullptr || anim == nullptr) {
            return;
        }
        draw.shade_width = anim->width;
        draw.shade_height = anim->height;
        draw.shade.resize((size_t)anim->width * anim->height);
        for (size_t i = 0; i < draw.shade.size(); ++i) {
            const int32_t level = mask[i];
            // Level 0 is drawn unshaded; level n darkens by gray_percentages[n] percent.
            const int32_t percent = level <= 0 || level >= level_count ? 0 : gray_percentages[level];
            draw.shade[i] = (uint8_t)SDL_clamp(100 - percent, 0, 100);
        }
    }

    void Begin_Draw_To_Bitmap(const s_animation_header* src, int16_t x, int16_t y, const void* target_bitmap) {
        (void)target_bitmap;
        Begin_Source(src, x, y);
    }

    void Begin_Glyph(const char* font_file, int16_t font_index, uint8_t code, int16_t x, int16_t y, const uint8_t* colors) {
        if (!initialized) {
            Initialize();
        }
        Start_Draw();
        if (!enabled || font_file == nullptr || colors == nullptr) {
            return;
        }
        const int32_t slot = Find_Glyph_Font(font_file, font_index);
        if (slot < 0) {
            if (stats_enabled) {
                Stats_Missing(std::string("font:") + font_file + "/" + std::to_string(font_index));
            }
            return;
        }
        s_hd_glyph_font* font = glyph_fonts[slot];
        if (font->state[code] == 0) {
            char name[16];
            snprintf(name, sizeof(name), "/%03d.png", (int)code);
            font->state[code] = Load_Frame_Png(font->directory + name, &font->glyphs[code]) ? 1 : 2;
        }
        if (font->state[code] != 1) {
            stats_glyph_missing_art++;
            if (stats_enabled) {
                Stats_Missing(std::string("glyph:") + font_file + "/" + std::to_string(font_index) + "/" + std::to_string(code));
            }
            return;
        }
        draw.source = SOURCE_ART;
        draw.frame = &font->glyphs[code];
        draw.glyph = true;
        draw.x = x;
        draw.y = y;
        memcpy(draw.colors, colors, sizeof(draw.colors));
        Settle_Art_Area();
        if (probe_x >= 0) {
            draw.name = std::string("glyph:") + font_file + "/" + std::to_string(font_index) + "/" + std::to_string(code);
        }
    }

    void Begin_Asset_Draw(const char* lbx_name, int32_t entry, int16_t frame, int16_t x, int16_t y) {
        if (!initialized) {
            Initialize();
        }
        Start_Draw();
        if (!enabled || lbx_name == nullptr) {
            return;
        }
        const int32_t asset_index = Find_Or_Create_Asset(lbx_name, entry);
        if (asset_index < 0) {
            return;
        }
        s_hd_asset* asset = assets[asset_index];
        if (!asset->loaded) {
            Load_Asset(asset);
        }
        const s_hd_frame* art = Resolve_Frame(asset, frame);
        if (art == nullptr) {
            return;
        }
        draw.source = SOURCE_ART;
        draw.frame = art;
        draw.x = x;
        draw.y = y;
        draw.glassed = asset->glassed;
        Settle_Art_Area();
        if (probe_x >= 0) {
            draw.name = asset->directory;
        }
    }

    void End_Draw() {
        Start_Draw();
    }

    void Stamp_Run(const uint8_t* dst, int32_t count) {
        Stamp_Page(video::current_video_page, dst, count);
    }

    void Stamp_Page(const uint8_t* page, const uint8_t* dst, int32_t count) {
        if (!enabled || count <= 0) {
            return;
        }
        s_surface* target = Page_Surface(page);
        if (target == nullptr) {
            return;
        }
        const ptrdiff_t offset = dst - page;
        if (offset < 0 || offset + count > (ptrdiff_t)target->width * target->height) {
            return;
        }
        for (int32_t i = 0; i < count; ++i) {
            Stamp(*target, (int32_t)offset + i, dst[i], -1);
        }
    }

    void Overwrite_Rect(const uint8_t* page, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
        if (!enabled) {
            return;
        }
        s_surface* target = Page_Surface(page);
        if (target == nullptr) {
            return;
        }
        Flush_Pending();
        x1 = SDL_max(x1, 0);
        y1 = SDL_max(y1, 0);
        x2 = SDL_min(x2, target->width - 1);
        y2 = SDL_min(y2, target->height - 1);
        for (int32_t y = y1; y <= y2; ++y) {
            const size_t row = (size_t)y * target->width;
            for (int32_t x = x1; x <= x2; ++x) {
                target->shadow[row + x] = page[row + x];
                Fill_Block(*target, x, y, Indexed(page[row + x]));
            }
        }
    }

    void Stamp_Glass(const uint8_t* dst, uint8_t background_value, int32_t level) {
        (void)background_value;
        if (!enabled) {
            return;
        }
        s_surface* target = Page_Surface(video::current_video_page);
        if (target == nullptr) {
            return;
        }
        const ptrdiff_t offset = dst - video::current_video_page;
        if (offset < 0 || offset >= (ptrdiff_t)target->width * target->height) {
            return;
        }
        Stamp(*target, (int32_t)offset, *dst, level);
    }

    void Bitmap_Reset(const void* bitmap, int32_t width, int32_t height) {
        if (!enabled || bitmap == nullptr || width <= 0 || height <= 0) {
            return;
        }
        Flush_Pending();
        s_surface& image = images[bitmap];
        image.width = width;
        image.height = height;
        image.hd.clear();
        image.shadow.clear();
        image.mark.clear();
    }

    void Stamp_Bitmap(const void* bitmap, int32_t index, uint8_t value) {
        if (!enabled) {
            return;
        }
        const auto found = images.find(bitmap);
        if (found == images.end()) {
            return;
        }
        s_surface& image = found->second;
        if (index < 0 || index >= image.width * image.height) {
            return;
        }
        if (draw.source == SOURCE_IMAGE && draw.image != &image) {
            Ensure_Hd(image);
        }
        Stamp(image, index, value, -1);
    }

    void Bitmap_Copy(const void* dst, const void* src) {
        if (!enabled || dst == src) {
            return;
        }
        Flush_Pending();
        const auto from = images.find(src);
        auto to = images.find(dst);
        if (to == images.end()) {
            return;
        }
        s_surface& target = to->second;
        if (from == images.end() || from->second.hd.empty() || from->second.width != target.width ||
            from->second.height != target.height) {
            target.hd.clear();
            target.shadow.clear();
            target.mark.clear();
            return;
        }
        target.hd = from->second.hd;
        target.shadow = from->second.shadow;
        target.mark.assign(target.shadow.size(), 0u);
    }

    void Bitmap_Capture(const void* bitmap, int32_t x1, int32_t y1, int32_t width, int32_t height) {
        if (!enabled) {
            return;
        }
        Flush_Pending();
        auto to = images.find(bitmap);
        s_surface* page = Page_Surface(video::current_video_page);
        if (to == images.end() || page == nullptr || to->second.width != width || to->second.height != height) {
            return;
        }
        s_surface& image = to->second;
        Ensure_Hd(image);
        const uint8_t* pixels = video::current_video_page;
        const size_t page_stride = (size_t)page->width * S;
        const size_t image_stride = (size_t)width * S;
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const int32_t px = x1 + x;
                const int32_t py = y1 + y;
                const size_t i = (size_t)y * width + x;
                if (px < 0 || py < 0 || px >= page->width || py >= page->height) {
                    image.shadow[i] = SHADOW_NONE;
                    continue;
                }
                const size_t p = (size_t)py * page->width + px;
                image.shadow[i] = page->shadow[p] == pixels[p] ? page->shadow[p] : SHADOW_NONE;
                const uint32_t* src = &page->hd[(size_t)py * S * page_stride + (size_t)px * S];
                uint32_t* dst = &image.hd[(size_t)y * S * image_stride + (size_t)x * S];
                for (int32_t sy = 0; sy < S; ++sy) {
                    memcpy(dst + sy * image_stride, src + sy * page_stride, S * sizeof(uint32_t));
                }
            }
        }
    }

    void Allocate_Planes(int32_t width, int32_t height) {
        if (!enabled) {
            return;
        }
        Free_Planes();
        Allocate_Page(page_off, width, height);
        Allocate_Page(page_back, width, height);
        Allocate_Page(page_published, width, height);
        Allocate_Page(page_present, width, height);
    }

    void Free_Planes() {
        pending.clear();
        Free_Surface(page_off);
        Free_Surface(page_back);
        Free_Surface(page_published);
        Free_Surface(page_present);
    }

    void Copy_Plane(const uint8_t* src_page, const uint8_t* dst_page) {
        s_surface* src = Page_Surface(src_page);
        s_surface* dst = Page_Surface(dst_page);
        if (src == nullptr || dst == nullptr || src == dst) {
            return;
        }
        Flush_Pending();
        s_scoped_timer timer(TIMER_COPY);
        dst->hd = src->hd;
        dst->shadow = src->shadow;
        std::fill(dst->row_dirty.begin(), dst->row_dirty.end(), (uint8_t)1);
    }

    void Copy_Plane_Span(const uint8_t* src_page, const uint8_t* dst_page, int32_t offset, int32_t count) {
        s_surface* src = Page_Surface(src_page);
        s_surface* dst = Page_Surface(dst_page);
        if (src == nullptr || dst == nullptr || src == dst || count <= 0 || offset < 0 ||
            offset + count > src->width * src->height) {
            return;
        }
        Flush_Pending();
        s_scoped_timer timer(TIMER_COPY);
        const size_t stride = (size_t)src->width * S;
        while (count > 0) {
            const int32_t x = offset % src->width;
            const int32_t y = offset / src->width;
            const int32_t run = SDL_min(count, src->width - x);
            memcpy(&dst->shadow[(size_t)offset], &src->shadow[(size_t)offset], (size_t)run * sizeof(uint16_t));
            for (int32_t sy = 0; sy < S; ++sy) {
                const size_t at = ((size_t)y * S + sy) * stride + (size_t)x * S;
                memcpy(&dst->hd[at], &src->hd[at], (size_t)run * S * sizeof(uint32_t));
            }
            dst->row_dirty[(size_t)y] = 1;
            offset += run;
            count -= run;
        }
    }

    void Publish_Plane(const uint8_t* published_page_pixels) {
        (void)published_page_pixels;
        if (page_off.hd.empty()) {
            return;
        }
        Flush_Pending();
        s_scoped_timer timer(TIMER_PUBLISH);
        // Settle pixels changed by code without hooks: the original pixel, enlarged.
        const uint32_t settled = Settle(page_off, video::g_page_off.pixels, 0, 0, page_off.width, page_off.height);
        if (stats_enabled) {
            stats_settled_pixels += settled;
            if (++stats_publishes % 100 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "hd stats: %u pixels settled to plain scaling over the last 100 frames; per frame: render %.2f ms, "
                    "page copies %.2f ms, publish %.2f ms",
                    stats_settled_pixels,
                    (double)stats_timer_ns[TIMER_RENDER] / 1e8,
                    (double)stats_timer_ns[TIMER_COPY] / 1e8,
                    (double)stats_timer_ns[TIMER_PUBLISH] / 1e8);
                stats_settled_pixels = 0;
                memset(stats_timer_ns, 0, sizeof(stats_timer_ns));
            }
        }
        const size_t row_size = (size_t)page_off.width * S * S;
        for (int32_t y = 0; y < page_off.height; ++y) {
            if (page_off.row_dirty[(size_t)y] == 0) {
                continue;
            }
            memcpy(&page_published.hd[(size_t)y * row_size], &page_off.hd[(size_t)y * row_size], row_size * sizeof(uint32_t));
            page_off.row_dirty[(size_t)y] = 0;
            page_published.row_dirty[(size_t)y] = 1;
        }
        // Literal pixels follow screen fades: compare the shown palette with the base palette.
        uint64_t base = 0;
        uint64_t shown = 0;
        for (int32_t i = 1; i < 256; ++i) {
            const s_palette_entry& entry = video::current_palette[i];
            base += (uint64_t)((entry.r & 0x3F) + (entry.g & 0x3F) + (entry.b & 0x3F)) * 4;
            shown += (uint64_t)video::g_palette_entries[i].r + video::g_palette_entries[i].g + video::g_palette_entries[i].b;
        }
        published_fade = base == 0 ? 256u : (uint32_t)SDL_min((uint64_t)256, shown * 256 / base);
    }

    void Snapshot_Plane() {
        if (page_published.hd.empty()) {
            return;
        }
        const size_t row_size = (size_t)page_published.width * S * S;
        for (int32_t y = 0; y < page_published.height; ++y) {
            if (page_published.row_dirty[(size_t)y] == 0) {
                continue;
            }
            memcpy(&page_present.hd[(size_t)y * row_size], &page_published.hd[(size_t)y * row_size], row_size * sizeof(uint32_t));
            page_published.row_dirty[(size_t)y] = 0;
            page_present.row_dirty[(size_t)y] = 1;
        }
        present_fade = published_fade;
    }

    bool Resolve(const video::s_palette_color* palette, uint32_t* output, int32_t* row_first, int32_t* row_last) {
        static uint32_t last_palette[256];
        static uint32_t last_fade = 0xFFFFFFFFu;
        if (page_present.hd.empty()) {
            return false;
        }
        uint32_t colors[256];
        for (int32_t i = 0; i < 256; ++i) {
            colors[i] = ((uint32_t)palette[i].r << 16) | ((uint32_t)palette[i].g << 8) | palette[i].b;
        }
        const bool all = memcmp(colors, last_palette, sizeof(colors)) != 0 || present_fade != last_fade;
        memcpy(last_palette, colors, sizeof(colors));
        last_fade = present_fade;
        const uint32_t fade = present_fade;
        const size_t row_size = (size_t)page_present.width * S * S;
        std::vector<int32_t> rows;
        for (int32_t y = 0; y < page_present.height; ++y) {
            if (all || page_present.row_dirty[(size_t)y] != 0) {
                page_present.row_dirty[(size_t)y] = 0;
                rows.push_back(y);
            }
        }
        if (rows.empty()) {
            return false;
        }
        const int32_t first = rows.front();
        const int32_t last = rows.back();
        auto resolve_rows = [&](size_t begin, size_t end) {
            for (size_t r = begin; r < end; ++r) {
                const int32_t y = rows[r];
                const uint32_t* src = &page_present.hd[(size_t)y * row_size];
                uint32_t* dst = output + (size_t)y * row_size;
                for (size_t i = 0; i < row_size; ++i) {
                    const uint32_t pixel = src[i];
                    uint32_t rgb;
                    if ((pixel & HD_LITERAL) != 0) {
                        rgb = pixel & 0xFFFFFFu;
                        if (fade < 256) {
                            rgb = ((((rgb >> 16) & 0xFF) * fade >> 8) << 16) | ((((rgb >> 8) & 0xFF) * fade >> 8) << 8) |
                                ((rgb & 0xFF) * fade >> 8);
                        }
                    } else {
                        const uint32_t alpha = (pixel >> 16) & 0xFF;
                        rgb = colors[pixel & 0xFF];
                        if (alpha < 255) {
                            rgb = Mix(rgb, (pixel & HD_CLEAR) != 0 ? 0u : colors[(pixel >> 8) & 0xFF], alpha);
                        }
                    }
                    dst[i] = 0xFF000000u | rgb;
                }
            }
        };
        // Whole-screen updates (palette fades and cycling) are split across threads.
        const size_t workers = rows.size() >= 64 ? SDL_min((size_t)8, (size_t)SDL_max(1, SDL_GetNumLogicalCPUCores() / 2)) : 1;
        if (workers <= 1) {
            resolve_rows(0, rows.size());
        } else {
            std::vector<std::thread> threads;
            const size_t chunk = (rows.size() + workers - 1) / workers;
            for (size_t w = 1; w < workers; ++w) {
                const size_t begin = SDL_min(rows.size(), w * chunk);
                const size_t end = SDL_min(rows.size(), begin + chunk);
                threads.emplace_back(resolve_rows, begin, end);
            }
            resolve_rows(0, SDL_min(rows.size(), chunk));
            for (std::thread& thread : threads) {
                thread.join();
            }
        }
        *row_first = first * S;
        *row_last = (last + 1) * S;
        return true;
    }

} // namespace hd_sprites
