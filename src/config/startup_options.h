#pragma once

#include <string>

namespace config {

#define STARTUP_DEFAULT_CONFIG_PATH "orion2re.cfg"
#define STARTUP_PROFILE_DIRECTORY "profiles"

    struct s_startup_options {
        // VFS path of the startup configuration entry point.
        std::string config_path;
        // True when --profile or --config selected the entry point, which then
        // must exist instead of falling back to defaults.
        bool config_selected;
        // Arguments not consumed here, joined by single spaces.
        std::string remaining_command_line;
    };

    /**
     * Parses launcher-facing startup options.
     *
     * Recognized options:
     *   --profile <name>   Use profiles/<name>.cfg as the startup configuration.
     *   --config <path>    Use the VFS path <path> as the startup configuration.
     * Both also accept the --option=value form. Profile names may contain
     * letters, digits, '-', '_' and '.', but may not start with '.'.
     * @param argc Argument count, including the program name.
     * @param argv Argument vector.
     * @param options Receives the selected configuration and remaining arguments.
     * @param error Receives a description of invalid usage.
     * @return False when the options are invalid.
     */
    bool Parse_Startup_Options(int argc, const char* const* argv, s_startup_options* options, std::string* error);

} // namespace config
