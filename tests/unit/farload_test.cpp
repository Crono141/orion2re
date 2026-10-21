#include "pch.h"

#include "doctest/doctest.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game/buffer.h"
#include "game/farload.h"
#include "game/string_overrides.h"
#include "game/vfs.h"

#include "miniz.h"

namespace test_farload_buffer_detail {
    namespace fs = std::filesystem;

    class Scoped_Test_Directory {
      public:
        Scoped_Test_Directory() {
            string_overrides::Reset_Cache();
            const auto unique_id = std::chrono::steady_clock::now().time_since_epoch().count();
            root_path_ = fs::temp_directory_path() / ("orion2re-farload-" + std::to_string(unique_id));
            fs::create_directories(root_path_);
        }

        ~Scoped_Test_Directory() {
            vfs::Shutdown();
            std::error_code error;
            fs::remove_all(root_path_, error);
        }

        [[nodiscard]] const fs::path& root_path() const {
            return root_path_;
        }

        void Write_File(const fs::path& relative_path, const std::string& contents) const {
            const fs::path full_path = root_path_ / relative_path;
            fs::create_directories(full_path.parent_path());
            std::ofstream output(full_path, std::ios::binary);
            DOCTEST_REQUIRE(output.good());
            output.write(contents.data(), (std::streamsize)contents.size());
            DOCTEST_REQUIRE(output.good());
        }

        void Write_Lbx_File(const fs::path& relative_path, const std::vector<std::string>& entries) const {
            const fs::path full_path = root_path_ / relative_path;
            s_lbx_disk_header header{};
            uint32_t offsets[510] = {};
            uint32_t next_offset = sizeof(header) + sizeof(offsets);

            fs::create_directories(full_path.parent_path());
            header.entry_count = (uint16_t)entries.size();
            header.magic = 0xFEAD;
            header.header_data = 0;
            offsets[0] = next_offset;

            for (size_t i = 0; i < entries.size(); ++i) {
                next_offset += (uint32_t)entries[i].size();
                offsets[i + 1] = next_offset;
            }

            std::ofstream output(full_path, std::ios::binary);
            DOCTEST_REQUIRE(output.good());
            output.write((const char*)&header, sizeof(header));
            output.write((const char*)offsets, sizeof(offsets));
            for (const std::string& entry : entries) {
                output.write(entry.data(), (std::streamsize)entry.size());
            }
            DOCTEST_REQUIRE(output.good());
        }

        void Write_Zip_File(const fs::path& relative_path, const std::vector<std::pair<std::string, std::string>>& entries) const {
            const fs::path full_path = root_path_ / relative_path;
            mz_zip_archive archive{};

            fs::create_directories(full_path.parent_path());
            mz_zip_zero_struct(&archive);
            DOCTEST_REQUIRE(mz_zip_writer_init_file(&archive, full_path.string().c_str(), 0) == MZ_TRUE);

            for (const auto& entry : entries) {
                DOCTEST_REQUIRE(mz_zip_writer_add_mem(&archive, entry.first.c_str(), entry.second.data(), entry.second.size(), MZ_BEST_COMPRESSION) == MZ_TRUE);
            }

            DOCTEST_REQUIRE(mz_zip_writer_finalize_archive(&archive) == MZ_TRUE);
            DOCTEST_REQUIRE(mz_zip_writer_end(&archive) == MZ_TRUE);
        }

      private:
        fs::path root_path_;
    };

    struct Scoped_Buffer_Pool {
        std::vector<uint8_t> storage;
        uint8_t* payload;

        explicit Scoped_Buffer_Pool(uint32_t payload_size)
            : storage(sizeof(allocate::s_alloc_header) + payload_size), payload(storage.data() + sizeof(allocate::s_alloc_header)) {
            auto* header = (allocate::s_alloc_header*)storage.data();
            header->current = 0;
            header->size = payload_size;
            header->reserved2 = 0;
        }
    };

    std::string Read_File_Contents(const fs::path& full_path) {
        std::ifstream input(full_path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }

    std::string Records(const std::vector<std::string>& strings, uint16_t size) {
        const uint16_t count = (uint16_t)strings.size();
        std::string result((const char*)&count, sizeof(count));
        result.append((const char*)&size, sizeof(size));
        for (const auto& text : strings) {
            DOCTEST_REQUIRE(text.size() <= size);
            result += text;
            result.append(size - text.size(), '\0');
        }
        return result;
    }
} // namespace test_farload_buffer_detail

DOCTEST_TEST_SUITE("farload buffer vfs") {
    DOCTEST_TEST_CASE("JSON5 overrides use caller supplied logical IDs and preserve missing strings") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("KENTEXT.LBX", {Records({"original"}, 100), Records({"unchanged"}, 100)});
        test_dir.Write_File("data/overrides/kentext.json5", "{/* 2: ignored */ 1: 'caf\\u00e9 // 2: text',}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        char output[100] = {};
        string_overrides::Load_Message("KENTEXT.LBX", 0, 0, 100, 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "caf\x82 // 2: text");
        string_overrides::Load_Message("kentext.lbx", 1, 0, 100, 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "caf\x82 // 2: text");
        string_overrides::Load_Message("kentext.lbx", 1, 0, 100, 2, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "unchanged");
        farload::Farload_Data_Static("KENTEXT.LBX", 0, output, 0, 1, 100);
        DOCTEST_CHECK(std::string(output) == "original");
    }

    DOCTEST_TEST_CASE("packed overrides resize strings and retain positional empty strings") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        std::string packed("old\0\0third\0", 11);
        packed.append(809, '\0');
        test_dir.Write_Lbx_File("estrings.lbx", {Records({packed}, 21000)});
        test_dir.Write_File("data/estrings.json5", "{1: 'longer replacement', '3': '', 812: 'last'}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        std::vector<char> output(21000);
        farload::Farload_Data_Static("estrings.lbx", 0, output.data(), 0, 1, (int32_t)output.size());
        string_overrides::Apply_Table("estrings.lbx", &output, 812);
        DOCTEST_CHECK(std::string(output.data()) == "longer replacement");
        const char* cursor = output.data() + strlen(output.data()) + 1;
        DOCTEST_CHECK(*cursor == '\0');
        ++cursor;
        DOCTEST_CHECK(*cursor == '\0');
        for (int i = 2; i < 811; ++i) cursor += strlen(cursor) + 1;
        DOCTEST_CHECK(std::string(cursor) == "last");
    }

    DOCTEST_TEST_CASE("record overrides use explicit group offsets") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("skildesc.lbx", {Records({"first", "second"}, 38), Records({"description"}, 255)});
        test_dir.Write_File("data/skildesc.json5", "{2: 'changed', 3: 'new description'}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        char output[255] = {};
        farload::Farload_Data_Static("skildesc.lbx", 0, output, 1, 1, 38);
        string_overrides::Apply_String("skildesc.lbx", 2, output, 38);
        DOCTEST_CHECK(std::string(output) == "changed");
        farload::Farload_Data_Static("skildesc.lbx", 1, output, 0, 1, 255);
        string_overrides::Apply_String("skildesc.lbx", 3, output, 255);
        DOCTEST_CHECK(std::string(output) == "new description");
    }

    DOCTEST_TEST_CASE("raw custom tables receive overrides before pointer parsing") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        std::string packed;
        for (int i = 0; i < 64; ++i) packed.append("name\0\0", 6);
        test_dir.Write_Lbx_File("racestuf.lbx", {packed});
        test_dir.Write_File("data/racestuf.json5", "{1: 'new name', 64: 'last'}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        auto* output = string_overrides::Load_Raw_Table("racestuf.lbx", 0, nullptr, 0, 64, 1, true);
        DOCTEST_CHECK(std::string(output) == "new name");
        const char* cursor = output;
        for (int i = 0; i < 63; ++i) cursor += strlen(cursor) + 1;
        DOCTEST_CHECK(std::string(cursor) == "last");
        std::free(output);
    }

    DOCTEST_TEST_CASE("invalid sidecars and oversized strings fail without overflowing") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("shipname.lbx", {Records({"old"}, 16)});
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        for (const auto& source : {"{0:'bad'}", "{1:42}", "{1:'a', '1':'b'}", "{1:'\\u0000'}", "[]", "{", "{1:'1234567890123456'}"}) {
            test_dir.Write_File("data/shipname.json5", source);
            string_overrides::Reset_Cache();
            char output[17] = {};
            output[16] = 'X';
            std::string diagnostic;
            DOCTEST_CHECK_FALSE(string_overrides::Try_Apply_String("shipname.lbx", 1, output, 16, &diagnostic));
            DOCTEST_CHECK(diagnostic.find("shipname.json5:") == 0);
            DOCTEST_CHECK(output[16] == 'X');
        }
    }

    DOCTEST_TEST_CASE("sidecars follow VFS precedence changes without cache resets") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_File("data/mods/text/data/custom.json5", "{1: 'package'}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        char output[32] = "original";
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "original");
        auto* transaction = vfs::Begin_Mount_Transaction();
        std::string diagnostic;
        DOCTEST_REQUIRE(vfs::Stage_Package_Mount(transaction, "text", &diagnostic));
        DOCTEST_REQUIRE(vfs::Commit_Mount_Transaction(transaction));
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "package");
        test_dir.Write_File("data/overrides/custom.json5", "{1: 'local'}");
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "local");
        string_overrides::Reset_Cache();
        DOCTEST_CHECK(std::string(output) == "local");
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "local");

        std::error_code error;
        DOCTEST_REQUIRE(fs::remove(test_dir.root_path() / "data/overrides/custom.json5", error));
        DOCTEST_REQUIRE_FALSE(error);
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "package");
        transaction = vfs::Begin_Mount_Transaction();
        DOCTEST_REQUIRE(vfs::Commit_Mount_Transaction(transaction));
        strlcpy(output, "original", sizeof(output));
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "original");
    }

    DOCTEST_TEST_CASE("sidecar edits removal and recreation are detected by content") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("custom.lbx", {Records({"original"}, 32)});
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        char output[32] = {};
        string_overrides::Load_Message("custom.lbx", 0, 0, 32, 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "original");
        test_dir.Write_File("data/custom.json5", "{1:'first'}");
        string_overrides::Load_Message("custom.lbx", 0, 0, 32, 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "first");
        std::error_code error;
        const auto path = test_dir.root_path() / "data/custom.json5";
        const auto timestamp = fs::last_write_time(path, error);
        DOCTEST_REQUIRE_FALSE(error);
        test_dir.Write_File("data/custom.json5", "{1:'other'}");
        fs::last_write_time(path, timestamp, error);
        DOCTEST_REQUIRE_FALSE(error);
        for (int i = 0; i < 2; ++i) {
            string_overrides::Load_Message("custom.lbx", 0, 0, 32, 1, output, sizeof(output));
            DOCTEST_CHECK(std::string(output) == "other");
        }
        DOCTEST_REQUIRE(fs::remove(path, error));
        DOCTEST_REQUIRE_FALSE(error);
        string_overrides::Load_Message("custom.lbx", 0, 0, 32, 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "original");
        test_dir.Write_File("data/custom.json5", "{1:'again'}");
        string_overrides::Load_Message("custom.lbx", 0, 0, 32, 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "again");
    }

    DOCTEST_TEST_CASE("invalid changed sidecars never serve stale overrides") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_File("data/custom.json5", "{1:'valid'}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        char output[32] = "original";
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "valid");
        for (const auto& source : {"{1:'partial', 2:42}", ""}) {
            test_dir.Write_File("data/custom.json5", source);
            for (int i = 0; i < 2; ++i) {
                strlcpy(output, "original", sizeof(output));
                std::string diagnostic;
                DOCTEST_CHECK_FALSE(string_overrides::Try_Apply_String("custom.lbx", 1, output, sizeof(output), &diagnostic));
                DOCTEST_CHECK(diagnostic.find("custom.json5:") == 0);
                DOCTEST_CHECK(std::string(output) == "original");
            }
        }
        test_dir.Write_File("data/custom.json5", "{1:'fixed'}");
        string_overrides::Apply_String("custom.lbx", 1, output, sizeof(output));
        DOCTEST_CHECK(std::string(output) == "fixed");
    }

    DOCTEST_TEST_CASE("sidecar cache follows ZIP contents after VFS reinitialization") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_Zip_File("data/base.zip", {{"custom.json5", "{1:'first'}"}});
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        std::vector<char> table = {'o', '\0'};
        string_overrides::Apply_Table("custom.lbx", &table, 1);
        DOCTEST_CHECK(std::string(table.data()) == "first");
        vfs::Shutdown();
        test_dir.Write_Zip_File("data/base.zip", {{"custom.json5", "{1:'other'}"}});
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        table = {'o', '\0'};
        string_overrides::Apply_Table("custom.lbx", &table, 1);
        DOCTEST_CHECK(std::string(table.data()) == "other");
    }

    DOCTEST_TEST_CASE("table failures are atomic and paths retain their directory") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_File("data/text/custom.json5", "{1:'replacement'}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        std::vector<char> table = {'a', '\0', 'b'};
        const auto original = table;
        std::string diagnostic;
        DOCTEST_CHECK_FALSE(string_overrides::Try_Apply_Table("TEXT/CUSTOM.LBX", &table, 2, 1, false, &diagnostic));
        DOCTEST_CHECK(diagnostic.find("unterminated") != std::string::npos);
        DOCTEST_CHECK(table == original);
        table.push_back('\0');
        string_overrides::Apply_Table("custom.lbx", &table, 2);
        DOCTEST_CHECK(std::string(table.data()) == "a");
    }

    DOCTEST_TEST_CASE("raw tables allocate their final size without overwriting neighboring pool blocks") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("custom.lbx", {std::string("a\0b\0", 4)});
        const std::string replacement(70000, 'L');
        test_dir.Write_File("data/custom.json5", "{1:'" + replacement + "'}");
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        Scoped_Buffer_Pool pool(80000);
        auto* before = (char*)allocate::Allocate_Next_Block_No_Header(pool.payload, 16);
        memset(before, 'B', 16);
        char* table = string_overrides::Load_Raw_Table("custom.lbx", 0, pool.payload, 2, 2);
        DOCTEST_CHECK(std::string(table) == replacement);
        DOCTEST_CHECK(std::string(table + replacement.size() + 1) == "b");
        auto* after = (char*)allocate::Allocate_Next_Block_No_Header(pool.payload, 16);
        DOCTEST_REQUIRE(after != nullptr);
        memset(after, 'A', 16);
        DOCTEST_CHECK(std::string(before, 16) == std::string(16, 'B'));
        DOCTEST_CHECK(std::string(table) == replacement);
        DOCTEST_CHECK(after >= table + replacement.size() + 3);
    }

    DOCTEST_TEST_CASE("dedicated table pools grow and can reload shorter strings") {
        using namespace test_farload_buffer_detail;
        Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("custom.lbx", {std::string("a\0b\0", 4)});
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        void* pool = allocate::Allocate_Space(4);
        for (size_t length : {size_t(40000), size_t(80000), size_t(2)}) {
            const std::string replacement(length, 'X');
            test_dir.Write_File("data/custom.json5", "{1:'" + replacement + "'}");
            char* table = string_overrides::Reload_Raw_Table("custom.lbx", 0, &pool, 2);
            DOCTEST_CHECK(std::string(table) == replacement);
            DOCTEST_CHECK(std::string(table + length + 1) == "b");
            DOCTEST_CHECK(allocate::Get_Max_Block_Space(pool) >= length + 3);
        }
        free((allocate::s_alloc_header*)pool - 1);
    }

    DOCTEST_TEST_CASE("cache invalidation changes resource provenance without moving the arena") {
        test_farload_buffer_detail::Scoped_Test_Directory test_dir;
        test_dir.Write_File("data/TEST.LBX/000", std::string(16, 'B'));
        test_dir.Write_File("data/mods/visual/data/TEST.LBX/000", std::string(31, 'M'));
        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);
        void* pool = buffer::Allocate_Buffer_Space(sizeof(s_file_buffer_entry) * 50 + 1024);
        void* metadata = buffer::file_buffer;
        auto* header = (allocate::s_alloc_header*)pool - 1;
        for (int repeat = 0; repeat < 30; ++repeat) {
            std::string diagnostic;
            auto* transaction = vfs::Begin_Mount_Transaction();
            DOCTEST_REQUIRE(transaction != nullptr);
            if (repeat % 2 != 0) {
                DOCTEST_REQUIRE(vfs::Stage_Package_Mount(transaction, "visual", &diagnostic));
            }
            DOCTEST_REQUIRE(vfs::Commit_Mount_Transaction(transaction));
            buffer::Reset_Buffer_Cache();
            DOCTEST_CHECK(buffer::buffer_space_seg == pool);
            DOCTEST_CHECK(buffer::file_buffer == metadata);
            DOCTEST_CHECK(header->current == 0);
            DOCTEST_CHECK(buffer::file_buffer_count == 0);
            DOCTEST_CHECK(buffer::global_load_count == 0);
            const size_t size = repeat % 2 != 0 ? 31 : 16;
            const char expected = repeat % 2 != 0 ? 'M' : 'B';
            void* loaded = buffer::Buffer_Reload("TEST.LBX", 0, pool);
            DOCTEST_CHECK(std::string((char*)loaded, size) == std::string(size, expected));
            DOCTEST_CHECK(header->current == allocate::Nested_No_Header_Pool_Size((uint32_t)size));
            DOCTEST_CHECK(buffer::Buffer_Reload("test.lbx", 0, pool) == loaded);
            const auto cursor = header->current;
            buffer::Flush_Buffer_Space(nullptr);
            DOCTEST_CHECK(header->current == cursor);
            DOCTEST_CHECK(buffer::file_buffer[0].segments[0].is_loaded == 1);
        }
        buffer::Free_Buffer_Space();
        buffer::Reset_Buffer_Cache();
        DOCTEST_CHECK(buffer::buffer_space_seg == nullptr);
    }

    DOCTEST_TEST_CASE("farload library loads an LBX entry through vfs per-entry override") {
        test_farload_buffer_detail::Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("TEST.LBX", {"raw-zero", "raw-one"});
        test_dir.Write_File(test_farload_buffer_detail::fs::path("data") / "overrides" / "TEST.LBX" / "001", "override-one");

        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);

        char pool[32] = {};
        void* loaded = farload::Farload_Library("TEST.LBX", 1, pool, 0);
        DOCTEST_REQUIRE(loaded != nullptr);
        DOCTEST_CHECK(std::string((const char*)loaded, 12) == "override-one");
        std::free(loaded);
    }

    DOCTEST_TEST_CASE("farload library data reads structured records from whole-LBX zip replacement") {
        test_farload_buffer_detail::Scoped_Test_Directory test_dir;

        std::string structured_entry;
        uint16_t total_count = 3;
        uint16_t element_size = 2;
        structured_entry.append((const char*)&total_count, sizeof(total_count));
        structured_entry.append((const char*)&element_size, sizeof(element_size));
        structured_entry.push_back('A');
        structured_entry.push_back('1');
        structured_entry.push_back('B');
        structured_entry.push_back('2');
        structured_entry.push_back('C');
        structured_entry.push_back('3');

        test_dir.Write_Lbx_File("ZIPSRC.LBX", {structured_entry});
        std::string zip_lbx = test_farload_buffer_detail::Read_File_Contents(test_dir.root_path() / "ZIPSRC.LBX");
        test_dir.Write_Zip_File(test_farload_buffer_detail::fs::path("data") / "base.zip", {
                                                                                               {"TESTDATA.LBX", zip_lbx},
                                                                                           });

        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);

        char output[4] = {};
        void* loaded = farload::Farload_Library_Data("TESTDATA.LBX", 0, output, 3, 1, 2, 2);
        DOCTEST_REQUIRE(loaded == output);
        DOCTEST_CHECK(std::string(output, 4) == "B2C3");
        DOCTEST_CHECK(farload::Get_Farload_Data_Record_Count("TESTDATA.LBX", 0) == 3);
    }

    DOCTEST_TEST_CASE("buffer farload library uses vfs lbx entry resolution") {
        test_farload_buffer_detail::Scoped_Test_Directory test_dir;
        test_dir.Write_Lbx_File("BUFFER.LBX", {"raw-entry"});

        test_dir.Write_Lbx_File("BUFFERZIP.LBX", {"zip-entry"});
        std::string zip_lbx = test_farload_buffer_detail::Read_File_Contents(test_dir.root_path() / "BUFFERZIP.LBX");
        test_dir.Write_Zip_File(test_farload_buffer_detail::fs::path("data") / "base.zip", {
                                                                                               {"BUFFER.LBX", zip_lbx},
                                                                                           });

        DOCTEST_REQUIRE(vfs::Init(test_dir.root_path().string().c_str()) == 1);

        test_farload_buffer_detail::Scoped_Buffer_Pool pool(64);
        void* loaded = buffer::Farload_Buffer_Library("BUFFER.LBX", 0, pool.payload);
        DOCTEST_REQUIRE(loaded != nullptr);
        DOCTEST_CHECK(std::string((const char*)loaded, 9) == "zip-entry");
    }
}