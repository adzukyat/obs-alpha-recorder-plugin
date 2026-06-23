#pragma once

#include "alpha_output_sink.hpp"
#include "gpu_texture_timeline_ledger.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <obs.h>

namespace alpha_recorder::obs
{

    struct GpuTextureRecordingOutputStats
    {
        std::uint64_t packet_count = 0U;
        std::uint64_t keyframe_count = 0U;
        std::uint64_t packet_bytes = 0U;
        std::uint64_t muxed_packet_count = 0U;
        std::int64_t first_pts = 0;
        std::int64_t last_pts = 0;
        bool finalized = false;
    };

    [[nodiscard]] constexpr const char *gpu_texture_recording_output_id() noexcept
    {
        return "alpha_recorder_gpu_texture_recording_output";
    }

    [[nodiscard]] constexpr const char *gpu_texture_program_alpha_source_id() noexcept
    {
        return "alpha_recorder_program_alpha_source";
    }

    [[nodiscard]] const char *gpu_texture_hevc_encoder_id_for_format(
        FinalizationFormat format) noexcept;
    [[nodiscard]] bool finalization_format_uses_gpu_texture_path(
        FinalizationFormat format) noexcept;
    [[nodiscard]] bool gpu_texture_hevc_encoder_runtime_available(
        FinalizationFormat format,
        std::string *reason = nullptr) noexcept;

    bool register_gpu_texture_recording_output() noexcept;
    void unregister_gpu_texture_recording_output() noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_set_visible_range(
        obs_output_t *output,
        const AlphaVisiblePacketRange &range,
        std::string *error_message = nullptr) noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_finalize_mux(
        obs_output_t *output,
        std::string *error_message = nullptr) noexcept;

    void gpu_texture_recording_output_abort_mux(obs_output_t *output) noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_compute_visible_range(
        obs_output_t *output,
        const std::vector<GpuTexturePacketRecord> &main_packets,
        bool main_texture_encoded,
        AlphaVisiblePacketRange &range,
        std::string *error_message = nullptr) noexcept;

    [[nodiscard]] GpuTextureRecordingOutputStats gpu_texture_recording_output_stats(
        obs_output_t *output) noexcept;

} // namespace alpha_recorder::obs
