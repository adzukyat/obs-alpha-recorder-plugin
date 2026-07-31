#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <vector>

#include "recording_session_controller_cadence.hpp"

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

    bool test_repeated_raw_video_frame_is_marked_duplicate()
    {
        alpha_recorder::obs::RawVideoCadenceTracker tracker;
        const std::uint8_t first_frame[] = {1U};
        const std::uint8_t second_frame[] = {2U};

        const auto first = tracker.remember(first_frame, 100U);
        const auto repeated = tracker.remember(first_frame, 133U);
        const auto next = tracker.remember(second_frame, 166U);

        return expect(!first.duplicate_previous, "first raw output frame must not be marked duplicate") &&
               expect(first.timestamp == 100U, "first raw output timestamp changed") &&
               expect(first.content_timestamp == 100U, "first raw output content timestamp changed") &&
               expect(repeated.duplicate_previous, "repeated raw output buffer must be marked duplicate") &&
               expect(repeated.timestamp == 133U, "duplicate raw output timestamp changed") &&
               expect(repeated.content_timestamp == 100U, "duplicate raw output lost its content timestamp") &&
               expect(!next.duplicate_previous, "new raw output buffer must not be marked duplicate") &&
               expect(next.content_timestamp == 166U, "new raw output content timestamp did not reset");
    }

    bool test_duplicate_output_frame_reuses_last_written_alpha()
    {
        const auto previous_alpha = std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{7U, 8U, 9U});
        const auto newer_pending_alpha =
            std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{100U, 101U, 102U});

        const alpha_recorder::obs::AlphaFrame last_written{100U, previous_alpha};
        const alpha_recorder::obs::AlphaFrame newer_pending{133U, newer_pending_alpha};
        const alpha_recorder::obs::OutputFrameCadence repeated_output{133U, 100U, true};
        alpha_recorder::obs::AlphaFrame resolved{};

        if (!expect(alpha_recorder::obs::duplicate_output_uses_previous_alpha(repeated_output, last_written, resolved),
                    "duplicate output frame should resolve from the last written alpha"))
        {
            return false;
        }

        return expect(resolved.timestamp == last_written.timestamp,
                      "duplicate output frame drifted to a newer alpha timestamp") &&
               expect(resolved.alpha == last_written.alpha, "duplicate output frame did not reuse previous alpha bytes") &&
               expect(resolved.alpha != newer_pending.alpha,
                      "duplicate output frame must not consume the newer pending alpha frame");
    }

    bool test_non_duplicate_output_does_not_force_previous_alpha()
    {
        const auto previous_alpha = std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{7U});
        const alpha_recorder::obs::AlphaFrame last_written{100U, previous_alpha};
        const alpha_recorder::obs::OutputFrameCadence fresh_output{133U, 133U, false};
        alpha_recorder::obs::AlphaFrame resolved{};

        return expect(!alpha_recorder::obs::duplicate_output_uses_previous_alpha(fresh_output, last_written, resolved),
                      "fresh output frame should not be forced to the previous alpha frame") &&
               expect(resolved.empty(), "fresh output frame unexpectedly resolved alpha");
    }

    bool test_exact_timestamp_match_consumes_admitted_alpha()
    {
        std::deque<alpha_recorder::obs::AlphaFrame> pending{
            make_alpha_frame(90U, 1U),
            make_alpha_frame(113U, 2U),
            make_alpha_frame(130U, 3U),
        };

        const alpha_recorder::obs::TimestampFrameSelection selection =
            alpha_recorder::obs::select_frame_by_timestamp(pending, 113U, false);

        if (!expect(selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::Selected,
                    "fresh output frame should select the matching pending alpha frame") ||
            !expect(selection.selected_index == 1U, "fresh output frame did not choose the exact alpha timestamp"))
        {
            return false;
        }

        const alpha_recorder::obs::AlphaFrame selected = pending[selection.selected_index];
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(selection.selected_index + 1U));

        return expect(selected.timestamp == 113U, "selected alpha timestamp changed") &&
               expect(selected.alpha && selected.alpha->front() == 2U, "selected alpha payload changed") &&
               expect(pending.size() == 1U && pending.front().timestamp == 130U,
                      "fresh output frame should consume the selected alpha and older pending frames");
    }

    bool test_packet_cts_skips_unadmitted_startup_cadence()
    {
        const std::deque<alpha_recorder::obs::OutputFrameCadence> pending{
            alpha_recorder::obs::OutputFrameCadence{1000U, 1000U, false},
            alpha_recorder::obs::OutputFrameCadence{1016U, 1016U, false},
            alpha_recorder::obs::OutputFrameCadence{1033U, 1033U, false},
            alpha_recorder::obs::OutputFrameCadence{1050U, 1050U, false},
        };

        const alpha_recorder::obs::TimestampFrameSelection selection =
            alpha_recorder::obs::select_frame_by_timestamp(pending, 1033U, false);

        return expect(selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::Selected,
                      "packet CTS should select the admitted raw-video cadence frame") &&
               expect(selection.selected_index == 2U,
                      "packet CTS should skip raw-video frames that were not admitted to the RGB recording");
    }

    bool test_texture_packet_cts_selects_observed_successor_cadence()
    {
        const std::deque<alpha_recorder::obs::OutputFrameCadence> pending{
            alpha_recorder::obs::OutputFrameCadence{1000U, 1000U, false},
            alpha_recorder::obs::OutputFrameCadence{1016U, 1016U, false},
            alpha_recorder::obs::OutputFrameCadence{1033U, 1033U, false},
        };

        const alpha_recorder::obs::TimestampFrameSelection selection =
            alpha_recorder::obs::select_frame_after_timestamp(pending, 1000U, false);

        return expect(selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::Selected,
                      "texture packet CTS should resolve to an observed successor cadence frame") &&
               expect(selection.selected_index == 1U,
                      "texture packet CTS should not use the stale CTS-labelled cadence frame as content");
    }

    bool test_texture_successor_preserves_duplicate_content_origin()
    {
        const std::deque<alpha_recorder::obs::OutputFrameCadence> pending{
            alpha_recorder::obs::OutputFrameCadence{1000U, 1000U, false},
            alpha_recorder::obs::OutputFrameCadence{1016U, 1000U, true},
            alpha_recorder::obs::OutputFrameCadence{1033U, 1033U, false},
        };

        const alpha_recorder::obs::TimestampFrameSelection selection =
            alpha_recorder::obs::select_frame_after_timestamp(pending, 1000U, false);

        return expect(selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::Selected,
                      "texture successor should resolve even when the successor is a duplicate") &&
               expect(selection.selected_index == 1U, "texture successor selected the wrong raw cadence frame") &&
               expect(pending[selection.selected_index].content_timestamp == 1000U,
                      "texture successor should keep duplicate content tied to its origin timestamp");
    }

    bool test_texture_successor_waits_for_future_cadence()
    {
        std::deque<alpha_recorder::obs::OutputFrameCadence> pending{
            alpha_recorder::obs::OutputFrameCadence{1000U, 1000U, false},
        };

        const alpha_recorder::obs::TimestampFrameSelection waiting =
            alpha_recorder::obs::select_frame_after_timestamp(pending, 1000U, false);
        pending.push_back(alpha_recorder::obs::OutputFrameCadence{1016U, 1016U, false});
        const alpha_recorder::obs::TimestampFrameSelection selected =
            alpha_recorder::obs::select_frame_after_timestamp(pending, 1000U, false);

        return expect(waiting.status == alpha_recorder::obs::TimestampFrameSelectionStatus::WaitingForMoreFrames,
                      "texture successor selector should wait until a future raw cadence frame exists") &&
               expect(selected.status == alpha_recorder::obs::TimestampFrameSelectionStatus::Selected,
                      "texture successor selector should resolve once future cadence exists") &&
               expect(selected.selected_index == 1U, "texture successor selector chose the wrong future cadence frame");
    }

    bool test_duplicate_startup_cadence_preserves_content_timestamp()
    {
        alpha_recorder::obs::RawVideoCadenceTracker tracker;
        const std::uint8_t first_frame[] = {1U};
        const std::uint8_t next_frame[] = {2U};

        (void)tracker.remember(first_frame, 1000U);
        const alpha_recorder::obs::OutputFrameCadence admitted_duplicate = tracker.remember(first_frame, 1016U);
        const alpha_recorder::obs::OutputFrameCadence admitted_fresh = tracker.remember(next_frame, 1033U);

        return expect(admitted_duplicate.duplicate_previous, "startup duplicate should be marked duplicate") &&
               expect(admitted_duplicate.timestamp == 1016U, "startup duplicate admitted timestamp changed") &&
               expect(admitted_duplicate.content_timestamp == 1000U,
                      "startup duplicate should keep the raw content origin timestamp") &&
               expect(!admitted_fresh.duplicate_previous, "fresh frame after startup duplicate should not be duplicate") &&
               expect(admitted_fresh.content_timestamp == 1033U,
                      "fresh frame after startup duplicate should reset content origin timestamp");
    }

    bool test_selector_waits_for_future_runtime_evidence()
    {
        std::deque<alpha_recorder::obs::AlphaFrame> pending{
            make_alpha_frame(100U, 1U),
            make_alpha_frame(116U, 2U),
        };

        const alpha_recorder::obs::TimestampFrameSelection waiting =
            alpha_recorder::obs::select_frame_by_timestamp(pending, 133U, false);
        pending.push_back(make_alpha_frame(133U, 3U));
        const alpha_recorder::obs::TimestampFrameSelection selected =
            alpha_recorder::obs::select_frame_by_timestamp(pending, 133U, false);

        return expect(waiting.status == alpha_recorder::obs::TimestampFrameSelectionStatus::WaitingForMoreFrames,
                      "selector should wait when the latest frame is still before the target timestamp") &&
               expect(selected.status == alpha_recorder::obs::TimestampFrameSelectionStatus::Selected,
                      "selector should resolve once exact timestamp evidence exists") &&
               expect(selected.selected_index == 2U, "selector did not choose the exact timestamp frame");
    }

    bool test_selector_rejects_missing_exact_runtime_evidence()
    {
        const std::deque<alpha_recorder::obs::AlphaFrame> pending{
            make_alpha_frame(100U, 1U),
            make_alpha_frame(116U, 2U),
            make_alpha_frame(134U, 3U),
        };

        const alpha_recorder::obs::TimestampFrameSelection selection =
            alpha_recorder::obs::select_frame_by_timestamp(pending, 133U, true);

        return expect(selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::NoPlausibleFrame,
                      "selector should reject missing exact runtime timestamp evidence");
    }

    bool test_main_packet_admission_reconciles_unwritten_stop_suffix()
    {
        alpha_recorder::obs::MainPacketAdmissionLedger ledger;
        std::vector<std::int64_t> live_admitted_pts{};
        for (std::int64_t pts = 0; pts < 18; ++pts)
        {
            const std::optional<alpha_recorder::obs::MainPacketTiming> admitted =
                ledger.remember(
                    alpha_recorder::obs::MainPacketTiming{
                        pts, static_cast<std::uint64_t>(1000 + pts)},
                    16U);
            if (admitted)
            {
                live_admitted_pts.push_back(admitted->pts);
            }
        }

        if (!expect(live_admitted_pts == std::vector<std::int64_t>{0, 1},
                    "admission hold must release only callbacks older than its window"))
        {
            return false;
        }

        std::vector<alpha_recorder::obs::MainPacketTiming> admitted_tail{};
        const alpha_recorder::obs::MainPacketAdmissionReconcileResult result =
            ledger.reconcile(16U, admitted_tail);
        if (!expect(result.ok, "written packet count should reconcile inside the admission window") ||
            !expect(result.callback_packet_count == 18U &&
                        result.written_packet_count == 16U &&
                        result.already_admitted_packet_count == 2U,
                    "reconcile counts changed") ||
            !expect(result.admitted_tail_packet_count == 14U &&
                        result.removed_unwritten_suffix_packets == 2U,
                    "reconcile must admit the written tail and discard two unwritten callbacks") ||
            !expect(admitted_tail.size() == 14U,
                    "reconcile returned the wrong admitted tail size"))
        {
            return false;
        }

        for (std::size_t index = 0U; index < admitted_tail.size(); ++index)
        {
            if (!expect(admitted_tail[index].pts ==
                            static_cast<std::int64_t>(index + 2U),
                        "reconciled packet timing order changed"))
            {
                return false;
            }
        }

        return expect(ledger.pending_packet_count() == 0U &&
                          ledger.admitted_packet_count() == 16U,
                      "reconciled admission ledger did not reach the written packet count");
    }

    bool test_main_packet_admission_rejects_irreconcilable_counts()
    {
        alpha_recorder::obs::MainPacketAdmissionLedger ledger;
        for (std::int64_t pts = 0; pts < 4; ++pts)
        {
            (void)ledger.remember(
                alpha_recorder::obs::MainPacketTiming{
                    pts, static_cast<std::uint64_t>(1000 + pts)},
                2U);
        }

        std::vector<alpha_recorder::obs::MainPacketTiming> admitted_tail{};
        const auto fewer_written = ledger.reconcile(1U, admitted_tail);
        if (!expect(!fewer_written.ok,
                    "written count below already-admitted callbacks must fail"))
        {
            return false;
        }

        const auto extra_written = ledger.reconcile(5U, admitted_tail);
        return expect(!extra_written.ok,
                      "written count above observed callbacks must fail");
    }
} // namespace

int main()
{
    if (!test_repeated_raw_video_frame_is_marked_duplicate() ||
        !test_duplicate_output_frame_reuses_last_written_alpha() ||
        !test_non_duplicate_output_does_not_force_previous_alpha() ||
        !test_exact_timestamp_match_consumes_admitted_alpha() ||
        !test_packet_cts_skips_unadmitted_startup_cadence() ||
        !test_texture_packet_cts_selects_observed_successor_cadence() ||
        !test_texture_successor_preserves_duplicate_content_origin() ||
        !test_texture_successor_waits_for_future_cadence() ||
        !test_duplicate_startup_cadence_preserves_content_timestamp() ||
        !test_selector_waits_for_future_runtime_evidence() ||
        !test_selector_rejects_missing_exact_runtime_evidence() ||
        !test_main_packet_admission_reconciles_unwritten_stop_suffix() ||
        !test_main_packet_admission_rejects_irreconcilable_counts())
    {
        return 1;
    }

    std::cout << "recording session cadence test passed\n";
    return 0;
}
