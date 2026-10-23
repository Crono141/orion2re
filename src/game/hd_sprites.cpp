#include "pch.h"

#include "game/hd_sprites.h"

#include <algorithm>
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
#define HD_REMAP_TABLES 4096

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
            uint8_t glassed;   // sprite was drawn with the glass flag (it can be set at runtime)
            uint16_t remap;    // remap table, 0 = none
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
        // What lay under glass pixels: (command << 8) | value written before the glass remap.
        uint32_t* under_off;
        uint32_t* under_back;
        uint32_t* under_published;
        // Glass colours (8-bit RGB, intensity 0-100) snapshotted with the published page.
        struct s_glass {
            uint8_t intensity;
            uint8_t r;
            uint8_t g;
            uint8_t b;
        };
        s_glass glass_published[16];
        int32_t plane_width;
        int32_t plane_height;

        // Written by the game thread, read by the presenter. A slot reused before a frame that
        // references it is presented only matters if the recorded pixel value also matches.
        s_command commands[HD_COMMAND_COUNT];
        uint32_t next_command = 1;
        uint32_t current_command;

        // Owner layers of off-screen images, keyed by the image header. Values have the same
        // meaning as page owners, with command positions in the image's own coordinates.
        struct s_bitmap_plane {
            int32_t width;
            int32_t height;
            std::vector<uint32_t> owners;
        };
        std::unordered_map<const void*, s_bitmap_plane> bitmap_planes;

        // When the current draw's source is an image with an owner layer, each written pixel
        // takes its owner from that layer, re-based to where the image is being drawn.
        const s_bitmap_plane* source_plane;
        int32_t source_origin_x;
        int32_t source_origin_y;
        std::unordered_map<uint32_t, uint32_t> translated;

        // Assets are appended and never moved or freed, so the presenter can read them while
        // the game thread registers new ones.
        s_hd_asset* assets[HD_MAX_ASSETS];
        int32_t asset_count;
        std::unordered_map<std::string, int32_t> asset_by_key;
        std::unordered_map<const void*, s_registration> registrations;

        // ORION2RE_HD_STATS: count draws that had no HD art, by asset, and log them
        // periodically. Debug aid for finding coverage gaps.
        bool stats_enabled;

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
        std::unordered_map<const void*, std::string> stats_names;
        std::unordered_map<std::string, uint32_t> stats_missing;
        uint32_t stats_draws;
        uint32_t stats_interval = 20000;
        uint32_t stats_glyph_commands;
        uint32_t stats_glyph_missing_art;
        uint32_t stats_compose_calls;
        bool debug_stale;
        // ORION2RE_HD_PROBE=x,y: log every owner write to that page pixel (debug aid).
        int32_t probe_x = -1;
        int32_t probe_y = -1;

        void Probe_Write(int32_t x, int32_t y, uint32_t owner, uint8_t value, const char* where) {
            if (x != probe_x || y != probe_y) {
                return;
            }
            const s_command& c = commands[owner >> 8];
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "hd probe (%d,%d) %s: value=%u owner=%u kind=%d asset=%d frame=%d at %d,%d current_command=%u plane=%d",
                x,
                y,
                where,
                value,
                owner >> 8,
                owner ? c.kind : -1,
                owner ? c.asset : -1,
                owner ? c.frame : -1,
                owner ? c.x : 0,
                owner ? c.y : 0,
                current_command,
                source_plane != nullptr);
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

        s_hd_glyph_font* glyph_fonts[HD_MAX_GLYPH_FONTS];
        int32_t glyph_font_count;
        std::unordered_map<std::string, int32_t> glyph_font_by_key;

        // Snapshots of remap tables used by Remap_Draw, in a ring (slot 0 unused).
        uint8_t remap_tables[HD_REMAP_TABLES][256];
        uint32_t next_remap_table = 1;

        uint32_t* Under_Plane_For(const uint8_t* page) {
            if (page == nullptr) {
                return nullptr;
            }
            if (page == video::g_page_off.pixels) {
                return under_off;
            }
            if (page == video::g_page_back.pixels) {
                return under_back;
            }
            return nullptr;
        }

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

        // Commands are interned: screens redraw the same things every frame, and giving each
        // redraw a new slot would wrap the ring within minutes, recycling slots that long-lived
        // owners (a border drawn once into the back page) still point at.
        std::unordered_map<std::string, uint32_t> command_ids;
        std::string command_keys[HD_COMMAND_COUNT];

        std::string Command_Key(const s_command& command) {
            char key[22];
            memcpy(key + 0, &command.asset, 4);
            memcpy(key + 4, &command.frame, 2);
            memcpy(key + 6, &command.x, 2);
            memcpy(key + 8, &command.y, 2);
            key[10] = (char)command.kind;
            key[11] = (char)command.glassed;
            memcpy(key + 12, &command.remap, 2);
            memcpy(key + 14, command.colors, 8);
            return std::string(key, sizeof(key));
        }

        uint32_t Alloc_Command(const s_command& command) {
            std::string key = Command_Key(command);
            const auto found = command_ids.find(key);
            if (found != command_ids.end()) {
                return found->second;
            }
            uint32_t id = next_command++ & (HD_COMMAND_COUNT - 1);
            if (id == 0) {
                id = next_command++ & (HD_COMMAND_COUNT - 1);
            }
            if (!command_keys[id].empty()) {
                command_ids.erase(command_keys[id]);
            }
            commands[id] = command;
            command_ids.emplace(key, id);
            command_keys[id] = std::move(key);
            return id;
        }

        // A copy of command `id` moved by the current source origin.
        uint32_t Translate(uint32_t id) {
            const auto found = translated.find(id);
            if (found != translated.end()) {
                return found->second;
            }
            s_command moved = commands[id];
            moved.x = (int16_t)(moved.x + source_origin_x);
            moved.y = (int16_t)(moved.y + source_origin_y);
            const uint32_t new_id = Alloc_Command(moved);
            translated.emplace(id, new_id);
            return new_id;
        }

        void Clear_Draw_State() {
            current_command = 0;
            source_plane = nullptr;
            translated.clear();
        }

        // Owner for a pixel at target coordinates (x, y) written with `value`.
        uint32_t Owner_For(int32_t x, int32_t y, uint8_t value) {
            if (current_command != 0) {
                return (current_command << 8) | value;
            }
            if (source_plane == nullptr) {
                return 0;
            }
            const int32_t sx = x - source_origin_x;
            const int32_t sy = y - source_origin_y;
            if (sx < 0 || sy < 0 || sx >= source_plane->width || sy >= source_plane->height) {
                return 0;
            }
            const uint32_t owner = source_plane->owners[(size_t)sy * source_plane->width + sx];
            if (owner == 0 || (owner & 0xFF) != value) {
                return 0;
            }
            return (Translate(owner >> 8) << 8) | value;
        }

        // Sets up the current draw for `anim` drawn with its top-left at (x, y).
        void Begin_Source(const s_animation_header* anim, int16_t x, int16_t y) {
            Clear_Draw_State();
            if (!enabled || anim == nullptr) {
                return;
            }
            if (!capture_dir.empty()) {
                const auto name = stats_names.find(anim);
                if (name != stats_names.end()) {
                    Capture_Palette(name->second);
                }
            }
            const auto plane = bitmap_planes.find(anim);
            if (plane != bitmap_planes.end() && plane->second.width == anim->width &&
                plane->second.height == anim->height) {
                source_plane = &plane->second;
                source_origin_x = x;
                source_origin_y = y;
                return;
            }
            const auto found = registrations.find(anim);
            if (found == registrations.end()) {
                if (stats_enabled) {
                    const auto name = stats_names.find(anim);
                    Stats_Missing(name != stats_names.end()
                            ? name->second
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
            if (Resolve_Frame(asset, anim->current_frame) == nullptr) {
                return;
            }
            current_command = Alloc_Command({found->second.asset, anim->current_frame, x, y, COMMAND_SPRITE, (uint8_t)((anim->flags & ANIMATION_FLAG_GLASSED) != 0), 0, {}});
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
        debug_stale = enabled && debug != nullptr && strcmp(debug, "stale") == 0;
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
        if (stats_enabled || !capture_dir.empty()) {
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
        if (current_command == 0 || remap == nullptr) {
            return;
        }
        // Reuse the most recent table when it is unchanged (the usual case), so remapped
        // redraws intern to the same command.
        uint32_t table = (next_remap_table - 1) % HD_REMAP_TABLES;
        if (table == 0 || memcmp(remap_tables[table], remap, 256) != 0) {
            table = next_remap_table++ % HD_REMAP_TABLES;
            if (table == 0) {
                table = next_remap_table++ % HD_REMAP_TABLES;
            }
            memcpy(remap_tables[table], remap, 256);
        }
        s_command command = commands[current_command];
        command.remap = (uint16_t)table;
        current_command = Alloc_Command(command);
    }

    void Begin_Draw_To_Bitmap(const s_animation_header* src, int16_t x, int16_t y, const void* target_bitmap) {
        (void)target_bitmap;
        Begin_Source(src, x, y);
    }

    void Begin_Glyph(const char* font_file, int16_t font_index, uint8_t code, int16_t x, int16_t y, const uint8_t* colors) {
        Clear_Draw_State();
        if (!initialized) {
            Initialize();
        }
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
        stats_glyph_commands++;
        s_command command = {slot, (int16_t)code, x, y, COMMAND_GLYPH, 0, 0, {}};
        memcpy(command.colors, colors, sizeof(command.colors));
        current_command = Alloc_Command(command);
    }

    void End_Draw() {
        Clear_Draw_State();
    }

    void Stamp_Run(const uint8_t* dst, int32_t count) {
        Stamp_Page(video::current_video_page, dst, count);
    }

    void Stamp_Page(const uint8_t* page, const uint8_t* dst, int32_t count) {
        if (!enabled || count <= 0) {
            return;
        }
        uint32_t* plane = Owner_Plane_For(page);
        if (plane == nullptr) {
            return;
        }
        const ptrdiff_t offset = dst - page;
        const ptrdiff_t limit = (ptrdiff_t)plane_width * plane_height;
        if (offset < 0 || offset + count > limit) {
            return;
        }
        uint32_t* owner = plane + offset;
        if (current_command == 0 && source_plane == nullptr) {
            memset(owner, 0, (size_t)count * sizeof(uint32_t));
            if (probe_x >= 0) {
                for (int32_t i = 0; i < count; ++i) {
                    Probe_Write((int32_t)((offset + i) % plane_width), (int32_t)((offset + i) / plane_width), 0, dst[i], "clear");
                }
            }
            return;
        }
        int32_t x = (int32_t)(offset % plane_width);
        int32_t y = (int32_t)(offset / plane_width);
        for (int32_t i = 0; i < count; ++i) {
            owner[i] = Owner_For(x, y, dst[i]);
            if (probe_x >= 0) {
                Probe_Write(x, y, owner[i], dst[i], "stamp");
            }
            if (++x == plane_width) {
                x = 0;
                ++y;
            }
        }
    }

    void Copy_Plane_Span(const uint8_t* src_page, const uint8_t* dst_page, int32_t offset, int32_t count) {
        if (!enabled || count <= 0 || offset < 0) {
            return;
        }
        uint32_t* src = Owner_Plane_For(src_page);
        uint32_t* dst = Owner_Plane_For(dst_page);
        if (src == nullptr || dst == nullptr || src == dst || offset + count > plane_width * plane_height) {
            return;
        }
        memcpy(dst + offset, src + offset, (size_t)count * sizeof(uint32_t));
        memcpy(Under_Plane_For(dst_page) + offset, Under_Plane_For(src_page) + offset, (size_t)count * sizeof(uint32_t));
    }

    void Begin_Asset_Draw(const char* lbx_name, int32_t entry, int16_t frame, int16_t x, int16_t y) {
        Clear_Draw_State();
        if (!initialized) {
            Initialize();
        }
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
        if (Resolve_Frame(asset, frame) == nullptr) {
            return;
        }
        current_command = Alloc_Command({asset_index, frame, x, y, COMMAND_SPRITE, 0, 0, {}});
    }

    void Stamp_Glass(const uint8_t* dst, uint8_t background_value) {
        if (!enabled) {
            return;
        }
        uint32_t* plane = Owner_Plane_For(video::current_video_page);
        uint32_t* under = Under_Plane_For(video::current_video_page);
        if (plane == nullptr || under == nullptr) {
            return;
        }
        const ptrdiff_t offset = dst - video::current_video_page;
        if (offset < 0 || offset >= (ptrdiff_t)plane_width * plane_height) {
            return;
        }
        const uint32_t below = plane[offset];
        // Keep the owner of what was underneath only if its record is still valid.
        under[offset] = ((below != 0 && (below & 0xFF) == background_value) ? (below & ~0xFFu) : 0u) | background_value;
        plane[offset] = Owner_For((int32_t)(offset % plane_width), (int32_t)(offset / plane_width), *dst);
    }

    void Bitmap_Reset(const void* bitmap, int32_t width, int32_t height) {
        if (!enabled || bitmap == nullptr || width <= 0 || height <= 0) {
            return;
        }
        s_bitmap_plane& plane = bitmap_planes[bitmap];
        plane.width = width;
        plane.height = height;
        plane.owners.assign((size_t)width * (size_t)height, 0);
    }

    void Stamp_Bitmap(const void* bitmap, int32_t index, uint8_t value) {
        if (!enabled) {
            return;
        }
        const auto found = bitmap_planes.find(bitmap);
        if (found == bitmap_planes.end()) {
            return;
        }
        s_bitmap_plane& plane = found->second;
        if (index < 0 || (size_t)index >= plane.owners.size()) {
            return;
        }
        plane.owners[(size_t)index] = Owner_For(index % plane.width, index / plane.width, value);
    }

    void Bitmap_Copy(const void* dst, const void* src) {
        if (!enabled || dst == src) {
            return;
        }
        const auto from = bitmap_planes.find(src);
        auto to = bitmap_planes.find(dst);
        if (to == bitmap_planes.end()) {
            return;
        }
        if (from == bitmap_planes.end() || from->second.width != to->second.width ||
            from->second.height != to->second.height) {
            std::fill(to->second.owners.begin(), to->second.owners.end(), 0u);
            return;
        }
        to->second.owners = from->second.owners;
    }

    void Bitmap_Capture(const void* bitmap, int32_t x1, int32_t y1, int32_t width, int32_t height) {
        if (!enabled) {
            return;
        }
        auto to = bitmap_planes.find(bitmap);
        const uint32_t* page_plane = Owner_Plane_For(video::current_video_page);
        if (to == bitmap_planes.end() || page_plane == nullptr || to->second.width != width ||
            to->second.height != height) {
            return;
        }
        Clear_Draw_State();
        source_origin_x = -x1;
        source_origin_y = -y1;
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const int32_t px = x1 + x;
                const int32_t py = y1 + y;
                uint32_t owner = 0;
                if (px >= 0 && py >= 0 && px < plane_width && py < plane_height) {
                    const uint32_t page_owner = page_plane[(size_t)py * plane_width + px];
                    if (page_owner != 0) {
                        owner = (Translate(page_owner >> 8) << 8) | (page_owner & 0xFF);
                    }
                }
                to->second.owners[(size_t)y * width + x] = owner;
            }
        }
        Clear_Draw_State();
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
        under_off = (uint32_t*)calloc(count, sizeof(uint32_t));
        under_back = (uint32_t*)calloc(count, sizeof(uint32_t));
        under_published = (uint32_t*)calloc(count, sizeof(uint32_t));
        if (plane_off == nullptr || plane_back == nullptr || plane_published == nullptr || under_off == nullptr ||
            under_back == nullptr || under_published == nullptr) {
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
        free(under_off);
        free(under_back);
        free(under_published);
        plane_off = plane_back = plane_published = nullptr;
        under_off = under_back = under_published = nullptr;
        plane_width = plane_height = 0;
    }

    void Copy_Plane(const uint8_t* src_page, const uint8_t* dst_page) {
        uint32_t* src = Owner_Plane_For(src_page);
        uint32_t* dst = Owner_Plane_For(dst_page);
        if (src != nullptr && dst != nullptr && src != dst) {
            memcpy(dst, src, (size_t)plane_width * plane_height * sizeof(uint32_t));
            memcpy(Under_Plane_For(dst_page), Under_Plane_For(src_page), (size_t)plane_width * plane_height * sizeof(uint32_t));
        }
    }

    void Publish_Plane(const uint8_t* published_page_pixels) {
        (void)published_page_pixels;
        if (plane_off != nullptr && plane_published != nullptr) {
            memcpy(plane_published, plane_off, (size_t)plane_width * plane_height * sizeof(uint32_t));
            memcpy(under_published, under_off, (size_t)plane_width * plane_height * sizeof(uint32_t));
            for (int32_t i = 0; i < 16; ++i) {
                // Glass colours are 6-bit VGA values like the palette.
                glass_published[i].intensity = video::glass_colors[i].intensity;
                glass_published[i].r = (uint8_t)((video::glass_colors[i].r & 0x3F) * 255 / 63);
                glass_published[i].g = (uint8_t)((video::glass_colors[i].g & 0x3F) * 255 / 63);
                glass_published[i].b = (uint8_t)((video::glass_colors[i].b & 0x3F) * 255 / 63);
            }
        }
    }

    namespace {

        uint32_t Blend(uint32_t over, uint32_t under, uint32_t alpha) {
            const uint32_t inv = 255u - alpha;
            const uint32_t r = (((over >> 16) & 0xFF) * alpha + ((under >> 16) & 0xFF) * inv) / 255u;
            const uint32_t g = (((over >> 8) & 0xFF) * alpha + ((under >> 8) & 0xFF) * inv) / 255u;
            const uint32_t b = ((over & 0xFF) * alpha + (under & 0xFF) * inv) / 255u;
            return 0xFF000000u | (r << 16) | (g << 8) | b;
        }

        uint32_t Apply_Glass(uint32_t rgb, int32_t level) {
            const s_glass& glass = glass_published[level & 15];
            if (glass.intensity == 0) {
                return rgb;
            }
            const uint32_t tint = 0xFF000000u | ((uint32_t)glass.r << 16) | ((uint32_t)glass.g << 8) | glass.b;
            return Blend(tint, rgb, glass.intensity >= 100 ? 255u : (uint32_t)glass.intensity * 255u / 100u);
        }

        // HD colour of the art recorded in `under` at sub-pixel (sx, sy) of page pixel (x, y),
        // over the flat original colour of that pixel.
        uint32_t Under_Color(uint32_t under, int32_t x, int32_t y, int32_t sx, int32_t sy, const uint32_t* palette) {
            const uint32_t flat = palette[under & 0xFF];
            const uint32_t id = under >> 8;
            if (id == 0) {
                return flat;
            }
            const s_command& command = commands[id];
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
                return flat;
            }
            const int32_t s = HD_SPRITES_SCALE;
            const int32_t lx = (x - command.x - frame->origin_x) * s + sx;
            const int32_t ly = (y - command.y - frame->origin_y) * s + sy;
            if (lx < 0 || ly < 0 || lx >= frame->width || ly >= frame->height) {
                return flat;
            }
            const size_t i = (size_t)ly * frame->width + lx;
            uint8_t index = frame->index[i];
            const uint8_t alpha = frame->alpha[i];
            if (alpha == 0) {
                return flat;
            }
            if (glyph) {
                if (index < 1 || index > 8) {
                    return flat;
                }
                index = command.colors[index - 1];
            } else if ((asset->glassed || command.glassed) && index >= HD_GLASS_FIRST) {
                return flat;
            } else if (command.remap != 0) {
                index = remap_tables[command.remap][index];
            }
            return alpha == 255 ? palette[index] : Blend(palette[index], flat, alpha);
        }

    } // namespace

    namespace {

        struct s_compose_counts {
            int32_t replaced;
            int32_t replaced_glyph;
            int32_t owned_pixels;
            int32_t stale_pixels;
        };

        void Compose_Rect(const s_compose_input* in, uint32_t* output, const uint32_t* palette, int32_t x0, int32_t y0, int32_t x1, int32_t y1, s_compose_counts* counts) {
            const int32_t s = HD_SPRITES_SCALE;
            const int32_t out_width = in->width * s;

            // Base image: the regular scaler output resampled to 4x.
            for (int32_t oy = y0 * s; oy < y1 * s; ++oy) {
                const uint32_t* base_row = in->base + (size_t)(oy * in->base_factor / s) * in->base_stride;
                uint32_t* out_row = output + (size_t)oy * out_width;
                if (in->base_factor == s) {
                    memcpy(out_row + x0 * s, base_row + x0 * s, (size_t)(x1 - x0) * s * sizeof(uint32_t));
                    continue;
                }
                for (int32_t ox = x0 * s; ox < x1 * s; ++ox) {
                    out_row[ox] = base_row[ox * in->base_factor / s];
                }
            }

            if (!enabled || plane_published == nullptr || plane_width != in->width || plane_height != in->height) {
                return;
            }

            int32_t& replaced = counts->replaced;
            int32_t& replaced_glyph = counts->replaced_glyph;
            int32_t& owned_pixels = counts->owned_pixels;
            int32_t& stale_pixels = counts->stale_pixels;
            for (int32_t y = y0; y < y1; ++y) {
                for (int32_t x = x0; x < x1; ++x) {
                    const size_t i = (size_t)y * in->width + x;
                    const uint32_t owner = plane_published[i];
                    if (owner != 0) {
                        owned_pixels++;
                    }
                    if (owner == 0 || (owner & 0xFF) != in->page[i]) {
                        if (owner != 0) {
                            stale_pixels++;
                            if (debug_stale) {
                                // ORION2RE_HD_DEBUG=stale: tint pixels whose owner record went stale.
                                for (int32_t sy = 0; sy < s; ++sy) {
                                    uint32_t* out = output + (size_t)(y * s + sy) * out_width + (size_t)x * s;
                                    for (int32_t sx = 0; sx < s; ++sx) {
                                        out[sx] = (out[sx] & 0xFF7F7F7Fu) | 0x00800000u;
                                    }
                                }
                            }
                        }
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
                            } else if ((asset->glassed || command.glassed) && index >= HD_GLASS_FIRST) {
                                // Translucent pixel: tint the HD art that was underneath.
                                const uint32_t tinted = Apply_Glass(
                                    Under_Color(under_published[i], x, y, sx, sy, palette), index - HD_GLASS_FIRST);
                                out[sx] = alpha == 255 ? tinted : Blend(tinted, out[sx], alpha);
                                continue;
                            } else if (command.remap != 0) {
                                index = remap_tables[command.remap][index];
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
                    if (glyph) {
                        ++replaced_glyph;
                    }
                }
            }
        }

    } // namespace

    int32_t Compose(const s_compose_input* in, uint32_t* output) {
        uint32_t palette[256];
        for (int32_t i = 0; i < 256; ++i) {
            palette[i] = 0xFF000000u | ((uint32_t)in->palette[i].r << 16) | ((uint32_t)in->palette[i].g << 8) | in->palette[i].b;
        }
        s_compose_counts counts = {};
        if (in->rects == nullptr) {
            Compose_Rect(in, output, palette, 0, 0, in->width, in->height, &counts);
        } else {
            for (int32_t r = 0; r < in->rect_count; ++r) {
                const s_rect& rect = in->rects[r];
                const int32_t x0 = SDL_max(rect.x, 0);
                const int32_t y0 = SDL_max(rect.y, 0);
                const int32_t x1 = SDL_min(rect.x + rect.width, in->width);
                const int32_t y1 = SDL_min(rect.y + rect.height, in->height);
                if (x0 < x1 && y0 < y1) {
                    Compose_Rect(in, output, palette, x0, y0, x1, y1, &counts);
                }
            }
        }
        if (stats_enabled && (++stats_compose_calls % 100) == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "hd stats: compose owned=%d stale=%d replaced=%d (glyph %d); glyph commands %u, glyphs without art %u",
                counts.owned_pixels,
                counts.stale_pixels,
                counts.replaced,
                counts.replaced_glyph,
                stats_glyph_commands,
                stats_glyph_missing_art);
        }
        return counts.replaced;
    }

} // namespace hd_sprites
