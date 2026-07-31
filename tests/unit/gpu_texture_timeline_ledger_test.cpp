#include "gpu_texture_timeline_ledger.hpp"

#include <iostream>
#include <limits>

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

    alpha_recorder::obs::GpuTexturePacketRecord alpha_sys_dts_packet(std::int64_t pts,
                                                                     std::int64_t sys_dts_usec,
                                                                     std::int64_t dts = std::numeric_limits<std::int64_t>::min(),
                                                                     std::int32_t timebase_num = 1,
                                                                     std::int32_t timebase_den = 1)
    {
        if (dts == std::numeric_limits<std::int64_t>::min())
        {
            dts = pts;
        }
        return alpha_recorder::obs::GpuTexturePacketRecord{pts,
                                                           dts,
                                                           timebase_num,
                                                           timebase_den,
                                                           true,
                                                           sys_dts_usec,
                                                           0,
                                                           false,
                                                           0,
                                                           0,
                                                           false,
                                                           false};
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
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {
            main_packet(0, 1000),
            main_packet(1001, 2000),
            main_packet(2002, 3000),
        };
        input.alpha_packets = {
            alpha_packet_without_generation(0, 1000),
            alpha_packet_without_generation(1001, 2000),
            alpha_packet_without_generation(2002, 3000),
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
        if (result.solution.alpha_epoch_source != alpha_recorder::obs::AlphaEpochSource::DirectCts)
        {
            std::cerr << "live-generation solve did not certify direct CTS\n";
            return 11;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
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
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        input.main_packets = {main_packet(0, 1000), main_packet(1001, 2000)};
        input.alpha_packets = {alpha_packet_without_generation(0, 1000),
                               alpha_packet_without_generation(1001, 2000)};
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
        input.cts_tolerance_ns = 0U;
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
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.fps_num = 1;
        input.fps_den = 1;
        input.main_packets = {
            main_packet(0, 1000, 0, 1, 1),
            main_packet(1, 2000, 1, 1, 1),
            main_packet(2, 3000, 2, 1, 1),
            main_packet(3, 4000, 3, 1, 1),
            main_packet(4, 5000, 4, 1, 1),
        };
        input.alpha_packets = {
            alpha_packet(0, 1000, 0, 0, 1, 1),
            alpha_packet(1, 2000, 1, 1, 1, 1),
            alpha_packet(2, 3000, 1, 2, 1, 1),
            alpha_packet(3, 4000, 1, 3, 1, 1),
            alpha_packet(4, 5000, 2, 4, 1, 1),
        };
        input.alpha_renders = {
            render(0, 1000, 0),
            render(1, 2000, 1),
            render(2, 5000, 2),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None ||
            result.solution.range.media_time != 0 ||
            result.solution.range.duration != 5)
        {
            std::cerr << "repeated main texture generation solve failed: "
                      << alpha_recorder::obs::timeline_solve_error_name(result.error)
                      << " media_time=" << result.solution.range.media_time
                      << " duration=" << result.solution.range.duration << '\n';
            return 17;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.fps_num = 1;
        input.fps_den = 1;
        input.main_packets = {
            main_packet(0, 4000, 0, 1, 1),
            main_packet(1, 5000, 1, 1, 1),
            main_packet(2, 6000, 2, 1, 1),
        };
        input.alpha_packets = {
            alpha_packet(0, 1000, 0, -2, 1, 1),
            alpha_packet(1, 2000, 1, -1, 1, 1),
            alpha_packet(2, 3000, 2, 0, 1, 1),
            alpha_packet(3, 4000, 3, 1, 1, 1),
            alpha_packet(4, 5000, 4, 2, 1, 1),
            alpha_packet(5, 6000, 5, 3, 1, 1),
        };
        input.alpha_renders = {
            render(0, 1000, 0),
            render(1, 2000, 1),
            render(2, 3000, 2),
            render(3, 4000, 3),
            render(4, 5000, 4),
            render(5, 6000, 5),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None ||
            result.solution.first_visible_alpha_pts != 3 ||
            result.solution.range.media_time != 3 ||
            result.solution.range.duration != 3)
        {
            std::cerr << "B-frame edit media_time compensation failed\n";
            return 16;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.fps_num = 1;
        input.fps_den = 1;
        input.main_packets = {main_packet(0, 1000000000, 0, 1, 1),
                              main_packet(1, 2000000000, 1, 1, 1),
                              main_packet(2, 3000000000, 2, 1, 1)};
        input.alpha_packets = {
            alpha_sys_dts_packet(0, 1000000),
            alpha_sys_dts_packet(1, 2000000),
            alpha_sys_dts_packet(2, 3000000),
            alpha_sys_dts_packet(3, 4000000),
            alpha_sys_dts_packet(4, 5000000),
        };
        input.alpha_renders = {
            render(0, 1000000000, 0),
            render(1, 2000000000, 1),
            render(2, 3000000000, 2),
            render(3, 4000000000, 3),
            render(4, 5000000000, 4),
            render(5, 6000000000, 5),
            render(6, 7000000000, 6),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None)
        {
            std::cerr << "sys_dts epoch solve failed: "
                      << alpha_recorder::obs::timeline_solve_error_name(result.error) << '\n';
            return 12;
        }
        if (result.solution.range.media_time != 0 || result.solution.alpha_latency_frames != 2 ||
            result.solution.alpha_epoch_source != alpha_recorder::obs::AlphaEpochSource::SysDts)
        {
            std::cerr << "sys_dts epoch solve used the wrong dynamic latency: media_time="
                      << result.solution.range.media_time << " latency="
                      << result.solution.alpha_latency_frames << " source="
                      << alpha_recorder::obs::alpha_epoch_source_name(result.solution.alpha_epoch_source)
                      << '\n';
            return 13;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.fps_num = 1;
        input.fps_den = 1;
        input.main_packets = {main_packet(0, 1000000000, 0, 1, 1)};
        input.alpha_packets = {
            packet(0, 2000000000, true, 0, false, 0, 1, 1),
            alpha_sys_dts_packet(1, 2000000),
        };
        input.alpha_renders = {
            render(0, 1000000000, 0),
            render(1, 2000000000, 1),
            render(2, 3000000000, 2),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::AmbiguousAlphaEpoch)
        {
            std::cerr << "conflicting sys_dts/direct CTS epoch was not rejected: "
                      << alpha_recorder::obs::timeline_solve_error_name(result.error) << '\n';
            return 14;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.fps_num = 1;
        input.fps_den = 1;
        input.main_packets = {main_packet(0, 1000000000, 0, 1, 1)};
        input.alpha_packets = {
            alpha_sys_dts_packet(0, 1000000),
            alpha_sys_dts_packet(1, 2000000),
        };
        input.alpha_renders = {
            render(0, 1000000000, 0),
            render(1, 2000000000, 1),
            render(2, 3500000000, 2),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::UnsupportedObsTimingModel)
        {
            std::cerr << "non-grid sys_dts epoch was not rejected\n";
            return 15;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
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
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {main_packet(0, 1000), main_packet(1001, 2000), main_packet(2002, 3000)};
        input.alpha_packets = {alpha_packet(0, 1000, 0), packet(1001, 0, false, 0, false),
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
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {main_packet(0, 1000), main_packet(1001, 2000)};
        input.alpha_packets = {alpha_packet_without_generation(0, 1000),
                               alpha_packet_without_generation(1001, 2000)};
        input.alpha_renders = {render(0, 1000, 0), render(1, 2000, 1), render(2, 2000, 2)};

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
        input.cts_tolerance_ns = 0U;
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

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {
            main_packet(0, 1000),
            main_packet(1001, 2000),
            main_packet(3003, 3000),
        };
        input.alpha_packets = {
            alpha_packet(0, 1000, 0),
            alpha_packet(1001, 2000, 1),
            alpha_packet(2002, 3000, 2),
            alpha_packet(3003, 4000, 2),
        };
        input.alpha_renders = {
            render(0, 1000, 0),
            render(1, 2000, 1),
            render(2, 3000, 2),
            render(3, 4000, 2),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None ||
            result.solution.range.media_time != 0 ||
            result.solution.range.duration != 3003)
        {
            std::cerr << "dense alpha input slots did not survive a main PTS gap and tail repeat\n";
            return 18;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase = alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {
            main_packet(0, 1000),
            main_packet(1001, 2000),
            main_packet(2002, 5000),
            main_packet(3003, 6000),
        };
        input.alpha_packets = {
            alpha_packet(0, 1000, 0),
            alpha_packet(1001, 2000, 1),
            alpha_packet(2002, 5000, 4),
            alpha_packet(3003, 6000, 5),
        };
        input.alpha_renders = {
            render(0, 1000, 0),
            render(1, 2000, 1),
            render(2, 3000, 0, false),
            render(3, 4000, 0, false),
            render(4, 5000, 4),
            render(5, 6000, 5),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult result =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (result.error != alpha_recorder::obs::TimelineSolveError::None ||
            result.solution.range.media_time != 0 ||
            result.solution.range.duration != 4004)
        {
            std::cerr << "paused render generations were not skipped without creating a PTS hole\n";
            return 19;
        }
    }

    {
        std::vector<alpha_recorder::obs::GpuTexturePacketRecord> packets{
            main_packet(0, 1000),
            main_packet(3003, 4000),
            main_packet(1001, 2000),
            main_packet(2002, 3000),
        };
        const alpha_recorder::obs::MainPacketLedgerReconcileResult result =
            alpha_recorder::obs::reconcile_main_packet_ledger_to_written_count(
                packets, 3U);
        if (!result.ok || result.callback_packet_count != 4U ||
            result.written_packet_count != 3U ||
            result.removed_unwritten_suffix_packets != 1U ||
            result.removed_unwritten_suffix.size() != 1U ||
            result.removed_unwritten_suffix.front().pts != 2002 ||
            packets.size() != 3U || packets[0].pts != 0 ||
            packets[1].pts != 3003 || packets[2].pts != 1001)
        {
            std::cerr << "unwritten callback suffix was not removed in delivery order\n";
            return 20;
        }
    }

    {
        std::vector<alpha_recorder::obs::GpuTexturePacketRecord> packets{
            main_packet(0, 1000),
            main_packet(1001, 2000),
        };
        const alpha_recorder::obs::MainPacketLedgerReconcileResult result =
            alpha_recorder::obs::reconcile_main_packet_ledger_to_written_count(
                packets, 3U);
        if (result.ok || packets.size() != 2U ||
            result.callback_packet_count != 2U ||
            result.written_packet_count != 3U ||
            result.removed_unwritten_suffix_packets != 0U)
        {
            std::cerr << "missing callback timing was incorrectly synthesized\n";
            return 21;
        }
    }

    {
        std::vector<alpha_recorder::obs::GpuTexturePacketRecord> packets{
            main_packet(0, 1000),
        };
        const alpha_recorder::obs::MainPacketLedgerReconcileResult result =
            alpha_recorder::obs::reconcile_main_packet_ledger_to_written_count(
                packets, 0U);
        if (!result.ok || !packets.empty() ||
            result.removed_unwritten_suffix_packets != 1U)
        {
            std::cerr << "empty written main stream did not clear callback timing\n";
            return 22;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase =
            alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.main_packets = {
            main_packet(0, 1000),
            main_packet(1001, 2000),
            main_packet(2002, 3000),
            main_packet(3003, 4000),
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
            render(3, 4000, 3),
        };

        const alpha_recorder::obs::GpuTextureTimelineSolveResult before =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (before.error !=
            alpha_recorder::obs::TimelineSolveError::MissingTailCoverage)
        {
            std::cerr << "unwritten stop-boundary callback did not reproduce tail failure\n";
            return 23;
        }

        const alpha_recorder::obs::MainPacketLedgerReconcileResult reconcile =
            alpha_recorder::obs::reconcile_main_packet_ledger_to_written_count(
                input.main_packets, 3U);
        const alpha_recorder::obs::GpuTextureTimelineSolveResult after =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (!reconcile.ok ||
            reconcile.removed_unwritten_suffix_packets != 1U ||
            after.error != alpha_recorder::obs::TimelineSolveError::None ||
            after.solution.range.media_time != 0 ||
            after.solution.range.duration != 3003)
        {
            std::cerr << "written main packet reconciliation did not repair tail proof\n";
            return 24;
        }
    }

    {
        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.cts_tolerance_ns = 0U;
        input.main_phase =
            alpha_recorder::obs::MainContentPhase::LiveProgramGeneration;
        input.fps_num = 60U;
        input.fps_den = 1U;
        for (std::int64_t index = 0; index < 120; ++index)
        {
            const std::uint64_t cts =
                1000U + static_cast<std::uint64_t>(index);
            input.main_packets.push_back(
                main_packet(index, cts, index, 1, 60));
            const std::uint64_t alpha_generation =
                index == 40 || index == 41
                    ? static_cast<std::uint64_t>(index + 1)
                    : static_cast<std::uint64_t>(index);
            input.alpha_packets.push_back(
                alpha_packet(index,
                             cts,
                             alpha_generation,
                             index,
                             1,
                             60));
            input.alpha_renders.push_back(
                render(static_cast<std::uint64_t>(index),
                       cts,
                       static_cast<std::uint64_t>(index)));
        }

        const alpha_recorder::obs::GpuTextureTimelineSolveResult strict =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (strict.error !=
            alpha_recorder::obs::TimelineSolveError::MissingTailCoverage)
        {
            std::cerr << "strict timeline unexpectedly accepted transient generation mismatches\n";
            return 25;
        }

        input.allow_transient_generation_mismatch = true;
        const alpha_recorder::obs::GpuTextureTimelineSolveResult recovered =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (recovered.error !=
                alpha_recorder::obs::TimelineSolveError::None ||
            !recovered.solution.recovery_certified ||
            recovered.solution.transient_generation_mismatches != 2U ||
            recovered.solution.terminal_clean_suffix_frames != 78U)
        {
            std::cerr << "timeline did not certify a bounded mismatch with a clean terminal suffix\n";
            return 26;
        }

        input.alpha_packets[110].input_generation = 111U;
        input.alpha_packets[110].emitted_generation = 111U;
        const alpha_recorder::obs::GpuTextureTimelineSolveResult dirty_tail =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        if (dirty_tail.error !=
            alpha_recorder::obs::TimelineSolveError::MissingTailCoverage)
        {
            std::cerr << "timeline accepted a recovery without a long clean terminal suffix\n";
            return 27;
        }
    }

    std::cout << "gpu texture timeline ledger test passed\n";
    return 0;
}
