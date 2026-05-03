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
#include "alpha_recorder/export_worker.hpp"

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

        alpha_recorder::AlphaLosslessWriter sidecar_writer;
        alpha_recorder::obs::AlphaMaskVideoWriter mask_writer;
        std::ofstream rgb_stream;

        auto open_segment = [&](int split_index) -> bool
        {
            std::filesystem::path suffix = (scenario.expected_split_at_sequence > 0) ? std::filesystem::path("." + std::to_string(split_index)) : std::filesystem::path{};
            std::filesystem::path current_rgb = std::filesystem::path(rgb_path.string() + suffix.string());
            std::filesystem::path current_sidecar = std::filesystem::path(sidecar_path.string() + suffix.string());
            std::filesystem::path current_manifest = std::filesystem::path(manifest_path.string() + suffix.string());

            if (const std::filesystem::path p = current_rgb.parent_path(); !p.empty())
            {
                std::error_code err;
                std::filesystem::create_directories(p, err);
                if (err)
                    return false;
            }

            if (!sidecar_writer.open(current_sidecar, current_manifest))
                return false;
            const alpha_recorder::FramePair first_pair = alpha_recorder::e2e::make_test_pair(0);
            alpha_recorder::obs::AlphaMaskVideoWriterConfig mask_config{};
            mask_config.output_path = current_sidecar.string() + ".alpha.mov";
            mask_config.finalization_format = alpha_recorder::obs::FinalizationFormat::MaskProRes422;
            mask_config.width = first_pair.alpha.width;
            mask_config.height = first_pair.alpha.height;
            mask_config.fps_num = 25U;
            mask_config.fps_den = 1U;
            std::string mask_error;
            if (!mask_writer.open(mask_config, &mask_error))
                return false;
            rgb_stream.open(current_rgb, std::ios::binary | std::ios::trunc);
            return rgb_stream.is_open();
        };

        auto close_segment_and_export = [&](int split_index) -> std::string
        {
            std::filesystem::path suffix = (scenario.expected_split_at_sequence > 0) ? std::filesystem::path("." + std::to_string(split_index)) : std::filesystem::path{};
            std::filesystem::path current_rgb = std::filesystem::path(rgb_path.string() + suffix.string());
            std::filesystem::path current_sidecar = std::filesystem::path(sidecar_path.string() + suffix.string());
            std::filesystem::path current_manifest = std::filesystem::path(manifest_path.string() + suffix.string());
            (void)current_rgb;
            (void)current_sidecar;
            (void)current_manifest;

            rgb_stream.flush();
            rgb_stream.close();
            sidecar_writer.close();

            std::string export_error;
            if (!mask_writer.close(&export_error))
            {
                return export_error;
            }
            return {};
        };

        if (!open_segment(0))
        {
            return "failed to open initial records";
        }

        alpha_recorder::PairAdmissionGate gate;
        gate.reset({scenario.expected_pair_count, scenario.expected_pair_count, scenario.expected_pair_count});

        std::uint64_t accepted_pairs = 0;
        std::uint64_t dropped_pairs = 0;
        int current_split = 0;

        for (std::uint64_t index = 0; index < attempted_pairs; ++index)
        {
            if (scenario.expected_split_at_sequence > 0 && index == scenario.expected_split_at_sequence && accepted_pairs > 0)
            {
                std::string error = close_segment_and_export(current_split);
                if (!error.empty())
                    return error;
                ++current_split;
                if (!open_segment(current_split))
                    return "failed to open new segment";
            }

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

            std::string mask_error;
            if (!mask_writer.write_frame(pair.alpha.bytes.data(), pair.alpha.stride, &mask_error))
            {
                return mask_error.empty() ? std::string{"failed to write alpha mask frame for sequence "} + std::to_string(pair.sequence)
                                          : mask_error;
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

        std::string close_error = close_segment_and_export(current_split);
        if (!close_error.empty())
            return close_error;

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
