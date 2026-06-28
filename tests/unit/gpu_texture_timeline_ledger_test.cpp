#include "gpu_texture_timeline_ledger.hpp"

#include <iostream>

namespace
{
    alpha_recorder::obs::GpuTexturePacketRecord packet(std::int64_t pts,
                                                       std::uint64_t cts,
                                                       bool has_cts,
                                                       std::uint64_t emitted_generation,
                                                       bool has_generation,
                                                       std::int64_t dts = 0,
                                                       std::int32_t timebase_num = 1001,
                                                       std::int32_t timebase_den = 60000,
                                                       bool ambiguous_generation = false)
    {
        return alpha_recorder::obs::GpuTexturePacketRecord{pts,
                                                           dts,
                                                           timebase_num,
                                                           timebase_den,
                                                           true,
                                                           0,
                                                           cts,
                                                           has_cts,
                                                           emitted_generation,
                                                           emitted_generation,
                                                           has_generation,
                                                           ambiguous_generation};
    }

    alpha_recorder::obs::GpuTexturePacketRecord main_packet(std::int64_t pts,
                                                            std::uint64_t cts,
                                                            std::int64_t dts = 0,
                                                            std::int32_t timebase_num = 1001,
                                                            std::int32_t timebase_den = 60000)
    {
        return packet(pts, cts, true, 0, false, dts, timebase_num, timebase_den);
    }

    alpha_recorder::obs::GpuTexturePacketRecord alpha_packet(std::int64_t pts,
                                                             std::uint64_t cts,
                                                             std::uint64_t emitted_generation,
                                                             std::int64_t dts = 0,
                                                             std::int32_t timebase_num = 1001,
                                                             std::int32_t timebase_den = 60000)
    {
        return packet(pts, cts, true, emitted_generation, true, dts, timebase_num, timebase_den);
    }

    alpha_recorder::obs::GpuTexturePacketRecord alpha_packet_without_generation(std::int64_t pts,
                                                                                std::uint64_t cts)
    {
        return packet(pts, cts, true, 0, false);
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
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {
            main_packet(0, 1000),
            main_packet(1001, 2000),
            main_packet(2002, 3000),
        };
        input.alpha_packets = {
            alpha_packet(0, 1000, 0),
            alpha_packet(1001, 2000, 1),
            alpha_packet(2002, 3000, 2),
        };
        input.alpha_renders = {
            render(0, 1000, 0),
            render(1, 2000, 1),
            render(2, 3000, 2),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None)
        {
            std::cerr << "live-generation solve failed: "
                      << alpha_recorder::obs::timeline_solve_error_name(result.error) << '\n';
            return 1;
        }
        if (result.solution.range.media_time != 0 || result.solution.range.duration != 3003)
        {
            std::cerr << "live-generation solve used a fixed content delay\n";
            return 2;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        input.main_packets = {
            main_packet(0, 2000),
            main_packet(1001, 3000),
            main_packet(2002, 4000),
        };
        input.alpha_packets = {
            alpha_packet_without_generation(0, 1000),
            alpha_packet(1001, 2000, 0),
            alpha_packet(2002, 3000, 1),
            alpha_packet(3003, 4000, 2),
        };
        input.alpha_renders = {
            render(0, 1000, 0, false),
            render(1, 2000, 0),
            render(2, 3000, 1),
            render(3, 4000, 2),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None)
        {
            std::cerr << "previous-generation solve failed: "
                      << alpha_recorder::obs::timeline_solve_error_name(result.error) << '\n';
            return 3;
        }
        if (result.solution.range.media_time != 1001 || result.solution.range.duration != 3003)
        {
            std::cerr << "previous-generation solve did not trim to the retained generation\n";
            return 4;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        input.main_packets = {main_packet(0, 1000), main_packet(1001, 2000)};
        input.alpha_packets = {alpha_packet_without_generation(0, 1000), alpha_packet(1001, 2000, 1)};
        input.alpha_renders = {render(0, 1000, 0, false), render(1, 2000, 0)};

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::MissingPrefixContent)
        {
            std::cerr << "missing previous generation was not rejected\n";
            return 5;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {
            main_packet(0, 1000, 0),
            main_packet(1001, 2000, -1001),
            main_packet(2002, 3000, 1001),
        };
        input.alpha_packets = {
            alpha_packet(2002, 3000, 2, 0),
            alpha_packet(0, 1000, 0, -2002),
            alpha_packet(1001, 2000, 1, -1001),
        };
        input.alpha_renders = {
            render(0, 1000, 0),
            render(1, 2000, 1),
            render(2, 3000, 2),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None ||
            result.solution.range.media_time != 0)
        {
            std::cerr << "B-frame/reordered packet solve failed\n";
            return 6;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {
            main_packet(0, 1000, 0, 1, 30),
            main_packet(1, 2000, 1, 1, 30),
            main_packet(2, 3000, 2, 1, 30),
        };
        input.alpha_packets = {
            alpha_packet(0, 1000, 0, 0, 1, 30),
            alpha_packet(1, 2000, 1, 1, 1, 30),
            alpha_packet(2, 3000, 2, 2, 1, 30),
        };
        input.alpha_renders = {render(0, 1000, 0), render(1, 2000, 1), render(2, 3000, 2)};

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None ||
            result.solution.range.duration != 3)
        {
            std::cerr << "30fps PTS-domain duration solve failed\n";
            return 7;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {main_packet(0, 1000), main_packet(1001, 2000), main_packet(2002, 3000)};
        input.alpha_packets = {alpha_packet(0, 1000, 0), alpha_packet_without_generation(1001, 2000),
                               alpha_packet(2002, 3000, 2)};
        input.alpha_renders = {render(0, 1000, 0), render(1, 2000, 1), render(2, 3000, 2)};

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::MissingAlphaGeneration)
        {
            std::cerr << "visible alpha packet without generation was not rejected\n";
            return 8;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {main_packet(0, 1000), main_packet(1001, 2000)};
        input.alpha_packets = {packet(0, 1000, true, 0, true, 0, 1001, 60000, true),
                               alpha_packet(1001, 2000, 1)};
        input.alpha_renders = {render(0, 1000, 0), render(1, 2000, 1)};

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::AmbiguousGeneration)
        {
            std::cerr << "ambiguous alpha generation was not rejected\n";
            return 9;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {main_packet(0, 1000), main_packet(1001, 2000), main_packet(2002, 3000)};
        input.alpha_packets = {alpha_packet(0, 1000, 0), alpha_packet(1001, 2000, 1),
                               alpha_packet(3003, 3000, 2)};
        input.alpha_renders = {render(0, 1000, 0), render(1, 2000, 1), render(2, 3000, 2)};

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::MissingTailCoverage)
        {
            std::cerr << "tail gap was not rejected\n";
            return 10;
        }
    }

    std::cout << "gpu texture timeline ledger test passed\n";
    return 0;
}
