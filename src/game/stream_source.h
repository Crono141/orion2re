#pragma once

#include <stdint.h>

#include <vector>

namespace stream_source {

    enum e_stream_source_format {
        STREAM_SOURCE_FORMAT_WAV,
        STREAM_SOURCE_FORMAT_OGG_VORBIS,
    };

    struct s_stream_source_info {
        e_stream_source_format format;
        uint16_t channels;
        uint32_t sample_rate;
        uint16_t bits_per_sample;
        // Exact number of PCM bytes Read returns before the end of the source.
        uint32_t total_bytes;
    };

    struct s_stream_source;

    /**
     * Opens an in-memory music entry as a PCM source.
     *
     * Accepts RIFF/WAVE with 8-bit unsigned or 16-bit signed PCM, tolerating
     * extra chunks before or after the data chunk, and Ogg Vorbis, which is
     * decoded to 16-bit signed PCM. The source takes ownership of the bytes.
     * @param bytes Complete entry bytes; emptied on success.
     * @param info Receives the decoded PCM layout.
     * @param error Receives a static description of a rejected entry.
     * @return The opened source, or nullptr when the entry is not supported.
     */
    s_stream_source* Open(std::vector<uint8_t>* bytes, s_stream_source_info* info, const char** error);

    /**
     * Copies the next PCM bytes into buffer.
     * Returns fewer than bytes_to_read only at the end of the source. A Vorbis
     * stream that ends before its declared length is padded with silence so the
     * returned total always matches info.total_bytes.
     */
    uint32_t Read(s_stream_source* source, void* buffer, uint32_t bytes_to_read);

    bool Rewind(s_stream_source* source);

    void Close(s_stream_source* source);

    const char* Format_Name(e_stream_source_format format);

} // namespace stream_source
