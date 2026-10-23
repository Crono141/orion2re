#include "pch.h"

#include "doctest/doctest.h"
#include "game/stream_source.h"

#include <string>
#include <vector>

namespace {

    void Append_Tag(std::vector<uint8_t>* bytes, const char* tag) {
        bytes->insert(bytes->end(), tag, tag + 4);
    }

    void Append_U16(std::vector<uint8_t>* bytes, uint16_t value) {
        bytes->push_back((uint8_t)(value & 0xFF));
        bytes->push_back((uint8_t)(value >> 8));
    }

    void Append_U32(std::vector<uint8_t>* bytes, uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes->push_back((uint8_t)(value >> shift));
        }
    }

    void Append_Fmt_Chunk(std::vector<uint8_t>* bytes, uint16_t format_tag, uint16_t channels, uint32_t sample_rate, uint16_t bits_per_sample) {
        const uint16_t block_align = (uint16_t)(channels * (bits_per_sample / 8));
        Append_Tag(bytes, "fmt ");
        Append_U32(bytes, 16);
        Append_U16(bytes, format_tag);
        Append_U16(bytes, channels);
        Append_U32(bytes, sample_rate);
        Append_U32(bytes, sample_rate * block_align);
        Append_U16(bytes, block_align);
        Append_U16(bytes, bits_per_sample);
    }

    void Append_Data_Chunk(std::vector<uint8_t>* bytes, const std::vector<uint8_t>& samples) {
        Append_Tag(bytes, "data");
        Append_U32(bytes, (uint32_t)samples.size());
        bytes->insert(bytes->end(), samples.begin(), samples.end());
        if ((samples.size() & 1u) != 0) {
            bytes->push_back(0);
        }
    }

    void Append_Chunk(std::vector<uint8_t>* bytes, const char* tag, uint32_t size) {
        Append_Tag(bytes, tag);
        Append_U32(bytes, size);
        bytes->insert(bytes->end(), size + (size & 1u), 0x5A);
    }

    std::vector<uint8_t> Riff_Header() {
        std::vector<uint8_t> bytes;
        Append_Tag(&bytes, "RIFF");
        Append_U32(&bytes, 0);
        Append_Tag(&bytes, "WAVE");
        return bytes;
    }

    std::vector<uint8_t> Sample_Ramp(size_t count) {
        std::vector<uint8_t> samples(count);
        for (size_t i = 0; i < count; ++i) {
            samples[i] = (uint8_t)i;
        }
        return samples;
    }

    std::vector<uint8_t> Read_Fixture(const char* name) {
        const std::string path = std::string(AUDIO_FIXTURE_DIR) + "/" + name;
        std::vector<uint8_t> bytes;
        FILE* file = fopen(path.c_str(), "rb");
        if (file == nullptr) {
            return bytes;
        }
        uint8_t buffer[4096];
        size_t count = 0;
        while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
            bytes.insert(bytes.end(), buffer, buffer + count);
        }
        fclose(file);
        return bytes;
    }

    std::vector<uint8_t> Read_All(stream_source::s_stream_source* source, uint32_t block_size) {
        std::vector<uint8_t> output;
        std::vector<uint8_t> block(block_size);
        for (;;) {
            const uint32_t count = stream_source::Read(source, block.data(), block_size);
            output.insert(output.end(), block.begin(), block.begin() + count);
            if (count < block_size) {
                break;
            }
        }
        return output;
    }

} // namespace

DOCTEST_TEST_CASE("canonical 44-byte WAV entries decode unchanged") {
    const std::vector<uint8_t> samples = Sample_Ramp(400);
    std::vector<uint8_t> bytes = Riff_Header();
    Append_Fmt_Chunk(&bytes, 1, 2, 22050, 16);
    Append_Data_Chunk(&bytes, samples);

    stream_source::s_stream_source_info info = {};
    const char* error = nullptr;
    stream_source::s_stream_source* source = stream_source::Open(&bytes, &info, &error);
    DOCTEST_REQUIRE(source != nullptr);
    DOCTEST_CHECK(info.format == stream_source::STREAM_SOURCE_FORMAT_WAV);
    DOCTEST_CHECK(info.channels == 2);
    DOCTEST_CHECK(info.sample_rate == 22050);
    DOCTEST_CHECK(info.bits_per_sample == 16);
    DOCTEST_CHECK(info.total_bytes == samples.size());
    DOCTEST_CHECK(Read_All(source, 96) == samples);

    DOCTEST_CHECK(stream_source::Rewind(source));
    DOCTEST_CHECK(Read_All(source, 1000) == samples);
    stream_source::Close(source);
}

DOCTEST_TEST_CASE("WAV entries with metadata chunks locate the data chunk") {
    const std::vector<uint8_t> samples = Sample_Ramp(101);
    std::vector<uint8_t> bytes = Riff_Header();
    Append_Chunk(&bytes, "LIST", 27);
    Append_Fmt_Chunk(&bytes, 1, 1, 44100, 8);
    Append_Chunk(&bytes, "fact", 4);
    Append_Data_Chunk(&bytes, samples);
    Append_Chunk(&bytes, "id3 ", 10);

    stream_source::s_stream_source_info info = {};
    stream_source::s_stream_source* source = stream_source::Open(&bytes, &info, nullptr);
    DOCTEST_REQUIRE(source != nullptr);
    DOCTEST_CHECK(info.bits_per_sample == 8);
    DOCTEST_CHECK(info.total_bytes == samples.size());
    DOCTEST_CHECK(Read_All(source, 64) == samples);
    stream_source::Close(source);
}

DOCTEST_TEST_CASE("WAV data is clamped to the entry and to whole frames") {
    std::vector<uint8_t> bytes = Riff_Header();
    Append_Fmt_Chunk(&bytes, 1, 2, 22050, 16);
    Append_Tag(&bytes, "data");
    Append_U32(&bytes, 1000);
    const std::vector<uint8_t> samples = Sample_Ramp(10);
    bytes.insert(bytes.end(), samples.begin(), samples.end());

    stream_source::s_stream_source_info info = {};
    stream_source::s_stream_source* source = stream_source::Open(&bytes, &info, nullptr);
    DOCTEST_REQUIRE(source != nullptr);
    DOCTEST_CHECK(info.total_bytes == 8);
    stream_source::Close(source);
}

DOCTEST_TEST_CASE("unsupported or malformed entries are rejected without consuming the bytes") {
    struct s_case {
        const char* name;
        std::vector<uint8_t> bytes;
    };
    std::vector<s_case> cases;

    cases.push_back({"empty", {}});
    cases.push_back({"not riff", {'J', 'U', 'N', 'K', 0, 0, 0, 0, 'W', 'A', 'V', 'E'}});

    std::vector<uint8_t> no_data = Riff_Header();
    Append_Fmt_Chunk(&no_data, 1, 1, 8000, 8);
    cases.push_back({"missing data", no_data});

    std::vector<uint8_t> no_fmt = Riff_Header();
    Append_Data_Chunk(&no_fmt, Sample_Ramp(8));
    cases.push_back({"missing fmt", no_fmt});

    std::vector<uint8_t> float_samples = Riff_Header();
    Append_Fmt_Chunk(&float_samples, 3, 1, 8000, 32);
    Append_Data_Chunk(&float_samples, Sample_Ramp(8));
    cases.push_back({"float", float_samples});

    std::vector<uint8_t> adpcm = Riff_Header();
    Append_Fmt_Chunk(&adpcm, 2, 1, 8000, 4);
    Append_Data_Chunk(&adpcm, Sample_Ramp(8));
    cases.push_back({"adpcm", adpcm});

    std::vector<uint8_t> zero_channels = Riff_Header();
    Append_Fmt_Chunk(&zero_channels, 1, 0, 8000, 8);
    Append_Data_Chunk(&zero_channels, Sample_Ramp(8));
    cases.push_back({"zero channels", zero_channels});

    std::vector<uint8_t> bad_ogg = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0};
    cases.push_back({"truncated ogg", bad_ogg});

    for (s_case& test_case : cases) {
        DOCTEST_CAPTURE(test_case.name);
        const size_t original_size = test_case.bytes.size();
        stream_source::s_stream_source_info info = {};
        const char* error = nullptr;
        DOCTEST_CHECK(stream_source::Open(&test_case.bytes, &info, &error) == nullptr);
        DOCTEST_CHECK(error != nullptr);
        DOCTEST_CHECK(test_case.bytes.size() == original_size);
    }
}

DOCTEST_TEST_CASE("Ogg Vorbis entries decode to 16-bit PCM of the declared length") {
    std::vector<uint8_t> bytes = Read_Fixture("tone_stereo_22050.ogg");
    DOCTEST_REQUIRE(!bytes.empty());

    stream_source::s_stream_source_info info = {};
    const char* error = nullptr;
    stream_source::s_stream_source* source = stream_source::Open(&bytes, &info, &error);
    DOCTEST_REQUIRE_MESSAGE(source != nullptr, (error != nullptr ? error : ""));
    DOCTEST_CHECK(info.format == stream_source::STREAM_SOURCE_FORMAT_OGG_VORBIS);
    DOCTEST_CHECK(info.channels == 2);
    DOCTEST_CHECK(info.sample_rate == 22050);
    DOCTEST_CHECK(info.bits_per_sample == 16);
    // 0.5 s at 22050 Hz, two 16-bit channels.
    DOCTEST_CHECK(info.total_bytes == 11025u * 2u * 2u);

    // An odd block size exercises reads that split decoded frames.
    const std::vector<uint8_t> first = Read_All(source, 7055);
    DOCTEST_CHECK(first.size() == info.total_bytes);

    bool has_signal = false;
    for (size_t i = 0; i + 1 < first.size(); i += 2) {
        const int16_t sample = (int16_t)(first[i] | (first[i + 1] << 8));
        if (sample > 1000 || sample < -1000) {
            has_signal = true;
            break;
        }
    }
    DOCTEST_CHECK(has_signal);

    DOCTEST_CHECK(stream_source::Rewind(source));
    const std::vector<uint8_t> second = Read_All(source, 4096);
    DOCTEST_CHECK(second == first);
    stream_source::Close(source);
}
