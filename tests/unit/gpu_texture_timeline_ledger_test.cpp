#include "gpu_texture_timeline_ledger.hpp"

#include <iostream>

namespace
{
    alpha_recorder::obs::GpuTexturePacketRecord packet(std::int64_t pts,
                                                       std::uint64_t cts = 0U,
                                                       bool has_cts = false)
    {
        return alpha_recorder::obs::GpuTexturePacketRecord{pts, pts, 1001, 60000, true, 0, cts, has_cts};
    }

    alpha_recorder::obs::ProgramRenderRecord render(std::uint64_t generation,
                                                    std::uint64_t time,
                                                    std::uint64_t emitted_generation,
                                                    bool emitted = true)
    {
        return alpha_recorder::obs::ProgramRenderRecord{generation, time, emitted_generation, emitted};
    }
} // namespace

int main()
{
    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        input.main_packets = {packet(0, 2000, true), packet(1001, 3000, true), packet(2002, 4000, true)};
        input.alpha_packets = {packet(0), packet(1001), packet(2002), packet(3003), packet(4004)};
        input.alpha_renders = {
            render(0, 1000, 0, false),
            render(1, 2000, 0),
            render(2, 3000, 1),
            render(3, 4000, 2),
            render(4, 5000, 3),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None)
        {
            std::cerr << "previous-generation solve failed: "
                      << alpha_recorder::obs::timeline_solve_error_name(result.error) << '\n';
            return 1;
        }
        if (result.solution.range.media_time != 1001 || result.solution.range.duration != 3003)
        {
            std::cerr << "59.94fps edit range used the wrong PTS domain\n";
            return 2;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        input.main_packets = {packet(0, 1000, true), packet(1001, 2000, true)};
        input.alpha_packets = {packet(0), packet(1001), packet(2002)};
        input.alpha_renders = {
            render(0, 1000, 0, false),
            render(1, 2000, 0),
            render(2, 3000, 1),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::MissingPrefixContent)
        {
            std::cerr << "missing prefix content was not rejected\n";
            return 3;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {packet(0, 1000, true), packet(1001, 2000, true), packet(2002, 3000, true)};
        input.alpha_packets = {packet(0), packet(1001), packet(3003)};
        input.alpha_renders = {render(0, 1000, 0), render(1, 2000, 1), render(2, 3000, 2)};

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::MissingTailCoverage)
        {
            std::cerr << "non-contiguous visible alpha PTS range was not rejected\n";
            return 4;
        }
    }

    std::cout << "gpu texture timeline ledger test passed\n";
    return 0;
}
