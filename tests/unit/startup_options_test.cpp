#include "pch.h"

#include "config/startup_options.h"
#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace {

    bool Parse(const std::vector<const char*>& arguments, config::s_startup_options* options, std::string* error) {
        std::vector<const char*> argv;
        argv.push_back("orion2re");
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        return config::Parse_Startup_Options((int)argv.size(), argv.data(), options, error);
    }

} // namespace

DOCTEST_TEST_CASE("startup options default to orion2re.cfg and forward every argument") {
    config::s_startup_options options;
    std::string error;
    DOCTEST_REQUIRE(Parse({}, &options, &error));
    DOCTEST_CHECK(options.config_path == "orion2re.cfg");
    DOCTEST_CHECK_FALSE(options.config_selected);
    DOCTEST_CHECK(options.remaining_command_line.empty());

    DOCTEST_REQUIRE(Parse({"-seed", "42", "--profiler"}, &options, &error));
    DOCTEST_CHECK(options.config_path == "orion2re.cfg");
    DOCTEST_CHECK(options.remaining_command_line == "-seed 42 --profiler");
}

DOCTEST_TEST_CASE("--profile selects a configuration below profiles/") {
    config::s_startup_options options;
    std::string error;
    DOCTEST_REQUIRE(Parse({"--profile", "v150i-classic"}, &options, &error));
    DOCTEST_CHECK(options.config_path == "profiles/v150i-classic.cfg");
    DOCTEST_CHECK(options.config_selected);

    DOCTEST_REQUIRE(Parse({"-x", "--profile=Melee_2.0", "-y"}, &options, &error));
    DOCTEST_CHECK(options.config_path == "profiles/Melee_2.0.cfg");
    DOCTEST_CHECK(options.remaining_command_line == "-x -y");
}

DOCTEST_TEST_CASE("--config selects an arbitrary VFS entry point") {
    config::s_startup_options options;
    std::string error;
    DOCTEST_REQUIRE(Parse({"--config", "launcher/current.cfg"}, &options, &error));
    DOCTEST_CHECK(options.config_path == "launcher/current.cfg");
    DOCTEST_CHECK(options.config_selected);

    DOCTEST_REQUIRE(Parse({"--config=test.cfg"}, &options, &error));
    DOCTEST_CHECK(options.config_path == "test.cfg");
}

DOCTEST_TEST_CASE("invalid startup options are rejected with a diagnostic") {
    const std::vector<std::vector<const char*>> cases = {
        {"--profile"},
        {"--profile="},
        {"--config"},
        {"--profile", "../escape"},
        {"--profile", "a/b"},
        {"--profile", ".hidden"},
        {"--profile", "with space"},
        {"--profile", "a", "--profile", "b"},
        {"--profile", "a", "--config", "b.cfg"},
        {"--config", "b.cfg", "--profile", "a"},
    };
    for (const std::vector<const char*>& arguments : cases) {
        DOCTEST_CAPTURE(arguments.size());
        config::s_startup_options options;
        std::string error;
        DOCTEST_CHECK_FALSE(Parse(arguments, &options, &error));
        DOCTEST_CHECK_FALSE(error.empty());
    }
}
