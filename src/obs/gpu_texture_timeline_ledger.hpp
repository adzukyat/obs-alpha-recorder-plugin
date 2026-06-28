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
        NonContiguousPts,
        TimebaseMismatch,
        MissingTailCoverage,
        UnsupportedObsTimingModel,
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
    };

    struct GpuTextureTimelineSolveResult
    {
        TimelineSolveError error = TimelineSolveError::None;
        GpuTextureTimelineSolution solution{};
    };

    [[nodiscard]] const char *timeline_solve_error_name(TimelineSolveError error) noexcept;
    [[nodiscard]] std::string timeline_solve_error_message(TimelineSolveError error);

    [[nodiscard]] GpuTextureTimelineSolveResult solve_gpu_texture_timeline(
        const GpuTextureTimelineInput &input) noexcept;

} // namespace alpha_recorder::obs
