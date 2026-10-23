#include "pch.h"

#include "game/hd_sprites.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "yyjson.h"

namespace hd_sprites {

#define HD_COMMAND_BITS 20
#define HD_COMMAND_COUNT (1u << HD_COMMAND_BITS)
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
            bool glassed;
            int32_t scale;
            int32_t frame_count;
            s_hd_frame* frames;
        };

        struct s_registration {
            int32_t asset;
            uint8_t header[12];
            int32_t size;
        };

        enum e_command_kind : uint8_t {
            COMMAND_SPRITE = 0,
            COMMAND_GLYPH = 1,
        };

        struct s_command {
            int32_t asset; // sprite asset, or glyph font slot
            int16_t frame; // sprite frame, or character code
            int16_t x;
            int16_t y;
            e_command_kind kind;
            uint8_t colors[8]; // glyph role -> palette index
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

        uint32_t* plane_off;
        uint32_t* plane_back;
        uint32_t* plane_published;
        int32_t plane_width;
        int32_t plane_height;

        // Written by the game thread, read by the presenter. A slot reused before a frame that
        // references it is presented only matters if the recorded pixel value also matches.
        s_command commands[HD_COMMAND_COUNT];
        uint32_t next_command = 1;
        uint32_t current_command;

        // Assets are appended and never moved or freed, so the presenter can read them while
        // the game thread registers new ones.
        s_hd_asset* assets[HD_MAX_ASSETS];
        int32_t asset_count;
        std::unordered_map<std::string, int32_t> asset_by_key;
        std::unordered_map<const void*, s_registration> registrations;

        s_hd_glyph_font* glyph_fonts[HD_MAX_GLYPH_FONTS];
        int32_t glyph_font_count;
        std::unordered_map<std::string, int32_t> glyph_font_by_key;

        uint32_t* Owner_Plane_For(const uint8_t* page) {
            if (page == nullptr) {
                return nullptr;
            }
            if (page == video::g_page_off.pixels) {
                return plane_off;
            }
            if (page == video::g_page_back.pixels) {
                return plane_back;
            }
            return nullptr;
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

    } // namespace

    void Initialize() {
        if (initialized) {
            return;
        }
        initialized = true;
        enabled = vfs::File_Exists("hd/pack.json") != 0;
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
        s_registration registration = {};
        registration.asset = Find_Or_Create_Asset(lbx_name, entry);
        registration.size = size;
        memcpy(registration.header, data, sizeof(registration.header));
        if (registration.asset < 0) {
            registrations.erase(data);
            return;
        }
        registrations[data] = registration;
    }

    void Begin_Draw(const s_animation_header* anim, int16_t x, int16_t y) {
        current_command = 0;
        if (!enabled || anim == nullptr) {
            return;
        }
        const auto found = registrations.find(anim);
        if (found == registrations.end()) {
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
        if (Resolve_Frame(asset, anim->current_frame) == nullptr) {
            return;
        }
        uint32_t id = next_command++ & (HD_COMMAND_COUNT - 1);
        if (id == 0) {
            id = next_command++ & (HD_COMMAND_COUNT - 1);
        }
        commands[id] = {found->second.asset, anim->current_frame, x, y, COMMAND_SPRITE, {}};
        current_command = id;
    }

    void Begin_Glyph(const char* font_file, int16_t font_index, uint8_t code, int16_t x, int16_t y, const uint8_t* colors) {
        current_command = 0;
        if (!initialized) {
            Initialize();
        }
        if (!enabled || font_file == nullptr || colors == nullptr) {
            return;
        }
        const int32_t slot = Find_Glyph_Font(font_file, font_index);
        if (slot < 0) {
            return;
        }
        s_hd_glyph_font* font = glyph_fonts[slot];
        if (font->state[code] == 0) {
            char name[16];
            snprintf(name, sizeof(name), "/%03d.png", (int)code);
            font->state[code] = Load_Frame_Png(font->directory + name, &font->glyphs[code]) ? 1 : 2;
        }
        if (font->state[code] != 1) {
            return;
        }
        uint32_t id = next_command++ & (HD_COMMAND_COUNT - 1);
        if (id == 0) {
            id = next_command++ & (HD_COMMAND_COUNT - 1);
        }
        s_command command = {slot, (int16_t)code, x, y, COMMAND_GLYPH, {}};
        memcpy(command.colors, colors, sizeof(command.colors));
        commands[id] = command;
        current_command = id;
    }

    void End_Draw() {
        current_command = 0;
    }

    void Stamp_Run(const uint8_t* dst, int32_t count) {
        if (!enabled || count <= 0) {
            return;
        }
        uint32_t* plane = Owner_Plane_For(video::current_video_page);
        if (plane == nullptr) {
            return;
        }
        const ptrdiff_t offset = dst - video::current_video_page;
        const ptrdiff_t limit = (ptrdiff_t)plane_width * plane_height;
        if (offset < 0 || offset + count > limit) {
            return;
        }
        uint32_t* owner = plane + offset;
        if (current_command == 0) {
            memset(owner, 0, (size_t)count * sizeof(uint32_t));
            return;
        }
        const uint32_t tag = current_command << 8;
        for (int32_t i = 0; i < count; ++i) {
            owner[i] = tag | dst[i];
        }
    }

    void Allocate_Planes(int32_t width, int32_t height) {
        if (!enabled) {
            return;
        }
        Free_Planes();
        const size_t count = (size_t)width * (size_t)height;
        plane_off = (uint32_t*)calloc(count, sizeof(uint32_t));
        plane_back = (uint32_t*)calloc(count, sizeof(uint32_t));
        plane_published = (uint32_t*)calloc(count, sizeof(uint32_t));
        if (plane_off == nullptr || plane_back == nullptr || plane_published == nullptr) {
            Free_Planes();
            enabled = false;
            return;
        }
        plane_width = width;
        plane_height = height;
    }

    void Free_Planes() {
        free(plane_off);
        free(plane_back);
        free(plane_published);
        plane_off = plane_back = plane_published = nullptr;
        plane_width = plane_height = 0;
    }

    void Copy_Plane(const uint8_t* src_page, const uint8_t* dst_page) {
        uint32_t* src = Owner_Plane_For(src_page);
        uint32_t* dst = Owner_Plane_For(dst_page);
        if (src != nullptr && dst != nullptr && src != dst) {
            memcpy(dst, src, (size_t)plane_width * plane_height * sizeof(uint32_t));
        }
    }

    void Publish_Plane(const uint8_t* published_page_pixels) {
        (void)published_page_pixels;
        if (plane_off != nullptr && plane_published != nullptr) {
            memcpy(plane_published, plane_off, (size_t)plane_width * plane_height * sizeof(uint32_t));
        }
    }

    int32_t Compose(const s_compose_input* in, uint32_t* output) {
        const int32_t s = HD_SPRITES_SCALE;
        const int32_t out_width = in->width * s;
        const int32_t out_height = in->height * s;

        // Base image: the regular scaler output resampled to 4x.
        for (int32_t oy = 0; oy < out_height; ++oy) {
            const uint32_t* base_row = in->base + (size_t)(oy * in->base_factor / s) * in->base_stride;
            uint32_t* out_row = output + (size_t)oy * out_width;
            if (in->base_factor == s) {
                memcpy(out_row, base_row, (size_t)out_width * sizeof(uint32_t));
                continue;
            }
            for (int32_t ox = 0; ox < out_width; ++ox) {
                out_row[ox] = base_row[ox * in->base_factor / s];
            }
        }

        if (!enabled || plane_published == nullptr || plane_width != in->width || plane_height != in->height) {
            return 0;
        }

        uint32_t palette[256];
        for (int32_t i = 0; i < 256; ++i) {
            palette[i] = 0xFF000000u | ((uint32_t)in->palette[i].r << 16) | ((uint32_t)in->palette[i].g << 8) | in->palette[i].b;
        }

        int32_t replaced = 0;
        for (int32_t y = 0; y < in->height; ++y) {
            for (int32_t x = 0; x < in->width; ++x) {
                const size_t i = (size_t)y * in->width + x;
                const uint32_t owner = plane_published[i];
                if (owner == 0 || (owner & 0xFF) != in->page[i]) {
                    continue;
                }
                if (in->cursor_x >= 0 && x >= in->cursor_x && x < in->cursor_x + in->cursor_size &&
                    y >= in->cursor_y && y < in->cursor_y + in->cursor_size) {
                    continue;
                }
                const s_command command = commands[owner >> 8];
                const bool glyph = command.kind == COMMAND_GLYPH;
                const s_hd_asset* asset = nullptr;
                const s_hd_frame* frame = nullptr;
                if (glyph) {
                    frame = Glyph_Frame(command.asset, command.frame);
                } else if (command.asset >= 0 && command.asset < asset_count) {
                    asset = assets[command.asset];
                    frame = Resolve_Frame(asset, command.frame);
                }
                if (frame == nullptr) {
                    continue;
                }
                const int32_t local_x = (x - command.x - frame->origin_x) * s;
                const int32_t local_y = (y - command.y - frame->origin_y) * s;
                if (local_x < 0 || local_y < 0 || local_x + s > frame->width || local_y + s > frame->height) {
                    continue;
                }
                for (int32_t sy = 0; sy < s; ++sy) {
                    const size_t src_row = (size_t)(local_y + sy) * frame->width + local_x;
                    uint32_t* out = output + (size_t)(y * s + sy) * out_width + (size_t)x * s;
                    for (int32_t sx = 0; sx < s; ++sx) {
                        const uint8_t alpha = frame->alpha[src_row + sx];
                        uint8_t index = frame->index[src_row + sx];
                        if (alpha == 0) {
                            continue;
                        }
                        if (glyph) {
                            // Glyph art stores colour roles; the recorded style supplies indices.
                            if (index < 1 || index > 8) {
                                continue;
                            }
                            index = command.colors[index - 1];
                        } else if (asset->glassed && index >= HD_GLASS_FIRST) {
                            continue;
                        }
                        const uint32_t color = palette[index];
                        if (alpha == 255) {
                            out[sx] = color;
                            continue;
                        }
                        const uint32_t under = out[sx];
                        const uint32_t inv = 255u - alpha;
                        const uint32_t r = (((color >> 16) & 0xFF) * alpha + ((under >> 16) & 0xFF) * inv) / 255u;
                        const uint32_t g = (((color >> 8) & 0xFF) * alpha + ((under >> 8) & 0xFF) * inv) / 255u;
                        const uint32_t b = ((color & 0xFF) * alpha + (under & 0xFF) * inv) / 255u;
                        out[sx] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                }
                ++replaced;
            }
        }
        return replaced;
    }

} // namespace hd_sprites
