#include "alpha_alignment_engine.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    bool expect(bool condition, const char *message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            return false;
        }

        return true;
    }

    alpha_recorder::obs::AlphaFrame make_alpha_frame(std::uint64_t timestamp, std::uint8_t value)
    {
        return alpha_recorder::obs::AlphaFrame{
            timestamp, std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{value})};
    }

    struct WriteCapture
    {
        std::vector<alpha_recorder::obs::AlphaFrame> frames{};
        bool fail_next = false;

        bool write(const alpha_recorder::obs::AlphaFrame &frame, bool &queued, std::string &error_message)
        {
            if (fail_next)
            {
                fail_next = false;
                error_message = "injected write failure";
                return false;
            }

            queued = true;
            frames.push_back(frame);
            return true;
        }
    };

    alpha_recorder::obs::AlphaAlignmentEngine make_engine(std::size_t alpha_limit = 8U,
                                                          std::size_t output_limit = 8U,
                                                          std::size_t reorder_frames = 0U)
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine;
        engine.configure(alpha_recorder::obs::AlphaAlignmentEngineConfig{2U, 2U, 60U, 1U,
                                                                         alpha_limit, output_limit, reorder_frames});
        return engine;
    }

    bool drain_once(alpha_recorder::obs::AlphaAlignmentEngine &engine,
                    alpha_recorder::obs::LivePipelineTelemetry &telemetry,
                    WriteCapture &writer,
                    const char *label,
                    std::vector<alpha_recorder::obs::AlignmentTraceEvent> *traces = nullptr,
                    bool drain_all = false)
    {
        const alpha_recorder::obs::AlphaAlignmentDrainResult result =
            engine.drain(drain_all,
                         1U,
                         telemetry,
                         [&writer](const alpha_recorder::obs::AlphaFrame &frame,
                                   bool &queued,
                                   std::string &error_message) {
                             return writer.write(frame, queued, error_message);
                         },
                         [traces](const alpha_recorder::obs::AlignmentTraceEvent &event) {
                             if (traces != nullptr)
                             {
                                 traces->push_back(event);
                             }
                         });

        if (result.failed)
        {
            std::cerr << label << ": " << (result.error_message.empty() ? "alignment drain failed"
                                                                        : result.error_message)
                      << '\n';
            return false;
        }
        if (result.drained_frames != 1U)
        {
            std::cerr << label << ": alignment drain emitted " << result.drained_frames
                      << " frames instead of exactly one\n";
            return false;
        }

        return true;
    }

    bool test_exact_packet_output_alpha_match_writes_frame()
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine = make_engine();
        alpha_recorder::obs::LivePipelineTelemetry telemetry{};
        WriteCapture writer;
        std::vector<alpha_recorder::obs::AlignmentTraceEvent> traces;

        engine.remember_alpha_frame(make_alpha_frame(100U, 42U), telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{100U, 100U, false}, telemetry);
        engine.queue_packet(0, 100U, 0U, 0U, false, telemetry);

        return expect(engine.has_work(), "engine should have drainable exact-match work") &&
               drain_once(engine, telemetry, writer, "exact match", &traces) &&
               expect(writer.frames.size() == 1U, "exact-match drain did not write a frame") &&
               expect(writer.frames.front().timestamp == 100U, "exact-match drain wrote the wrong alpha timestamp") &&
               expect(writer.frames.front().alpha && writer.frames.front().alpha->front() == 42U,
                      "exact-match drain wrote the wrong alpha payload") &&
               expect(telemetry.aligned_frames == 1U, "exact-match drain did not count aligned frames") &&
               expect(!traces.empty() && std::string{traces.front().reason} == "selected",
                      "exact-match drain did not report selected trace");
    }

    bool test_duplicate_output_reuses_last_written_alpha()
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine = make_engine();
        alpha_recorder::obs::LivePipelineTelemetry telemetry{};
        WriteCapture writer;
        std::vector<alpha_recorder::obs::AlignmentTraceEvent> traces;

        engine.remember_alpha_frame(make_alpha_frame(100U, 7U), telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{100U, 100U, false}, telemetry);
        engine.queue_packet(0, 100U, 0U, 0U, false, telemetry);
        if (!drain_once(engine, telemetry, writer, "duplicate setup", &traces))
        {
            return false;
        }

        engine.remember_alpha_frame(make_alpha_frame(116U, 99U), telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{116U, 100U, true}, telemetry);
        engine.queue_packet(1, 116U, 0U, 0U, false, telemetry);
        if (!drain_once(engine, telemetry, writer, "duplicate output", &traces))
        {
            return false;
        }

        return expect(writer.frames.size() == 2U, "duplicate output did not write a second frame") &&
               expect(writer.frames[1].timestamp == 100U, "duplicate output did not reuse previous alpha timestamp") &&
               expect(writer.frames[1].alpha == writer.frames[0].alpha,
                      "duplicate output did not reuse previous alpha buffer") &&
               expect(std::string{traces.back().reason} == "output_duplicate",
                      "duplicate output trace reason changed");
    }

    bool test_missing_alpha_repeats_previous_frame_on_drain_all()
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine = make_engine();
        alpha_recorder::obs::LivePipelineTelemetry telemetry{};
        WriteCapture writer;
        std::vector<alpha_recorder::obs::AlignmentTraceEvent> traces;

        engine.remember_alpha_frame(make_alpha_frame(100U, 5U), telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{100U, 100U, false}, telemetry);
        engine.queue_packet(0, 100U, 0U, 0U, false, telemetry);
        if (!drain_once(engine, telemetry, writer, "missing alpha setup", &traces))
        {
            return false;
        }

        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{133U, 133U, false}, telemetry);
        engine.queue_packet(1, 133U, 0U, 0U, false, telemetry);
        if (!drain_once(engine, telemetry, writer, "missing alpha repeat", &traces, true))
        {
            return false;
        }

        return expect(writer.frames.size() == 2U, "missing alpha did not emit a repeated frame") &&
               expect(writer.frames[1].alpha == writer.frames[0].alpha,
                      "missing alpha did not repeat the previous written alpha") &&
               expect(telemetry.alignment_missing_alpha_repeats == 1U,
                      "missing alpha repeat telemetry changed") &&
               expect(std::string{traces.back().reason} == "missing_alpha",
                      "missing alpha trace reason changed");
    }

    bool test_missing_output_drain_all_repeats_last_captured_frame()
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine = make_engine();
        alpha_recorder::obs::LivePipelineTelemetry telemetry{};
        WriteCapture writer;

        engine.remember_alpha_frame(make_alpha_frame(100U, 17U), telemetry);
        engine.queue_packet(0, 100U, 0U, 0U, false, telemetry);

        return drain_once(engine, telemetry, writer, "missing output repeat", nullptr, true) &&
               expect(writer.frames.size() == 1U, "missing output did not emit a frame") &&
               expect(writer.frames.front().timestamp == 100U,
                      "missing output did not repeat the last captured alpha") &&
               expect(telemetry.alignment_missing_output_repeats == 1U,
                      "missing output repeat telemetry changed");
    }

    bool test_texture_packet_uses_successor_output_cadence()
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine = make_engine();
        alpha_recorder::obs::LivePipelineTelemetry telemetry{};
        WriteCapture writer;
        std::vector<alpha_recorder::obs::AlignmentTraceEvent> traces;

        engine.remember_alpha_frame(make_alpha_frame(1016U, 88U), telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{1000U, 1000U, false}, telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{1016U, 1016U, false}, telemetry);
        engine.queue_packet(0, 1000U, 0U, 0U, true, telemetry);

        return drain_once(engine, telemetry, writer, "texture successor", &traces) &&
               expect(writer.frames.size() == 1U, "texture successor did not write a frame") &&
               expect(writer.frames.front().timestamp == 1016U,
                      "texture successor did not choose alpha for the successor cadence") &&
               expect(!traces.empty() && traces.front().selection.output_index == 1U,
                      "texture successor selected the wrong output cadence index");
    }

    bool test_queue_limits_drop_oldest_frames()
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine = make_engine(2U, 2U);
        alpha_recorder::obs::LivePipelineTelemetry telemetry{};

        engine.remember_alpha_frame(make_alpha_frame(100U, 1U), telemetry);
        engine.remember_alpha_frame(make_alpha_frame(116U, 2U), telemetry);
        engine.remember_alpha_frame(make_alpha_frame(133U, 3U), telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{100U, 100U, false}, telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{116U, 116U, false}, telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{133U, 133U, false}, telemetry);

        return expect(engine.pending_alpha_size() == 2U, "alpha queue did not enforce its limit") &&
               expect(engine.pending_output_size() == 2U, "output queue did not enforce its limit") &&
               expect(telemetry.alignment_alpha_dropped_frames == 1U,
                      "alpha queue did not count dropped frames") &&
               expect(telemetry.alignment_output_dropped_frames == 1U,
                      "output queue did not count dropped frames");
    }

    bool test_write_failure_surfaces_as_drain_failure()
    {
        alpha_recorder::obs::AlphaAlignmentEngine engine = make_engine();
        alpha_recorder::obs::LivePipelineTelemetry telemetry{};
        WriteCapture writer;
        writer.fail_next = true;

        engine.remember_alpha_frame(make_alpha_frame(100U, 42U), telemetry);
        engine.remember_output_frame(alpha_recorder::obs::OutputFrameCadence{100U, 100U, false}, telemetry);
        engine.queue_packet(0, 100U, 0U, 0U, false, telemetry);

        const alpha_recorder::obs::AlphaAlignmentDrainResult result =
            engine.drain(false,
                         1U,
                         telemetry,
                         [&writer](const alpha_recorder::obs::AlphaFrame &frame,
                                   bool &queued,
                                   std::string &error_message) {
                             return writer.write(frame, queued, error_message);
                         },
                         {});

        return expect(result.failed, "write failure did not fail the drain") &&
               expect(result.error_message == "injected write failure", "write failure error was not preserved") &&
               expect(writer.frames.empty(), "write failure should not record a written frame");
    }
} // namespace

int main()
{
    if (!test_exact_packet_output_alpha_match_writes_frame() ||
        !test_duplicate_output_reuses_last_written_alpha() ||
        !test_missing_alpha_repeats_previous_frame_on_drain_all() ||
        !test_missing_output_drain_all_repeats_last_captured_frame() ||
        !test_texture_packet_uses_successor_output_cadence() ||
        !test_queue_limits_drop_oldest_frames() ||
        !test_write_failure_surfaces_as_drain_failure())
    {
        return 1;
    }

    std::cout << "alpha alignment engine test passed\n";
    return 0;
}
