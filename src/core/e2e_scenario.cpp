#include "alpha_recorder/e2e_scenario.hpp"

#include <charconv>
#include <fstream>
#include <string_view>

namespace alpha_recorder::e2e
{
    namespace
    {

        std::string trim_copy(std::string_view text)
        {
            const std::string_view::size_type first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
            {
                return {};
            }

            const std::string_view::size_type last = text.find_last_not_of(" \t\r\n");
            return std::string{text.substr(first, last - first + 1U)};
        }

        bool parse_uint64(std::string_view text, std::uint64_t &value) noexcept
        {
            std::uint64_t parsed_value = 0;
            const char *const begin = text.data();
            const char *const end = begin + text.size();
            const std::from_chars_result result = std::from_chars(begin, end, parsed_value);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                return false;
            }

            value = parsed_value;
            return true;
        }

        bool validate_relative_path(const std::filesystem::path &path, std::string_view field_name, std::string &error_message)
        {
            if (path.empty())
            {
                error_message.assign(field_name);
                error_message.append(" must not be empty");
                return false;
            }

            if (path.is_absolute())
            {
                error_message.assign(field_name);
                error_message.append(" must be relative");
                return false;
            }

            return true;
        }

    } // namespace

    bool load_scenario(const std::filesystem::path &scenario_path, E2EScenario &scenario, std::string &error_message) noexcept
    {
        try
        {
            std::ifstream stream(scenario_path);
            if (!stream)
            {
                error_message.assign("failed to open scenario file: ");
                error_message.append(scenario_path.generic_string());
                return false;
            }

            bool name_seen = false;
            bool pair_count_seen = false;
            bool drop_count_seen = false;
            bool split_sequence_seen = false;
            bool output_root_seen = false;
            bool rgb_artifact_seen = false;
            bool alpha_sidecar_seen = false;
            bool alpha_manifest_seen = false;

            std::string line;
            while (std::getline(stream, line))
            {
                const std::string trimmed = trim_copy(line);
                if (trimmed.empty() || trimmed.front() == '#')
                {
                    continue;
                }

                const std::string_view trimmed_view{trimmed};
                const std::string_view::size_type equals = trimmed_view.find('=');
                if (equals == std::string_view::npos)
                {
                    error_message.assign("invalid scenario line: ");
                    error_message.append(trimmed);
                    return false;
                }

                const std::string key = trim_copy(trimmed_view.substr(0U, equals));
                const std::string value = trim_copy(trimmed_view.substr(equals + 1U));

                if (key == "name")
                {
                    if (name_seen)
                    {
                        error_message.assign("duplicate scenario entry: name");
                        return false;
                    }

                    scenario.name = value;
                    name_seen = true;
                    continue;
                }

                if (key == "expected_pair_count")
                {
                    if (pair_count_seen)
                    {
                        error_message.assign("duplicate scenario entry: expected_pair_count");
                        return false;
                    }

                    if (!parse_uint64(value, scenario.expected_pair_count))
                    {
                        error_message.assign("invalid unsigned integer for scenario entry: expected_pair_count");
                        return false;
                    }

                    pair_count_seen = true;
                    continue;
                }

                if (key == "expected_drop_count")
                {
                    if (drop_count_seen)
                    {
                        error_message.assign("duplicate scenario entry: expected_drop_count");
                        return false;
                    }

                    if (!parse_uint64(value, scenario.expected_drop_count))
                    {
                        error_message.assign("invalid unsigned integer for scenario entry: expected_drop_count");
                        return false;
                    }

                    drop_count_seen = true;
                    continue;
                }

                if (key == "expected_split_at_sequence")
                {
                    if (split_sequence_seen)
                    {
                        error_message.assign("duplicate scenario entry: expected_split_at_sequence");
                        return false;
                    }

                    if (!parse_uint64(value, scenario.expected_split_at_sequence))
                    {
                        error_message.assign("invalid unsigned integer for scenario entry: expected_split_at_sequence");
                        return false;
                    }

                    split_sequence_seen = true;
                    continue;
                }

                if (key == "output_root")
                {
                    if (output_root_seen)
                    {
                        error_message.assign("duplicate scenario entry: output_root");
                        return false;
                    }

                    scenario.output_root = std::filesystem::path{value};
                    output_root_seen = true;
                    continue;
                }

                if (key == "rgb_artifact")
                {
                    if (rgb_artifact_seen)
                    {
                        error_message.assign("duplicate scenario entry: rgb_artifact");
                        return false;
                    }

                    scenario.rgb_artifact = std::filesystem::path{value};
                    rgb_artifact_seen = true;
                    continue;
                }

                if (key == "alpha_sidecar")
                {
                    if (alpha_sidecar_seen)
                    {
                        error_message.assign("duplicate scenario entry: alpha_sidecar");
                        return false;
                    }

                    scenario.alpha_sidecar = std::filesystem::path{value};
                    alpha_sidecar_seen = true;
                    continue;
                }

                if (key == "alpha_manifest")
                {
                    if (alpha_manifest_seen)
                    {
                        error_message.assign("duplicate scenario entry: alpha_manifest");
                        return false;
                    }

                    scenario.alpha_manifest = std::filesystem::path{value};
                    alpha_manifest_seen = true;
                    continue;
                }
            }

            if (!name_seen || !pair_count_seen || !drop_count_seen || !output_root_seen || !rgb_artifact_seen || !alpha_sidecar_seen || !alpha_manifest_seen)
            {
                error_message.assign("scenario file is missing required fields");
                return false;
            }

            if (scenario.name.empty())
            {
                error_message.assign("name must not be empty");
                return false;
            }

            if (!validate_relative_path(scenario.output_root, "output_root", error_message) || !validate_relative_path(scenario.rgb_artifact, "rgb_artifact", error_message) || !validate_relative_path(scenario.alpha_sidecar, "alpha_sidecar", error_message) || !validate_relative_path(scenario.alpha_manifest, "alpha_manifest", error_message))
            {
                return false;
            }

            return true;
        }
        catch (...)
        {
            error_message.assign("unexpected error while parsing scenario file");
            return false;
        }
    }

    std::filesystem::path resolve_output_root(const std::filesystem::path &artifact_root, const E2EScenario &scenario)
    {
        return artifact_root / scenario.output_root;
    }

    std::filesystem::path resolve_artifact_path(const std::filesystem::path &output_root, const std::filesystem::path &artifact_name)
    {
        return output_root / artifact_name;
    }

    std::uint64_t attempted_pair_count(const E2EScenario &scenario) noexcept
    {
        return scenario.expected_pair_count + scenario.expected_drop_count;
    }

    FramePair make_test_pair(std::uint64_t index) noexcept
    {
        FramePair pair;
        pair.sequence = 1000ULL + index;
        pair.pts = 1'000'000ULL + (index * 40ULL);

        const std::uint32_t rgb_width = static_cast<std::uint32_t>(2ULL + index);
        const std::uint32_t rgb_height = 2U;
        pair.rgb.width = rgb_width;
        pair.rgb.height = rgb_height;
        pair.rgb.stride = rgb_width * 3U;
        pair.rgb.bytes.resize(static_cast<std::size_t>(pair.rgb.stride) * pair.rgb.height);
        for (std::size_t byte_index = 0; byte_index < pair.rgb.bytes.size(); ++byte_index)
        {
            pair.rgb.bytes[byte_index] = static_cast<std::uint8_t>((0x10ULL + index * 13ULL + static_cast<std::uint64_t>(byte_index)) & 0xFFULL);
        }

        pair.alpha.width = 2U;
        pair.alpha.height = static_cast<std::uint32_t>(2ULL + index);
        pair.alpha.stride = 2U;
        pair.alpha.bytes.resize(static_cast<std::size_t>(pair.alpha.stride) * pair.alpha.height);
        for (std::size_t byte_index = 0; byte_index < pair.alpha.bytes.size(); ++byte_index)
        {
            pair.alpha.bytes[byte_index] = static_cast<std::uint8_t>((0xA0ULL + index * 17ULL + static_cast<std::uint64_t>(byte_index)) & 0xFFULL);
        }

        return pair;
    }

} // namespace alpha_recorder::e2e