#include "pch.h"

#include "game/stream_source.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace stream_source {

#define WAV_FORMAT_PCM 0x0001
#define WAV_FORMAT_EXTENSIBLE 0xFFFE
#define VORBIS_DECODE_FRAMES 1024

    struct s_stream_source {
        e_stream_source_format format;
        std::vector<uint8_t> bytes;
        uint32_t data_offset;
        uint32_t total_bytes;
        uint32_t position;
        uint16_t channels;
        stb_vorbis* vorbis;
        std::vector<int16_t> pending;
        uint32_t pending_offset;
        uint32_t pending_size;
        bool vorbis_ended;
    };

    namespace {

        uint16_t Read_U16(const uint8_t* bytes) {
            return (uint16_t)(bytes[0] | (bytes[1] << 8));
        }

        uint32_t Read_U32(const uint8_t* bytes) {
            return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
        }

        bool Tag_Equals(const uint8_t* bytes, const char* tag) {
            return SDL_strncasecmp((const char*)bytes, tag, 4) == 0;
        }

        bool Open_Wav(s_stream_source* source, s_stream_source_info* info, const char** error) {
            const uint8_t* bytes = source->bytes.data();
            const size_t size = source->bytes.size();
            if (size < 12 || !Tag_Equals(bytes, "RIFF") || !Tag_Equals(bytes + 8, "WAVE")) {
                *error = "missing RIFF/WAVE header";
                return false;
            }

            bool have_format = false;
            bool have_data = false;
            uint16_t format_tag = 0;
            uint16_t channels = 0;
            uint32_t sample_rate = 0;
            uint16_t bits_per_sample = 0;
            uint32_t data_offset = 0;
            uint32_t data_size = 0;

            size_t offset = 12;
            while (offset + 8 <= size && !(have_format && have_data)) {
                const uint8_t* chunk = bytes + offset;
                const uint32_t chunk_size = Read_U32(chunk + 4);
                const size_t body_offset = offset + 8;
                const size_t available = size - body_offset;

                if (Tag_Equals(chunk, "fmt ")) {
                    if (chunk_size < 16 || available < 16) {
                        *error = "truncated fmt chunk";
                        return false;
                    }
                    format_tag = Read_U16(chunk + 8);
                    channels = Read_U16(chunk + 10);
                    sample_rate = Read_U32(chunk + 12);
                    bits_per_sample = Read_U16(chunk + 22);
                    if (format_tag == WAV_FORMAT_EXTENSIBLE) {
                        // The subformat GUID begins with the plain format tag.
                        if (chunk_size < 40 || available < 40) {
                            *error = "truncated extensible fmt chunk";
                            return false;
                        }
                        format_tag = Read_U16(chunk + 32);
                    }
                    have_format = true;
                } else if (Tag_Equals(chunk, "data")) {
                    data_offset = (uint32_t)body_offset;
                    data_size = chunk_size <= available ? chunk_size : (uint32_t)available;
                    have_data = true;
                }

                offset = body_offset + (size_t)chunk_size + (chunk_size & 1u);
            }

            if (!have_format || !have_data) {
                *error = have_format ? "missing data chunk" : "missing fmt chunk";
                return false;
            }
            if (format_tag != WAV_FORMAT_PCM || (bits_per_sample != 8 && bits_per_sample != 16)) {
                *error = "unsupported WAV encoding; use 8-bit or 16-bit PCM";
                return false;
            }
            if (channels == 0 || sample_rate == 0) {
                *error = "invalid WAV channel count or sample rate";
                return false;
            }

            const uint32_t frame_size = (uint32_t)channels * (bits_per_sample / 8u);
            data_size -= data_size % frame_size;
            if (data_size == 0) {
                *error = "empty WAV data chunk";
                return false;
            }

            source->format = STREAM_SOURCE_FORMAT_WAV;
            source->data_offset = data_offset;
            source->total_bytes = data_size;
            source->channels = channels;

            info->format = STREAM_SOURCE_FORMAT_WAV;
            info->channels = channels;
            info->sample_rate = sample_rate;
            info->bits_per_sample = bits_per_sample;
            info->total_bytes = data_size;
            return true;
        }

        bool Open_Vorbis(s_stream_source* source, s_stream_source_info* info, const char** error) {
            if (source->bytes.size() > (size_t)INT32_MAX) {
                *error = "Ogg Vorbis entry is too large";
                return false;
            }

            int vorbis_error = 0;
            source->vorbis = stb_vorbis_open_memory(source->bytes.data(), (int)source->bytes.size(), &vorbis_error, nullptr);
            if (source->vorbis == nullptr) {
                *error = "invalid Ogg Vorbis stream";
                return false;
            }

            const stb_vorbis_info vorbis_info = stb_vorbis_get_info(source->vorbis);
            const uint32_t frames = stb_vorbis_stream_length_in_samples(source->vorbis);
            if (vorbis_info.channels <= 0 || vorbis_info.channels > 8 || vorbis_info.sample_rate == 0 || frames == 0) {
                *error = "Ogg Vorbis stream has no playable audio";
                return false;
            }

            const uint64_t total_bytes = (uint64_t)frames * (uint64_t)vorbis_info.channels * sizeof(int16_t);
            if (total_bytes > UINT32_MAX) {
                *error = "Ogg Vorbis stream is too long";
                return false;
            }

            source->format = STREAM_SOURCE_FORMAT_OGG_VORBIS;
            source->total_bytes = (uint32_t)total_bytes;
            source->channels = (uint16_t)vorbis_info.channels;
            source->pending.resize((size_t)VORBIS_DECODE_FRAMES * (size_t)vorbis_info.channels);

            info->format = STREAM_SOURCE_FORMAT_OGG_VORBIS;
            info->channels = (uint16_t)vorbis_info.channels;
            info->sample_rate = vorbis_info.sample_rate;
            info->bits_per_sample = 16;
            info->total_bytes = (uint32_t)total_bytes;
            return true;
        }

        uint32_t Read_Vorbis(s_stream_source* source, uint8_t* buffer, uint32_t bytes_to_read) {
            uint32_t copied = 0;
            while (copied < bytes_to_read) {
                if (source->pending_offset == source->pending_size) {
                    source->pending_offset = 0;
                    source->pending_size = 0;
                    if (!source->vorbis_ended) {
                        const int frames = stb_vorbis_get_samples_short_interleaved(source->vorbis,
                            source->channels,
                            source->pending.data(),
                            (int)source->pending.size());
                        if (frames <= 0) {
                            source->vorbis_ended = true;
                        } else {
                            source->pending_size = (uint32_t)frames * source->channels * (uint32_t)sizeof(int16_t);
                        }
                    }
                }

                const uint32_t wanted = bytes_to_read - copied;
                if (source->pending_size == 0) {
                    memset(buffer + copied, 0, wanted);
                    copied += wanted;
                    break;
                }

                uint32_t chunk = source->pending_size - source->pending_offset;
                if (chunk > wanted) {
                    chunk = wanted;
                }
                memcpy(buffer + copied, (const uint8_t*)source->pending.data() + source->pending_offset, chunk);
                source->pending_offset += chunk;
                copied += chunk;
            }
            return copied;
        }

    } // namespace

    s_stream_source* Open(std::vector<uint8_t>* bytes, s_stream_source_info* info, const char** error) {
        static const char* unused_error = nullptr;
        if (error == nullptr) {
            error = &unused_error;
        }
        *error = nullptr;
        if (bytes == nullptr || info == nullptr) {
            *error = "invalid arguments";
            return nullptr;
        }
        *info = {};

        s_stream_source* source = new s_stream_source();
        source->bytes.swap(*bytes);

        bool opened = false;
        if (source->bytes.size() >= 4 && memcmp(source->bytes.data(), "OggS", 4) == 0) {
            opened = Open_Vorbis(source, info, error);
        } else {
            opened = Open_Wav(source, info, error);
        }

        if (!opened) {
            source->bytes.swap(*bytes);
            Close(source);
            return nullptr;
        }
        return source;
    }

    uint32_t Read(s_stream_source* source, void* buffer, uint32_t bytes_to_read) {
        if (source == nullptr || buffer == nullptr) {
            return 0;
        }

        const uint32_t remaining = source->total_bytes - source->position;
        if (bytes_to_read > remaining) {
            bytes_to_read = remaining;
        }

        if (source->format == STREAM_SOURCE_FORMAT_WAV) {
            memcpy(buffer, source->bytes.data() + source->data_offset + source->position, bytes_to_read);
        } else {
            Read_Vorbis(source, (uint8_t*)buffer, bytes_to_read);
        }
        source->position += bytes_to_read;
        return bytes_to_read;
    }

    bool Rewind(s_stream_source* source) {
        if (source == nullptr) {
            return false;
        }
        source->position = 0;
        if (source->format == STREAM_SOURCE_FORMAT_OGG_VORBIS) {
            source->pending_offset = 0;
            source->pending_size = 0;
            source->vorbis_ended = false;
            return stb_vorbis_seek_start(source->vorbis) != 0;
        }
        return true;
    }

    void Close(s_stream_source* source) {
        if (source == nullptr) {
            return;
        }
        if (source->vorbis != nullptr) {
            stb_vorbis_close(source->vorbis);
        }
        delete source;
    }

    const char* Format_Name(e_stream_source_format format) {
        return format == STREAM_SOURCE_FORMAT_OGG_VORBIS ? "ogg" : "wav";
    }

} // namespace stream_source
