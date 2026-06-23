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
        std::uint64_t cts = 0U;
        bool has_cts = false;
    };

    struct GpuTextureTimelineInput
    {
        std::vector<GpuTexturePacketRecord> main_packets{};
        std::vector<GpuTexturePacketRecord> alpha_packets{};
        std::vector<ProgramRenderRecord> alpha_renders{};
        MainContentPhase main_phase = MainContentPhase::PreviousProgramGeneration;
        bool has_alpha_render_packet_offset = false;
        std::uint64_t alpha_render_packet_offset = 0U;
    };

    struct GpuTextureTimelineSolution
    {
        AlphaVisiblePacketRange range{};
        std::uint64_t main_generation = 0U;
        std::uint64_t alpha_generation = 0U;
        std::int64_t alpha_pts_step = 0;
        std::uint64_t alpha_render_index = 0U;
        std::uint64_t main_packet_count = 0U;
        std::uint64_t alpha_packet_count = 0U;
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
