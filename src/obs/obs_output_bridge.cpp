#include "alpha_recorder/plugin.hpp"
#include "alpha_recorder/version.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include "alpha_recorder/e2e_module.hpp"
#include "alpha_recorder/e2e_scenario.hpp"
#include "alpha_recorder/pair_gate.hpp"
#include "alpha_recorder/sidecar_writer.hpp"

#if defined(_WIN32) && defined(ALPHA_RECORDER_BUILD_E2E_MODULE)
#define ALPHA_RECORDER_E2E_EXPORT __declspec(dllexport)
#else
#define ALPHA_RECORDER_E2E_EXPORT
#endif

namespace
{

    void copy_error_message(char *error_message, std::size_t error_message_size, std::string_view message) noexcept
    {
        if (error_message == nullptr || error_message_size == 0U)
        {
            return;
        }

        const std::size_t copy_size = std::min<std::size_t>(error_message_size - 1U, message.size());
        std::memcpy(error_message, message.data(), copy_size);
        error_message[copy_size] = '\0';
    }

    std::string run_e2e_impl(const std::filesystem::path &scenario_path,
                             const std::filesystem::path &output_root,
                             alpha_recorder::e2e::E2ERunResult &result)
    {
        result = {};

        alpha_recorder::e2e::E2EScenario scenario;
        std::string parse_error;
        if (!alpha_recorder::e2e::load_scenario(scenario_path, scenario, parse_error))
        {
            return parse_error;
        }

        if (scenario.name.empty())
        {
            return "scenario name must not be empty";
        }

        if (output_root.empty())
        {
            return "output root must not be empty";
        }

        if (scenario.expected_pair_count > std::numeric_limits<std::uint64_t>::max() - scenario.expected_drop_count)
        {
            return "scenario pair counts overflowed when computing the attempted count";
        }

        const std::uint64_t attempted_pairs = alpha_recorder::e2e::attempted_pair_count(scenario);

        std::error_code remove_error;
        std::filesystem::remove_all(output_root, remove_error);
        if (remove_error)
        {
            return std::string{"failed to clear output root: "} + output_root.generic_string();
        }

        std::error_code directory_error;
        std::filesystem::create_directories(output_root, directory_error);
        if (directory_error)
        {
            return std::string{"failed to create output root: "} + output_root.generic_string();
        }

        const std::filesystem::path rgb_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.rgb_artifact);
        const std::filesystem::path sidecar_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_sidecar);
        const std::filesystem::path manifest_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_manifest);

        if (const std::filesystem::path rgb_parent = rgb_path.parent_path(); !rgb_parent.empty())
        {
            std::error_code rgb_directory_error;
            std::filesystem::create_directories(rgb_parent, rgb_directory_error);
            if (rgb_directory_error)
            {
                return std::string{"failed to create RGB artifact directory: "} + rgb_parent.generic_string();
            }
        }

        alpha_recorder::AlphaLosslessWriter sidecar_writer;
        if (!sidecar_writer.open(sidecar_path, manifest_path))
        {
            return std::string{"failed to open alpha sidecar: "} + sidecar_path.generic_string();
        }

        std::ofstream rgb_stream(rgb_path, std::ios::binary | std::ios::trunc);
        if (!rgb_stream)
        {
            return std::string{"failed to open RGB artifact: "} + rgb_path.generic_string();
        }

        alpha_recorder::PairAdmissionGate gate;
        gate.reset({scenario.expected_pair_count, scenario.expected_pair_count, scenario.expected_pair_count});

        std::uint64_t accepted_pairs = 0;
        std::uint64_t dropped_pairs = 0;

        for (std::uint64_t index = 0; index < attempted_pairs; ++index)
        {
            const alpha_recorder::FramePair pair = alpha_recorder::e2e::make_test_pair(index);
            if (!gate.try_accept(pair))
            {
                ++dropped_pairs;
                continue;
            }

            if (!sidecar_writer.write_pair(pair))
            {
                return std::string{"failed to write alpha sidecar record for sequence "} + std::to_string(pair.sequence);
            }

            rgb_stream.write(reinterpret_cast<const char *>(pair.rgb.bytes.data()), static_cast<std::streamsize>(pair.rgb.bytes.size()));
            if (!rgb_stream)
            {
                return std::string{"failed to write RGB artifact bytes for sequence "} + std::to_string(pair.sequence);
            }

            ++accepted_pairs;
        }

        rgb_stream.flush();
        if (!rgb_stream)
        {
            return std::string{"failed to flush RGB artifact: "} + rgb_path.generic_string();
        }

        rgb_stream.close();
        sidecar_writer.close();

        const alpha_recorder::AlphaSessionSummary &summary = sidecar_writer.summary();
        if (summary.pair_count != accepted_pairs || summary.index_entry_count != accepted_pairs)
        {
            return "sidecar summary did not match the accepted pair count";
        }

        if (!std::filesystem::exists(sidecar_path))
        {
            return std::string{"alpha sidecar was not written: "} + sidecar_path.generic_string();
        }

        if (!std::filesystem::exists(manifest_path))
        {
            return std::string{"manifest was not written: "} + manifest_path.generic_string();
        }

        std::error_code size_error;
        const std::uintmax_t sidecar_size = std::filesystem::file_size(sidecar_path, size_error);
        if (size_error)
        {
            return std::string{"failed to read sidecar file size: "} + sidecar_path.generic_string();
        }

        if (summary.sidecar_size_bytes != sidecar_size)
        {
            return "sidecar size in the summary did not match the on-disk file";
        }

        result.attempted_pairs = attempted_pairs;
        result.accepted_pairs = accepted_pairs;
        result.dropped_pairs = dropped_pairs;
        return {};
    }

} // namespace

namespace alpha_recorder::obs
{

    std::string_view module_name() noexcept
    {
        return project_name();
    }

    std::string_view module_description() noexcept
    {
        return "OBS module wrapper scaffold for alpha_recorder";
    }

    bool register_output_bridge() noexcept
    {
        return !project_version().empty();
    }

    bool initialize_module() noexcept
    {
        return register_output_bridge();
    }

} // namespace alpha_recorder::obs

extern "C" ALPHA_RECORDER_E2E_EXPORT bool alpha_recorder_run_e2e(const char *scenario_path,
                                                                 const char *output_root,
                                                                 alpha_recorder::e2e::E2ERunResult *result,
                                                                 char *error_message,
                                                                 std::size_t error_message_size) noexcept
{
    try
    {
        if (result == nullptr)
        {
            copy_error_message(error_message, error_message_size, "result pointer must not be null");
            return false;
        }

        if (scenario_path == nullptr || *scenario_path == '\0')
        {
            copy_error_message(error_message, error_message_size, "scenario_path must not be empty");
            return false;
        }

        if (output_root == nullptr || *output_root == '\0')
        {
            copy_error_message(error_message, error_message_size, "output_root must not be empty");
            return false;
        }

        const std::string error = run_e2e_impl(std::filesystem::path{scenario_path}, std::filesystem::path{output_root}, *result);
        if (!error.empty())
        {
            copy_error_message(error_message, error_message_size, error);
            return false;
        }

        copy_error_message(error_message, error_message_size, std::string_view{});
        return true;
    }
    catch (...)
    {
        copy_error_message(error_message, error_message_size, "unexpected error while running the E2E module export");
        return false;
    }
}