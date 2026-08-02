#pragma once

#include "alpha_recorder/plugin.hpp"
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
        std::uint64_t max_buffered_packet_count = 0U;
        std::uint64_t max_buffered_packet_bytes = 0U;
        std::int64_t first_pts = 0;
        std::int64_t last_pts = 0;
        std::uint64_t replay_consumed_entries = 0U;
        std::uint64_t replay_queued_entries = 0U;
        std::uint64_t replay_catchup_slots = 0U;
        std::uint64_t replay_compressed_gap_slots = 0U;
        std::uint64_t replay_skipped_stale_entries = 0U;
        std::uint64_t replay_queue_underflows = 0U;
        std::uint64_t replay_emitted_frames = 0U;
        std::uint64_t replay_repeated_slots = 0U;
        std::uint64_t replay_prefix_repeated_packets = 0U;
        std::uint64_t replay_queue_pending = 0U;
        std::uint64_t replay_missing_textures = 0U;
        std::uint64_t replay_generation_slots = 0U;
        std::uint64_t replay_ambiguous_slots = 0U;
        std::uint64_t lagged_frames_during_capture = 0U;
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

    void gpu_texture_recording_output_request_stop(obs_output_t *output) noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_wait_stop_boundary(
        obs_output_t *output,
        std::uint32_t timeout_ms,
        std::string *error_message = nullptr) noexcept;

    void gpu_texture_recording_output_end_data_capture(obs_output_t *output) noexcept;

    void gpu_texture_recording_output_set_main_texture_encoded(obs_output_t *output,
                                                               bool main_texture_encoded) noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_set_paused(
        obs_output_t *output,
        bool paused,
        std::uint64_t main_pause_offset_ns,
        std::string *error_message = nullptr) noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_begin_delayed_replay(
        obs_output_t *output,
        std::int64_t main_first_packet_pts,
        std::uint64_t main_first_packet_cts,
        std::int64_t safe_main_pts_watermark,
        bool has_safe_main_pts_watermark,
        bool main_packet_input_complete,
        bool main_texture_encoded,
        std::string *error_message = nullptr,
        bool *main_packet_queued = nullptr) noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_queue_main_packet_replay(
        obs_output_t *output,
        std::int64_t main_packet_pts,
        std::uint64_t main_packet_cts,
        std::int64_t safe_main_pts_watermark,
        bool has_safe_main_pts_watermark,
        bool main_packet_input_complete,
        bool main_texture_encoded,
        std::string *error_message = nullptr) noexcept;

    [[nodiscard]] bool gpu_texture_recording_output_wait_deactivated(
        obs_output_t *output,
        std::uint32_t timeout_ms,
        std::string *error_message = nullptr) noexcept;

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
