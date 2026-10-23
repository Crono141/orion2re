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
        // --skip-intro: go straight to the main menu without the logos and intro movie.
        bool skip_intro;
        // --load-save <n>: load SAVE<n>.GAM (1-10) from the main menu as if chosen there; 0 = off.
        int32_t load_save;
        // Arguments not consumed here, joined by single spaces.
        std::string remaining_command_line;
    };

    /**
     * Parses launcher-facing startup options.
     *
     * Recognized options:
     *   --profile <name>   Use profiles/<name>.cfg as the startup configuration.
     *   --config <path>    Use the VFS path <path> as the startup configuration.
     *   --skip-intro       Skip the logos and the intro movie.
     *   --load-save <n>    Load SAVE<n>.GAM (1-10) straight from the main menu.
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
