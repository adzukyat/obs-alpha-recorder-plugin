#include "alpha_recorder/plugin.hpp"
#include "alpha_recorder/version.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <obs-module.h>

#include "alpha_recorder/e2e_scenario.hpp"
#include "alpha_recorder/pair_gate.hpp"
#include "alpha_recorder/sidecar_writer.hpp"

namespace
{

    struct AlphaRecorderOutputContext
    {
        obs_output_t *output = nullptr;
        std::filesystem::path scenario_path{};
        std::filesystem::path artifact_root{};
    };

    std::string run_e2e_impl(const std::filesystem::path &scenario_path, const std::filesystem::path &output_root)
    {
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

        if (accepted_pairs != scenario.expected_pair_count || dropped_pairs != scenario.expected_drop_count)
        {
            return "accepted or dropped pair counts did not match the scenario expectations";
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

        return {};
    }

    const char *alpha_recorder_output_get_name(void *)
    {
        return "Alpha Recorder";
    }

    void *alpha_recorder_output_create(obs_data_t *settings, obs_output_t *output)
    {
        auto *context = new AlphaRecorderOutputContext{};
        context->output = output;

        if (settings != nullptr)
        {
            const char *scenario_path = obs_data_get_string(settings, "scenario_path");
            if (scenario_path != nullptr && *scenario_path != '\0')
            {
                context->scenario_path = std::filesystem::path{scenario_path};
            }

            const char *artifact_root = obs_data_get_string(settings, "artifact_root");
            if (artifact_root != nullptr && *artifact_root != '\0')
            {
                context->artifact_root = std::filesystem::path{artifact_root};
            }
        }

        return context;
    }

    void alpha_recorder_output_destroy(void *data)
    {
        delete static_cast<AlphaRecorderOutputContext *>(data);
    }

    bool alpha_recorder_output_start(void *data)
    {
        auto *context = static_cast<AlphaRecorderOutputContext *>(data);
        if (context == nullptr || context->output == nullptr)
        {
            return false;
        }

        if (context->scenario_path.empty())
        {
            obs_output_set_last_error(context->output, "scenario_path is required");
            return false;
        }

        if (context->artifact_root.empty())
        {
            obs_output_set_last_error(context->output, "artifact_root is required");
            return false;
        }

        const std::string error = run_e2e_impl(context->scenario_path, context->artifact_root);
        if (!error.empty())
        {
            obs_output_set_last_error(context->output, error.c_str());
            return false;
        }

        obs_output_set_last_error(context->output, nullptr);
        return true;
    }

    void alpha_recorder_output_stop(void *data, uint64_t)
    {
        (void)data;
    }

} // namespace

namespace alpha_recorder::obs
{

    bool register_output_module() noexcept
    {
        obs_output_info info = {};
        info.id = "alpha_recorder_output";
        info.flags = 0;
        info.get_name = alpha_recorder_output_get_name;
        info.create = alpha_recorder_output_create;
        info.destroy = alpha_recorder_output_destroy;
        info.start = alpha_recorder_output_start;
        info.stop = alpha_recorder_output_stop;

        obs_register_output(&info);
        return true;
    }

} // namespace alpha_recorder::obs