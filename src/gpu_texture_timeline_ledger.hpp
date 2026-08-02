#pragma once

#include "alpha_output_sink.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alpha_recorder::obs
{

    enum class MainContentPhase
    {
        LiveProgramGeneration,
        PreviousProgramGeneration,
    };

    enum class TimelineSolveError
    {
        None,
        MissingMainPacketTiming,
        MissingAlphaPacketTiming,
        MissingRenderLedger,
        MissingPrefixContent,
        AmbiguousMainGeneration,
        MissingAlphaGeneration,
        AmbiguousGeneration,
        AmbiguousAlphaEpoch,
        NonContiguousPts,
        TimebaseMismatch,
        MissingTailCoverage,
        UnsupportedObsTimingModel,
    };

    enum class AlphaEpochSource
    {
        None,
        DirectCts,
        SysDts,
    };

    struct ProgramRenderRecord
    {
        std::uint64_t generation = 0U;
        std::uint64_t render_time_ns = 0U;
        std::uint64_t emitted_generation = 0U;
        bool emitted = false;
    };

    struct GpuTexturePacketRecord
    {
        std::int64_t pts = 0;
        std::int64_t dts = 0;
        std::int32_t timebase_num = 0;
        std::int32_t timebase_den = 0;
        bool keyframe = false;
        std::int64_t sys_dts_usec = 0;
        std::uint64_t input_cts = 0U;
        bool has_input_cts = false;
        std::uint64_t input_generation = 0U;
        std::uint64_t emitted_generation = 0U;
        bool has_generation = false;
        bool ambiguous_generation = false;
    };

    struct GpuTextureTimelineInput
    {
        std::vector<GpuTexturePacketRecord> main_packets{};
        std::vector<GpuTexturePacketRecord> alpha_packets{};
        std::vector<ProgramRenderRecord> alpha_renders{};
        MainContentPhase main_phase = MainContentPhase::PreviousProgramGeneration;
        std::uint32_t fps_num = 60U;
        std::uint32_t fps_den = 1U;
        std::uint64_t cts_tolerance_ns = 10000U;
        bool allow_transient_generation_mismatch = false;
    };

    struct GpuTextureTimelineSolution
    {
        AlphaVisiblePacketRange range{};
        std::uint64_t main_generation = 0U;
        std::uint64_t alpha_generation = 0U;
        std::int64_t alpha_pts_step = 0;
        std::int64_t first_visible_alpha_pts = 0;
        std::uint64_t main_packet_count = 0U;
        std::uint64_t alpha_packet_count = 0U;
        std::uint64_t alpha_packets_with_generation = 0U;
        AlphaEpochSource alpha_epoch_source = AlphaEpochSource::None;
        std::uint64_t alpha_latency_frames = 0U;
        std::uint64_t alpha_latency_ns = 0U;
        std::uint64_t alpha_epoch_candidate_count = 0U;
        bool recovery_certified = false;
        std::uint64_t transient_generation_mismatches = 0U;
        std::uint64_t terminal_clean_suffix_frames = 0U;
    };

    struct GpuTextureTimelineSolveResult
    {
        TimelineSolveError error = TimelineSolveError::None;
        GpuTextureTimelineSolution solution{};
    };

    struct MainPacketLedgerReconcileResult
    {
        bool ok = false;
        std::uint64_t callback_packet_count = 0U;
        std::uint64_t written_packet_count = 0U;
        std::uint64_t removed_unwritten_suffix_packets = 0U;
        std::vector<GpuTexturePacketRecord> removed_unwritten_suffix{};
    };

    [[nodiscard]] const char *timeline_solve_error_name(TimelineSolveError error) noexcept;
    [[nodiscard]] const char *alpha_epoch_source_name(AlphaEpochSource source) noexcept;
    [[nodiscard]] std::string timeline_solve_error_message(TimelineSolveError error);

    [[nodiscard]] MainPacketLedgerReconcileResult reconcile_main_packet_ledger_to_written_count(
        std::vector<GpuTexturePacketRecord> &packets,
        std::uint64_t written_packet_count) noexcept;

    [[nodiscard]] GpuTextureTimelineSolveResult solve_gpu_texture_timeline(
        const GpuTextureTimelineInput &input) noexcept;

} // namespace alpha_recorder::obs
