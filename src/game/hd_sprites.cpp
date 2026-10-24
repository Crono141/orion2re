#include "pch.h"

#include "game/hd_sprites.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "webp/decode.h"
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

        // Decoding state of a frame's pixels (index/alpha). Frames are decoded by worker threads;
        // only READY frames are read or freed by the game thread, only QUEUED/LOADING ones are
        // written by workers.
        enum e_frame_load : uint8_t {
            LOAD_NONE = 0,
            LOAD_QUEUED = 1,
            LOAD_LOADING = 2,
            LOAD_READY = 3,
            LOAD_FAILED = 4,
        };

        struct s_hd_asset;

        struct s_hd_frame {
            e_frame_state state;
            int32_t same_as;
            int32_t origin_x;
            int32_t origin_y;
            int32_t width;
            int32_t height;
            uint8_t* index;
            uint8_t* alpha;
            uint8_t* rgb;     // full-colour art (RGB per pixel) instead of index, or null
            bool full_colour; // the PNG is RGBA (sprite.json "format": "rgba")
            std::atomic<uint8_t> load;
            std::string vfs_path;        // hd/<LBX>/<EEE>/<FFF>.png
            std::string disk_path;       // the same file on disk when it is not inside a zip
            std::vector<uint8_t> packed; // PNG bytes read on the game thread (zipped packs)
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
            uint32_t last_used; // publish count of the last draw or load, for eviction
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
        uint64_t stats_last_publish_ns;
        uint64_t stats_longest_frame_ns;

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

        // Decodes PNG bytes (LA or grey) into a frame's index/alpha planes.
        bool Decode_Png(SDL_IOStream* stream, const std::string& name, s_hd_frame* frame) {
            SDL_Surface* loaded = stream != nullptr ? SDL_LoadPNG_IO(stream, true) : nullptr;
            if (loaded == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: unable to decode %s: %s", name.c_str(), SDL_GetError());
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
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: %s is %dx%d, sprite.json says %dx%d", name.c_str(), rgba->w, rgba->h, frame->width, frame->height);
                SDL_DestroySurface(rgba);
                return false;
            }
            const size_t count = (size_t)frame->width * (size_t)frame->height;
            uint8_t* index = (uint8_t*)malloc(frame->full_colour ? count * 3 : count);
            uint8_t* alpha = (uint8_t*)malloc(count);
            if (index == nullptr || alpha == nullptr) {
                free(index);
                free(alpha);
                SDL_DestroySurface(rgba);
                return false;
            }
            for (int32_t y = 0; y < frame->height; ++y) {
                const uint8_t* row = (const uint8_t*)rgba->pixels + (size_t)y * (size_t)rgba->pitch;
                for (int32_t x = 0; x < frame->width; ++x) {
                    const size_t i = (size_t)y * frame->width + x;
                    if (frame->full_colour) {
                        index[i * 3 + 0] = row[x * 4 + 0];
                        index[i * 3 + 1] = row[x * 4 + 1];
                        index[i * 3 + 2] = row[x * 4 + 2];
                    } else {
                        // Grey PNGs expand to R = G = B = palette index.
                        index[i] = row[x * 4 + 0];
                    }
                    alpha[i] = row[x * 4 + 3];
                }
            }
            SDL_DestroySurface(rgba);
            if (frame->full_colour) {
                frame->rgb = index;
            } else {
                frame->index = index;
            }
            frame->alpha = alpha;
            return true;
        }

        // Decodes a WebP frame (full-colour art) into the frame's planes.
        bool Decode_Webp(const uint8_t* bytes, size_t size, const std::string& name, s_hd_frame* frame) {
            int width = 0;
            int height = 0;
            uint8_t* rgba = WebPDecodeRGBA(bytes, size, &width, &height);
            if (rgba == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: unable to decode %s", name.c_str());
                return false;
            }
            if (width != frame->width || height != frame->height) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: %s is %dx%d, sprite.json says %dx%d", name.c_str(), width, height, frame->width, frame->height);
                WebPFree(rgba);
                return false;
            }
            const size_t count = (size_t)width * (size_t)height;
            uint8_t* rgb = (uint8_t*)malloc(count * 3);
            uint8_t* alpha = (uint8_t*)malloc(count);
            if (rgb == nullptr || alpha == nullptr) {
                free(rgb);
                free(alpha);
                WebPFree(rgba);
                return false;
            }
            for (size_t i = 0; i < count; ++i) {
                rgb[i * 3 + 0] = rgba[i * 4 + 0];
                rgb[i * 3 + 1] = rgba[i * 4 + 1];
                rgb[i * 3 + 2] = rgba[i * 4 + 2];
                alpha[i] = rgba[i * 4 + 3];
            }
            WebPFree(rgba);
            frame->full_colour = true;
            frame->rgb = rgb;
            frame->alpha = alpha;
            return true;
        }

        // A frame file's bytes: WebP (by signature) or PNG.
        bool Decode_Frame_Bytes(const uint8_t* bytes, size_t size, const std::string& name, s_hd_frame* frame) {
            if (size >= 12 && memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WEBP", 4) == 0) {
                return Decode_Webp(bytes, size, name, frame);
            }
            return Decode_Png(SDL_IOFromConstMem(bytes, size), name, frame);
        }

        // Synchronous load through the VFS (game thread; glyphs).
        bool Load_Frame_Png(const std::string& path, s_hd_frame* frame) {
            std::vector<uint8_t> bytes;
            if (!Read_Vfs_File(path.c_str(), &bytes)) {
                return false;
            }
            return Decode_Png(SDL_IOFromConstMem(bytes.data(), bytes.size()), path, frame);
        }

        bool Read_Int_Pair(yyjson_val* array, int32_t* a, int32_t* b) {
            if (!yyjson_is_arr(array) || yyjson_arr_size(array) != 2) {
                return false;
            }
            *a = (int32_t)yyjson_get_int(yyjson_arr_get(array, 0));
            *b = (int32_t)yyjson_get_int(yyjson_arr_get(array, 1));
            return true;
        }

        // ------------------------------------------------------------ frame streaming
        //
        // Frame PNGs are decoded by worker threads, so drawing never waits for art: a draw whose
        // frame is not ready shows the original pixels and is upgraded when the frame arrives
        // (see s_deferred). Workers read files directly when the pack is a directory; for zipped
        // packs the game thread reads the bytes (the VFS is not thread safe) and workers decode.

        std::mutex queue_mutex;
        std::condition_variable queue_ready;
        std::deque<s_hd_frame*> queue;
        std::vector<std::thread> workers;
        bool workers_stop;
        std::atomic<int64_t> decoded_bytes;
        uint32_t publish_count;
        int64_t cache_budget = (int64_t)4096 << 20;
        bool workers_started;

        int64_t Frame_Bytes(const s_hd_frame& frame) {
            return (int64_t)frame.width * frame.height * (frame.full_colour ? 4 : 2);
        }

        void Worker() {
            for (;;) {
                s_hd_frame* frame = nullptr;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    queue_ready.wait(lock, [] { return !queue.empty() || workers_stop; });
                    if (workers_stop) {
                        return;
                    }
                    frame = queue.front();
                    queue.pop_front();
                }
                uint8_t expected = LOAD_QUEUED;
                if (!frame->load.compare_exchange_strong(expected, LOAD_LOADING)) {
                    continue; // already taken (queued twice) or evicted meanwhile
                }
                bool ok = false;
                if (!frame->disk_path.empty()) {
                    size_t size = 0;
                    void* bytes = SDL_LoadFile(frame->disk_path.c_str(), &size);
                    if (bytes != nullptr) {
                        ok = Decode_Frame_Bytes((const uint8_t*)bytes, size, frame->disk_path, frame);
                        SDL_free(bytes);
                    }
                } else {
                    ok = !frame->packed.empty() && Decode_Frame_Bytes(frame->packed.data(), frame->packed.size(), frame->vfs_path, frame);
                    std::vector<uint8_t>().swap(frame->packed);
                }
                if (ok) {
                    decoded_bytes += Frame_Bytes(*frame);
                }
                frame->load.store(ok ? LOAD_READY : LOAD_FAILED, std::memory_order_release);
            }
        }

        // Workers must be stopped before the queue's mutex and condition variable are destroyed
        // at exit (destroying them under waiting threads hangs or crashes the shutdown).
        void Stop_Workers() {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                workers_stop = true;
                queue.clear();
            }
            queue_ready.notify_all();
            for (std::thread& worker : workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            workers.clear();
        }

        void Start_Workers() {
            if (workers_started) {
                return;
            }
            workers_started = true;
            const int32_t count = SDL_clamp(SDL_GetNumLogicalCPUCores() / 4, 2, 8);
            for (int32_t i = 0; i < count; ++i) {
                workers.emplace_back(Worker);
            }
            // Registered after the queue globals were constructed, so it runs before they are
            // destroyed.
            std::atexit(Stop_Workers);
        }

        // Queues a frame for decoding; urgent frames (wanted by a draw now) go first.
        void Request_Frame(s_hd_frame* frame, bool urgent) {
            uint8_t expected = LOAD_NONE;
            if (frame == nullptr || frame->state != FRAME_ART || !frame->load.compare_exchange_strong(expected, LOAD_QUEUED)) {
                if (urgent && frame != nullptr && frame->load.load() == LOAD_QUEUED) {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    queue.push_front(frame); // a duplicate entry; the worker skips whichever comes second
                    queue_ready.notify_one();
                }
                return;
            }
            if (frame->disk_path.empty() && frame->packed.empty()) {
                (void)Read_Vfs_File(frame->vfs_path.c_str(), &frame->packed);
            }
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (urgent) {
                queue.push_front(frame);
            } else {
                queue.push_back(frame);
            }
            queue_ready.notify_one();
        }

        int32_t Queue_Length() {
            std::lock_guard<std::mutex> lock(queue_mutex);
            return (int32_t)queue.size();
        }

        bool Frame_Ready(const s_hd_frame* frame) {
            return frame != nullptr && frame->load.load(std::memory_order_acquire) == LOAD_READY;
        }

        // Reads sprite.json; frame pixels are decoded later by the workers.
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
            const char* format = yyjson_get_str(yyjson_obj_get(root, "format"));
            // Frame files are <FFF>.png, or <FFF>.webp when sprite.json says "image": "webp".
            const char* image = yyjson_get_str(yyjson_obj_get(root, "image"));
            const bool webp = image != nullptr && strcmp(image, "webp") == 0;
            const bool full_colour = format != nullptr && strcmp(format, "rgba") == 0;
            yyjson_val* source_hash = yyjson_obj_get(root, "source_hash");
            asset->has_source_hash = yyjson_is_uint(source_hash) || yyjson_is_int(source_hash);
            asset->source_hash = asset->has_source_hash ? (uint32_t)yyjson_get_uint(source_hash) : 0;
            if (asset->scale != HD_SPRITES_SCALE) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "hd: %s has scale %d, only %d is supported", meta_path.c_str(), asset->scale, HD_SPRITES_SCALE);
                yyjson_doc_free(document);
                return;
            }

            const int32_t count = (int32_t)yyjson_arr_size(frames);
            s_hd_frame* loaded = new s_hd_frame[(size_t)count]();
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
                snprintf(name, sizeof(name), webp ? "/%03d.webp" : "/%03d.png", frame_number);
                frame->vfs_path = asset->directory + name;
                frame->full_colour = full_colour;
                char disk[1024];
                if (vfs::Resolve_Read_Path_No_Zip(frame->vfs_path.c_str(), disk, sizeof(disk)) != 0) {
                    frame->disk_path = disk;
                }
                frame->state = FRAME_ART;
            }
            yyjson_doc_free(document);
            asset->frames = loaded;
            asset->frame_count = count;
        }

        // Queues every frame of an asset (the game just loaded its LBX entry, so it is about to
        // be drawn).
        void Prefetch_Asset(s_hd_asset* asset) {
            if (!asset->loaded) {
                Load_Asset(asset);
            }
            asset->last_used = publish_count;
            for (int32_t i = 0; i < asset->frame_count; ++i) {
                Request_Frame(&asset->frames[i], false);
            }
        }

        // Frees the decoded pixels of the least recently used assets beyond the cache budget.
        void Evict_Over_Budget() {
            if (decoded_bytes.load() <= cache_budget) {
                return;
            }
            std::vector<std::pair<uint32_t, s_hd_asset*>> candidates;
            for (int32_t i = 0; i < asset_count; ++i) {
                s_hd_asset* asset = assets[i];
                // Keep anything used within the last few seconds.
                if (asset->frames != nullptr && publish_count - asset->last_used > 120) {
                    candidates.emplace_back(asset->last_used, asset);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            const int64_t target = cache_budget - cache_budget / 8;
            for (const auto& candidate : candidates) {
                if (decoded_bytes.load() <= target) {
                    break;
                }
                s_hd_asset* asset = candidate.second;
                for (int32_t i = 0; i < asset->frame_count; ++i) {
                    s_hd_frame& frame = asset->frames[i];
                    if (frame.load.load(std::memory_order_acquire) != LOAD_READY) {
                        continue;
                    }
                    free(frame.index);
                    free(frame.alpha);
                    free(frame.rgb);
                    frame.index = frame.alpha = frame.rgb = nullptr;
                    decoded_bytes -= Frame_Bytes(frame);
                    frame.load.store(LOAD_NONE, std::memory_order_release);
                }
            }
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

        // Glass of level `level` with coverage `alpha` over `dest`. Palette-based pixels go through
        // the game's own glass table (exactly what the original pixel gets, so e.g. bright lights
        // under dark glass turn dark as in the original); full-colour pixels and partial
        // coverage are tinted in true colour.
        uint32_t Glass_Tint(uint32_t dest, int32_t level, uint32_t alpha) {
            if (alpha >= 255 && (dest & HD_LITERAL) == 0) {
                const uint8_t* table = video::glass_remap_colors[level & 15];
                return (dest & 0xFFFF0000u) | ((uint32_t)table[(dest >> 8) & 0xFF] << 8) | table[dest & 0xFF];
            }
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
        // Guards allocating/freeing the page surfaces (game thread) against the presenter
        // reading the published/present surfaces.
        std::mutex planes_mutex;
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
            // The frame has art that is still being decoded: written pixels show the original
            // for now and are recorded so they can be upgraded when it arrives.
            bool waiting = false;
            // Clip window at the start of the draw (the fringe must stay inside it).
            int32_t clip_x1 = 0;
            int32_t clip_y1 = 0;
            int32_t clip_x2 = 0x7FFF;
            int32_t clip_y2 = 0x7FFF;
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

        // Page draws that showed the original because their frame was still decoding. When the
        // frame is ready, pixels that no later draw touched (mark still holds the draw's
        // generation, page value unchanged) get the art.
        struct s_deferred {
            s_draw draw;
            uint32_t generation;
            uint32_t published; // publish count when recorded
            std::vector<s_stamp> stamps;
        };
        std::vector<s_deferred> deferred;
        std::vector<s_stamp> waiting_stamps;

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
            if (frame->rgb != nullptr) {
                uint32_t rgb = ((uint32_t)frame->rgb[i * 3] << 16) | ((uint32_t)frame->rgb[i * 3 + 1] << 8) | frame->rgb[i * 3 + 2];
                if (!draw.shade.empty()) {
                    rgb = Shade(rgb, x - draw.x, y - draw.y, sx, sy);
                }
                return Over_Rgb(dest, rgb, alpha);
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
                const uint16_t made_for = !image.hd.empty() && ix >= 0 && iy >= 0 && ix < image.width && iy < image.height
                    ? image.shadow[(size_t)iy * image.width + ix]
                    : SHADOW_NONE;
                // A remapped image draw (e.g. the hovered colony building flashing) recolours
                // the image's HD pixels with the same table.
                if (made_for != SHADOW_NONE && (draw.remapped ? draw.remap[made_for] : made_for) == value) {
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
                                    out = Over_Index(out, draw.remapped ? draw.remap[pixel & 0xFF] : (uint8_t)pixel, (pixel >> 16) & 0xFF);
                                }
                            } else if (draw.remapped && (pixel & HD_LITERAL) == 0) {
                                out = (pixel & 0xFFFF0000u) | ((uint32_t)draw.remap[(pixel >> 8) & 0xFF] << 8) |
                                    draw.remap[pixel & 0xFF];
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

        bool Interior(const s_surface& target, const s_stamp& stamp) {
            const int32_t x = stamp.offset % target.width;
            const int32_t y = stamp.offset / target.width;
            return stamp.level < 0 && Marked(target, x - 1, y) && Marked(target, x + 1, y) && Marked(target, x, y - 1) &&
                Marked(target, x, y + 1);
        }

        // Whether the art agrees with what the game drew: inside the sprite, the art's colour at
        // each pixel centre must mostly be close to the pixel written. Art made with the wrong
        // palette, for another frame or for different source pixels fails, and the draw shows
        // the original pixels instead.
        bool Art_Matches(const s_surface& target) {
            int32_t samples = 0;
            int32_t mismatches = 0;
            for (const s_stamp& stamp : pending) {
                if (stamp.target != &target || !Interior(target, stamp)) {
                    continue;
                }
                const int32_t x = stamp.offset % target.width;
                const int32_t y = stamp.offset / target.width;
                const uint32_t art = Base_Color(Apply_Art(Indexed(stamp.value), x, y, S / 2, S / 2));
                const uint32_t drawn = Base_Rgb(stamp.value);
                const int32_t distance = abs((int32_t)((art >> 16) & 0xFF) - (int32_t)((drawn >> 16) & 0xFF)) +
                    abs((int32_t)((art >> 8) & 0xFF) - (int32_t)((drawn >> 8) & 0xFF)) +
                    abs((int32_t)(art & 0xFF) - (int32_t)(drawn & 0xFF));
                samples++;
                mismatches += distance > 150 ? 1 : 0;
            }
            return samples < 12 || mismatches * 10 < samples * 6;
        }

        // Renders the pending stamps of an art draw into one target.
        void Render_Pending_Target(s_surface& target) {
            if (!Art_Matches(target)) {
                if (stats_enabled) {
                    Stats_Missing("art-mismatch:" + draw.name);
                }
                for (const s_stamp& stamp : pending) {
                    if (stamp.target == &target) {
                        Render_Now(target, stamp.offset, stamp.value, stamp.level);
                    }
                }
                return;
            }
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
                if (Interior(target, stamp)) {
                    Fill_Block(target, x, y, Indexed(stamp.value));
                }
                Apply_Art_Block(target, x, y);
                target.shadow[(size_t)stamp.offset] = stamp.value;
            }
            if (x1 < 0 || target.row_dirty.empty()) {
                return; // images get no fringe: their transparent pixels are never drawn
            }
            // Fringe: the art's soft edge past the original outline, over unwritten neighbours.
            const int32_t clip_x1 = SDL_max(0, draw.clip_x1);
            const int32_t clip_y1 = SDL_max(0, draw.clip_y1);
            const int32_t clip_x2 = SDL_min(target.width - 1, draw.clip_x2);
            const int32_t clip_y2 = SDL_min(target.height - 1, draw.clip_y2);
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
            if (draw.waiting && !waiting_stamps.empty()) {
                if (deferred.size() >= 4096) {
                    deferred.erase(deferred.begin(), deferred.begin() + 1024);
                }
                deferred.push_back({draw, generation, publish_count, std::move(waiting_stamps)});
                waiting_stamps.clear();
            }
            waiting_stamps.clear();
            draw.waiting = false;
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
            if (draw.source == SOURCE_ART && draw.waiting) {
                Render_Now(target, offset, value, level);
                if (!target.row_dirty.empty()) {
                    target.mark[(size_t)offset] = generation;
                    waiting_stamps.push_back({&target, offset, value, (int8_t)level});
                }
                return;
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
            if (animate::clipping_flag == 1) {
                draw.clip_x1 = video::screen_window_x1;
                draw.clip_y1 = video::screen_window_y1;
                draw.clip_x2 = video::screen_window_x2;
                draw.clip_y2 = video::screen_window_y2;
            } else {
                draw.clip_x1 = draw.clip_y1 = 0;
                draw.clip_x2 = draw.clip_y2 = 0x7FFF;
            }
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
            asset->last_used = publish_count;
            if (!Frame_Ready(frame)) {
                Request_Frame(const_cast<s_hd_frame*>(frame), true);
                draw.waiting = true;
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

        // Gives deferred draws their art once it is decoded (game thread, between draws).
        void Upgrade_Deferred() {
            if (deferred.empty()) {
                return;
            }
            s_draw saved_draw = draw;
            const uint32_t saved_generation = generation;
            size_t kept = 0;
            for (size_t i = 0; i < deferred.size(); ++i) {
                s_deferred& entry = deferred[i];
                const uint8_t state = entry.draw.frame->load.load(std::memory_order_acquire);
                if (state != LOAD_READY) {
                    // Still decoding (or evicted and queued again): keep it a while.
                    if (state != LOAD_FAILED && publish_count - entry.published < 600) {
                        if (kept != i) {
                            deferred[kept] = std::move(entry);
                        }
                        kept++;
                    }
                    continue;
                }
                draw = entry.draw;
                draw.waiting = false;
                generation = entry.generation;
                // Screens often copy the finished page to the other page and restore from it, so
                // the other page may hold the same plain blocks: those get the upgraded art too.
                std::vector<s_stamp> twins;
                for (const s_stamp& stamp : entry.stamps) {
                    s_surface& target = *stamp.target;
                    if (target.mark[(size_t)stamp.offset] != entry.generation || target.shadow[(size_t)stamp.offset] != stamp.value) {
                        continue;
                    }
                    pending.push_back(stamp);
                    s_surface* other = &target == &page_off ? &page_back : &target == &page_back ? &page_off
                                                                                                 : nullptr;
                    if (other != nullptr && !other->hd.empty() && other->shadow[(size_t)stamp.offset] == stamp.value) {
                        const uint32_t* block = Block(*other, stamp.offset % other->width, stamp.offset / other->width);
                        const size_t stride = (size_t)other->width * S;
                        bool plain = true;
                        for (int32_t sy = 0; sy < S && plain; ++sy) {
                            for (int32_t sx = 0; sx < S && plain; ++sx) {
                                plain = block[sy * stride + sx] == Indexed(stamp.value);
                            }
                        }
                        if (plain) {
                            twins.push_back({other, stamp.offset, stamp.value, stamp.level});
                        }
                    }
                }
                Flush_Pending();
                for (const s_stamp& twin : twins) {
                    s_surface& from = twin.target == &page_off ? page_back : page_off;
                    const int32_t x = twin.offset % from.width;
                    const int32_t y = twin.offset / from.width;
                    const size_t stride = (size_t)from.width * S;
                    const uint32_t* src = Block(from, x, y);
                    uint32_t* dst = Block(*twin.target, x, y);
                    for (int32_t sy = 0; sy < S; ++sy) {
                        memcpy(dst + sy * stride, src + sy * stride, S * sizeof(uint32_t));
                    }
                    Mark_Row(*twin.target, y);
                }
            }
            deferred.resize(kept);
            draw = saved_draw;
            generation = saved_generation;
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
        const char* budget = SDL_getenv("ORION2RE_HD_CACHE_MB");
        if (budget != nullptr && atoi(budget) > 0) {
            cache_budget = (int64_t)atoi(budget) << 20;
        }
        if (enabled) {
            Start_Workers();
            // The title screen is the first thing shown: start decoding it right away.
            for (int32_t entry = 0; entry < 40; ++entry) {
                const int32_t index = Find_Or_Create_Asset("MAINMENU.LBX", entry);
                if (index >= 0) {
                    Prefetch_Asset(assets[index]);
                }
            }
        }
        if (enabled && video::g_page_off.pixels != nullptr) {
            Allocate_Planes(video::g_page_off.width, video::g_page_off.height);
        }
    }

    bool Enabled() {
        return enabled;
    }

    void Shutdown() {
        if (workers_started) {
            Stop_Workers();
        }
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
        // The entry is about to be drawn: start decoding its art (unless a mod changed it).
        s_hd_asset* asset = assets[registration.asset];
        if (!asset->loaded) {
            Load_Asset(asset);
        }
        if (!asset->has_source_hash || asset->source_hash == registration.data_hash) {
            Prefetch_Asset(asset);
        }
    }

    void Forget_Loads(const void* begin, const void* end) {
        if (!enabled) {
            return;
        }
        for (auto it = registrations.begin(); it != registrations.end();) {
            it = it->first >= begin && it->first < end ? registrations.erase(it) : std::next(it);
        }
    }

    void Move_Loads(const void* begin, const void* end, ptrdiff_t delta) {
        if (!enabled || delta == 0) {
            return;
        }
        std::vector<std::pair<const void*, s_registration>> moved;
        for (auto it = registrations.begin(); it != registrations.end();) {
            if (it->first >= begin && it->first < end) {
                moved.emplace_back((const uint8_t*)it->first + delta, it->second);
                it = registrations.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& item : moved) {
            registrations[item.first] = item.second;
        }
        if (!stats_names.empty()) {
            std::vector<std::pair<const void*, std::string>> names;
            for (auto it = stats_names.begin(); it != stats_names.end();) {
                if (it->first >= begin && it->first < end) {
                    names.emplace_back((const uint8_t*)it->first + delta, it->second);
                    it = stats_names.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto& item : names) {
                stats_names[item.first] = item.second;
            }
        }
    }

    void Begin_Draw(const s_animation_header* anim, int16_t x, int16_t y) {
        Begin_Source(anim, x, y);
    }

    void Begin_Remap_Draw(const s_animation_header* anim, int16_t x, int16_t y, const uint8_t* remap) {
        Begin_Source(anim, x, y);
        if (remap == nullptr) {
            return;
        }
        if (draw.source == SOURCE_ART || draw.source == SOURCE_IMAGE) {
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
            // Spaces have no pixels and so no art: not worth reporting.
            if (stats_enabled && code != ' ') {
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
        asset->last_used = publish_count;
        if (!Frame_Ready(art)) {
            Request_Frame(const_cast<s_hd_frame*>(art), true);
            draw.waiting = true;
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

    void Tint_Rect(const uint8_t* page, int32_t x, int32_t y, int32_t width, int32_t height, int32_t level) {
        if (!enabled || width <= 0 || height <= 0) {
            return;
        }
        s_surface* target = Page_Surface(page);
        if (target == nullptr) {
            return;
        }
        Flush_Pending();
        const uint8_t* table = video::glass_remap_colors[level & 15];
        const size_t stride = (size_t)target->width * S;
        const int32_t x0 = SDL_max(x, 0);
        const int32_t y0 = SDL_max(y, 0);
        const int32_t x1 = SDL_min(x + width, target->width);
        const int32_t y1 = SDL_min(y + height, target->height);
        for (int32_t py = y0; py < y1; ++py) {
            for (int32_t px = x0; px < x1; ++px) {
                const size_t i = (size_t)py * target->width + px;
                const uint16_t old = target->shadow[i];
                // Only blocks that showed the pixel before the tint can be tinted; others settle.
                if (old == SHADOW_NONE || table[old] != page[i]) {
                    target->shadow[i] = SHADOW_NONE;
                    continue;
                }
                target->shadow[i] = page[i];
                uint32_t* block = Block(*target, px, py);
                for (int32_t sy = 0; sy < S; ++sy) {
                    for (int32_t sx = 0; sx < S; ++sx) {
                        block[sy * stride + sx] = Glass_Tint(block[sy * stride + sx], level, 255u);
                    }
                }
            }
            Mark_Row(*target, py);
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

    void Bitmap_Scale(const void* dst, const void* src, const uint8_t* dst_pixels, int32_t width, int32_t height) {
        if (!enabled || dst == src || dst_pixels == nullptr) {
            return;
        }
        Flush_Pending();
        const auto from = images.find(src);
        auto to = images.find(dst);
        if (from == images.end() || to == images.end() || from->second.hd.empty() || to->second.width != width ||
            to->second.height != height) {
            return;
        }
        const s_surface& source = from->second;
        s_surface& target = to->second;
        Ensure_Hd(target);
        const int32_t src_w = source.width * S;
        const int32_t src_h = source.height * S;
        const int32_t dst_w = width * S;
        const int32_t dst_h = height * S;
        // Box filter over the source HD pixels each target HD pixel covers: coverage is averaged,
        // the colour comes from the most covered source pixel.
        for (int32_t y = 0; y < dst_h; ++y) {
            const int32_t sy0 = y * src_h / dst_h;
            const int32_t sy1 = SDL_max(sy0 + 1, (y + 1) * src_h / dst_h);
            for (int32_t x = 0; x < dst_w; ++x) {
                const int32_t sx0 = x * src_w / dst_w;
                const int32_t sx1 = SDL_max(sx0 + 1, (x + 1) * src_w / dst_w);
                uint32_t coverage = 0;
                uint32_t samples = 0;
                uint32_t best = HD_CLEAR;
                uint32_t best_alpha = 0;
                for (int32_t sy = sy0; sy < sy1 && sy < src_h; ++sy) {
                    for (int32_t sx = sx0; sx < sx1 && sx < src_w; ++sx) {
                        const uint32_t pixel = source.hd[(size_t)sy * src_w + sx];
                        const uint32_t alpha = (pixel & HD_LITERAL) != 0 || (pixel & HD_CLEAR) == 0 ? 255u : (pixel >> 16) & 0xFF;
                        coverage += alpha;
                        samples++;
                        if (alpha > best_alpha) {
                            best_alpha = alpha;
                            best = pixel;
                        }
                    }
                }
                const uint32_t alpha = samples != 0 ? coverage / samples : 0;
                uint32_t out;
                if (alpha >= 255 && (best & HD_CLEAR) == 0) {
                    out = best;
                } else if ((best & HD_LITERAL) != 0) {
                    out = alpha >= 128 ? best : (HD_CLEAR | (alpha << 16));
                } else {
                    out = (best & 0xFFu) | (alpha << 16) | HD_CLEAR;
                }
                target.hd[(size_t)y * dst_w + x] = out;
            }
        }
        for (int32_t i = 0; i < width * height; ++i) {
            target.shadow[(size_t)i] = dst_pixels[i];
        }
    }

    void Bitmap_Glass(const void* bitmap, const uint8_t* pixels, int32_t level) {
        if (!enabled || pixels == nullptr) {
            return;
        }
        Flush_Pending();
        auto found = images.find(bitmap);
        if (found == images.end() || found->second.hd.empty()) {
            return;
        }
        s_surface& image = found->second;
        const uint8_t* table = video::glass_remap_colors[level & 15];
        const size_t stride = (size_t)image.width * S;
        for (int32_t i = 0; i < image.width * image.height; ++i) {
            const uint16_t old = image.shadow[(size_t)i];
            if (old == SHADOW_NONE) {
                continue;
            }
            if (table[old] != pixels[i]) {
                image.shadow[(size_t)i] = SHADOW_NONE;
                continue;
            }
            image.shadow[(size_t)i] = pixels[i];
            uint32_t* block = &image.hd[(size_t)(i / image.width) * S * stride + (size_t)(i % image.width) * S];
            for (int32_t sy = 0; sy < S; ++sy) {
                for (int32_t sx = 0; sx < S; ++sx) {
                    uint32_t& pixel = block[sy * stride + sx];
                    if ((pixel & HD_LITERAL) == 0 && (pixel & HD_CLEAR) != 0) {
                        // Partly covered over transparency: tint the colour, keep the coverage.
                        pixel = (pixel & ~0xFFu) | table[pixel & 0xFF];
                    } else {
                        pixel = Glass_Tint(pixel, level, 255u);
                    }
                }
            }
        }
    }

    void Bitmap_Sync(const void* bitmap, const uint8_t* pixels) {
        if (!enabled || pixels == nullptr) {
            return;
        }
        Flush_Pending();
        auto found = images.find(bitmap);
        if (found == images.end() || found->second.hd.empty()) {
            return;
        }
        s_surface& image = found->second;
        const size_t stride = (size_t)image.width * S;
        for (int32_t i = 0; i < image.width * image.height; ++i) {
            const uint16_t old = image.shadow[(size_t)i];
            if (old == SHADOW_NONE || old == pixels[i]) {
                continue;
            }
            image.shadow[(size_t)i] = pixels[i];
            uint32_t* block = &image.hd[(size_t)(i / image.width) * S * stride + (size_t)(i % image.width) * S];
            const uint32_t from = Base_Rgb((uint8_t)old);
            const uint32_t to = Base_Rgb(pixels[i]);
            for (int32_t sy = 0; sy < S; ++sy) {
                for (int32_t sx = 0; sx < S; ++sx) {
                    uint32_t& pixel = block[sy * stride + sx];
                    if (pixels[i] == 0) {
                        pixel = HD_CLEAR;
                    } else if ((pixel & HD_LITERAL) == 0 && (pixel & HD_CLEAR) != 0) {
                        pixel = (pixel & ~0xFFu) | pixels[i];
                    } else {
                        // Same per-channel change as the page pixel went through.
                        const uint32_t c = Base_Color(pixel);
                        uint32_t out = 0;
                        for (int32_t shift = 0; shift <= 16; shift += 8) {
                            const uint32_t f = SDL_max((from >> shift) & 0xFF, 1u);
                            out |= SDL_min(255u, ((c >> shift) & 0xFF) * ((to >> shift) & 0xFF) / f) << shift;
                        }
                        pixel = Literal(out);
                    }
                }
            }
        }
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
        std::lock_guard<std::mutex> lock(planes_mutex);
        Allocate_Page(page_off, width, height);
        Allocate_Page(page_back, width, height);
        Allocate_Page(page_published, width, height);
        Allocate_Page(page_present, width, height);
    }

    void Free_Planes() {
        std::lock_guard<std::mutex> lock(planes_mutex);
        pending.clear();
        deferred.clear();
        waiting_stamps.clear();
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
        if (stats_enabled) {
            const uint64_t now = SDL_GetTicksNS();
            if (stats_last_publish_ns != 0) {
                stats_longest_frame_ns = SDL_max(stats_longest_frame_ns, now - stats_last_publish_ns);
            }
            stats_last_publish_ns = now;
        }
        s_scoped_timer timer(TIMER_PUBLISH);
        Upgrade_Deferred();
        publish_count++;
        Evict_Over_Budget();
        // Settle pixels changed by code without hooks: the original pixel, enlarged.
        const uint32_t settled = Settle(page_off, video::g_page_off.pixels, 0, 0, page_off.width, page_off.height);
        if (stats_enabled) {
            stats_settled_pixels += settled;
            if (++stats_publishes % 100 == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "hd stats: %u pixels settled to plain scaling over the last 100 frames; per frame: render %.2f ms, "
                    "page copies %.2f ms, publish %.2f ms, longest frame %.0f ms; cache %lld MB decoded, %d frames queued, %d draws waiting for art",
                    stats_settled_pixels,
                    (double)stats_timer_ns[TIMER_RENDER] / 1e8,
                    (double)stats_timer_ns[TIMER_COPY] / 1e8,
                    (double)stats_timer_ns[TIMER_PUBLISH] / 1e8,
                    (double)stats_longest_frame_ns / 1e6,
                    (long long)(decoded_bytes.load() >> 20),
                    Queue_Length(),
                    (int)deferred.size());
                stats_settled_pixels = 0;
                memset(stats_timer_ns, 0, sizeof(stats_timer_ns));
                stats_longest_frame_ns = 0;
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
        std::lock_guard<std::mutex> lock(planes_mutex);
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

    bool Cursor_Art(const uint8_t* pixels, int32_t size, const video::s_palette_color* palette, uint32_t* argb) {
        static std::unordered_map<uint32_t, s_hd_frame*> cursors;
        if (!enabled || pixels == nullptr || size <= 0) {
            return false;
        }
        uint32_t hash = 2166136261u;
        for (int32_t i = 0; i < size * size; ++i) {
            hash = (hash ^ pixels[i]) * 16777619u;
        }
        auto found = cursors.find(hash);
        if (found == cursors.end()) {
            // Small and rare: loaded synchronously on the presenter, through the VFS lock-free
            // path only when the file is on disk (the VFS is not thread safe).
            s_hd_frame* frame = new s_hd_frame();
            char path[64];
            snprintf(path, sizeof(path), "hd/CURSORS/%08x.png", hash);
            char disk[1024];
            size_t bytes_size = 0;
            void* bytes = vfs::Resolve_Read_Path_No_Zip(path, disk, sizeof(disk)) != 0 ? SDL_LoadFile(disk, &bytes_size) : nullptr;
            const bool ok = bytes != nullptr && Decode_Frame_Bytes((const uint8_t*)bytes, bytes_size, path, frame) &&
                frame->index != nullptr && frame->width == size * S && frame->height == size * S;
            SDL_free(bytes);
            if (!ok) {
                delete frame;
                frame = nullptr;
                if (stats_enabled) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "hd stats: no art for cursor %08x", hash);
                }
            }
            found = cursors.emplace(hash, frame).first;
        }
        const s_hd_frame* frame = found->second;
        if (frame == nullptr) {
            return false;
        }
        for (int32_t i = 0; i < frame->width * frame->height; ++i) {
            const video::s_palette_color& c = palette[frame->index[i]];
            argb[i] = ((uint32_t)frame->alpha[i] << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
        }
        return true;
    }

    bool Resolve(const video::s_palette_color* palette, uint32_t* output, int32_t* row_first, int32_t* row_last) {
        static uint32_t last_palette[256];
        static uint32_t last_fade = 0xFFFFFFFFu;
        std::lock_guard<std::mutex> lock(planes_mutex);
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
