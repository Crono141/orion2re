#include "pch.h"

#include "config/startup_options.h"

namespace config {

    namespace {

        bool Is_Valid_Profile_Name(const std::string& name) {
            if (name.empty() || name[0] == '.') {
                return false;
            }
            for (const char c : name) {
                const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
                if (!valid) {
                    return false;
                }
            }
            return true;
        }

        // Matches "--name value" and "--name=value". Advances *index past a
        // separate value argument.
        bool Match_Option(int argc, const char* const* argv, int* index, const char* name, bool* matched, std::string* value, std::string* error) {
            const char* argument = argv[*index];
            const size_t name_length = strlen(name);
            *matched = false;
            if (strncmp(argument, name, name_length) != 0) {
                return true;
            }
            if (argument[name_length] == '=') {
                *value = argument + name_length + 1;
            } else if (argument[name_length] == '\0') {
                if (*index + 1 >= argc) {
                    *error = std::string(name) + " requires a value";
                    return false;
                }
                *index += 1;
                *value = argv[*index];
            } else {
                return true;
            }
            if (value->empty()) {
                *error = std::string(name) + " requires a value";
                return false;
            }
            *matched = true;
            return true;
        }

    } // namespace

    bool Parse_Startup_Options(int argc, const char* const* argv, s_startup_options* options, std::string* error) {
        if (options == nullptr || error == nullptr) {
            return false;
        }
        *options = {};
        options->config_path = STARTUP_DEFAULT_CONFIG_PATH;
        error->clear();

        const char* selected_by = nullptr;
        for (int index = 1; index < argc; ++index) {
            if (argv[index] == nullptr) {
                continue;
            }

            bool matched = false;
            std::string value;
            if (!Match_Option(argc, argv, &index, "--profile", &matched, &value, error)) {
                return false;
            }
            if (matched) {
                if (!Is_Valid_Profile_Name(value)) {
                    *error = "invalid profile name '" + value + "'; use letters, digits, '-', '_' or '.'";
                    return false;
                }
                if (selected_by != nullptr) {
                    *error = std::string("--profile conflicts with an earlier ") + selected_by;
                    return false;
                }
                selected_by = "--profile";
                options->config_path = std::string(STARTUP_PROFILE_DIRECTORY) + "/" + value + ".cfg";
                options->config_selected = true;
                continue;
            }

            if (!Match_Option(argc, argv, &index, "--config", &matched, &value, error)) {
                return false;
            }
            if (matched) {
                if (selected_by != nullptr) {
                    *error = std::string("--config conflicts with an earlier ") + selected_by;
                    return false;
                }
                selected_by = "--config";
                options->config_path = value;
                options->config_selected = true;
                continue;
            }

            if (!options->remaining_command_line.empty()) {
                options->remaining_command_line += ' ';
            }
            options->remaining_command_line += argv[index];
        }
        return true;
    }

} // namespace config
