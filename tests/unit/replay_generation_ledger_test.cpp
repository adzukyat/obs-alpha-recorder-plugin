#include "replay_generation_ledger.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace
{
    using alpha_recorder::obs::GpuTexturePacketRecord;
    using alpha_recorder::obs::ReplayGenerationLedger;
    using alpha_recorder::obs::ReplayGenerationQueueEntry;
    using alpha_recorder::obs::ReplayGenerationSelectionStatus;
    using alpha_recorder::obs::ReplayGenerationStatus;

    int failures = 0;

    void expect(bool condition, const char *message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    void expect_generation(const ReplayGenerationLedger &ledger,
                           std::int64_t pts,
                           std::uint64_t generation,
                           const char *message)
    {
        const auto lookup = ledger.lookup(pts);
        expect(lookup.status == ReplayGenerationStatus::Proven &&
                   lookup.generation == generation,
               message);
    }

    void test_dense_pts_domains()
    {
        for (const std::int64_t step : std::array<std::int64_t, 3>{1, 1001, 1001})
        {
            ReplayGenerationLedger ledger(step);
            for (std::uint64_t generation = 40U; generation < 100U; ++generation)
            {
                const std::int64_t expected_pts =
                    static_cast<std::int64_t>(generation - 40U) * step;
                expect(ledger.record_next(generation) == expected_pts,
                       "record_next must allocate a dense encoder PTS slot");
                expect_generation(ledger, expected_pts, generation,
                                  "recorded generation must be recoverable by encoder PTS");
            }
            expect(ledger.size() == 60U, "60/1 and fractional-rate ledgers must contain every slot");
            expect(ledger.next_pts() == 60 * step, "next PTS must remain in the encoder timebase");
        }
    }

    void test_nonzero_start_and_main_pts_independence()
    {
        ReplayGenerationLedger ledger;
        ledger.reset(1001, 7007);
        const std::array<std::int64_t, 6> main_pts{9000, 9000, 12000, 18000, 19000, 42000};
        for (std::size_t index = 0U; index < main_pts.size(); ++index)
        {
            (void)main_pts[index];
            const std::int64_t alpha_pts = ledger.record_next(100U + index);
            expect(alpha_pts == 7007 + static_cast<std::int64_t>(index) * 1001,
                   "alpha PTS must not inherit main PTS gaps, duplicates, or nonzero origin");
        }
    }

    void test_packet_callbacks_before_and_after_generation()
    {
        ReplayGenerationLedger ledger(1);
        GpuTexturePacketRecord packet_before{};
        packet_before.pts = 0;
        alpha_recorder::obs::merge_replay_generation(ledger, packet_before);
        expect(!packet_before.has_generation, "packet callback may arrive before generation evidence");

        expect(ledger.record_next(77U) == 0, "first generated PTS must be zero");
        alpha_recorder::obs::merge_replay_generation(ledger, packet_before);
        expect(packet_before.has_generation && packet_before.emitted_generation == 77U,
               "late generation evidence must backfill an existing packet");

        expect(ledger.record_next(78U) == 1, "second generated PTS must be one");
        GpuTexturePacketRecord packet_after{};
        packet_after.pts = 1;
        alpha_recorder::obs::merge_replay_generation(ledger, packet_after);
        expect(packet_after.has_generation && packet_after.emitted_generation == 78U,
               "generation evidence must merge when it arrives before the packet callback");
    }

    void test_b_frame_callback_order()
    {
        ReplayGenerationLedger ledger(1);
        for (std::uint64_t generation = 200U; generation < 208U; ++generation)
        {
            (void)ledger.record_next(generation);
        }

        std::array<std::int64_t, 8> callback_pts{0, 3, 1, 2, 6, 4, 5, 7};
        for (const std::int64_t pts : callback_pts)
        {
            GpuTexturePacketRecord packet{};
            packet.pts = pts;
            alpha_recorder::obs::merge_replay_generation(ledger, packet);
            expect(packet.has_generation &&
                       packet.emitted_generation == 200U + static_cast<std::uint64_t>(pts),
                   "packet delivery/DTS order must not affect PTS generation lookup");
        }
    }

    void test_repeat_and_conflict()
    {
        ReplayGenerationLedger ledger(1);
        expect(ledger.record_next(10U) == 0, "repeat test slot zero");
        expect(ledger.record_next(10U) == 1, "repeat test slot one");
        expect_generation(ledger, 0, 10U, "tail repeat keeps generation on first slot");
        expect_generation(ledger, 1, 10U, "tail repeat keeps generation on later slot");

        ledger.record(1, 11U);
        expect(ledger.lookup(1).status == ReplayGenerationStatus::Ambiguous,
               "conflicting generation evidence must be ambiguous");
        GpuTexturePacketRecord packet{};
        packet.pts = 1;
        alpha_recorder::obs::merge_replay_generation(ledger, packet);
        expect(!packet.has_generation && packet.ambiguous_generation,
               "ambiguous evidence must fail closed at packet merge");
    }

    void test_pause_target_adjustment_reconciles_texture_phase()
    {
        expect(alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   0,
                   1) == 1,
               "no dropped input must advance the replay target by the one-slot texture phase");
        expect(alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   1,
                   1) == 0,
               "one dropped input must cancel the one-slot texture phase");
        expect(alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   2,
                   1) == -1,
               "two dropped inputs must move the replay target back by one slot");
        expect(alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   1001,
                   1001) == 0,
               "one 59.94 fps dropped input must cancel its one-slot texture phase");
        expect(alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   2002,
                   1001) == -1001,
               "two 59.94 fps dropped inputs must move the replay target back by one slot");
        expect(alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   1,
                   0) == 0,
               "an invalid encoder PTS step must disable pause adjustment");
        expect(alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   1,
                   1001) == 0,
               "a correction outside the encoder PTS grid must disable pause adjustment");
        const std::int64_t first_pause_adjustment =
            alpha_recorder::obs::replay_pause_target_adjustment_pts(
                2, 1);
        const std::int64_t second_pause_adjustment =
            alpha_recorder::obs::replay_pause_target_adjustment_pts(
                2, 1);
        expect(first_pause_adjustment + second_pause_adjustment == -2,
               "successive pauses must accumulate only each pause's newly dropped inputs");

        expect(-alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   0, 1) == -1,
               "a zero-drop resume must map packet proof one generation slot back");
        expect(-alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   1, 1) == 0,
               "a one-drop resume must preserve the existing packet-generation mapping");
        expect(-alpha_recorder::obs::replay_pause_target_adjustment_pts(
                   2, 1) == 1,
               "a two-drop resume must map packet proof one generation slot forward");
    }

    void test_pause_packet_pts_epochs_map_only_accepted_inputs()
    {
        using alpha_recorder::obs::ReplayPacketPtsEpoch;

        const std::vector<ReplayPacketPtsEpoch> epochs{
            {46, 1},
            {120, 2},
        };
        expect(alpha_recorder::obs::replay_input_pts_for_packet(
                   45, epochs) == 45,
               "packets before a pause epoch must retain their original input PTS");
        expect(alpha_recorder::obs::replay_input_pts_for_packet(
                   46, epochs) == 47,
               "the first resumed packet must map by the uncompensated generation slot");
        expect(alpha_recorder::obs::replay_input_pts_for_packet(
                   119, epochs) == 120,
               "a generation PTS epoch must remain active until the next pause");
        expect(alpha_recorder::obs::replay_input_pts_for_packet(
                   120, epochs) == 122,
               "a later pause epoch must replace the cumulative generation offset");

        alpha_recorder::obs::ReplayGenerationLedger ledger{};
        ledger.record(47, 530U);
        alpha_recorder::obs::GpuTexturePacketRecord packet{};
        packet.pts = 46;
        alpha_recorder::obs::merge_replay_generation(
            ledger,
            alpha_recorder::obs::replay_input_pts_for_packet(
                packet.pts, epochs),
            packet);
        expect(packet.has_generation &&
                   packet.emitted_generation == 530U,
               "packet proof must use the generation PTS after source-side pause compensation");
    }

    void test_split_prefix_fallback_is_bounded_to_startup()
    {
        std::uint64_t generation = 999U;
        expect(!alpha_recorder::obs::choose_replay_prefix_fallback_generation(
                   {8U, 6U, 7U}, false, false, generation),
               "normal segments must not synthesize missing prefix evidence");
        expect(!alpha_recorder::obs::choose_replay_prefix_fallback_generation(
                   {}, true, false, generation),
               "a split prefix cannot be repeated before any texture exists");
        expect(alpha_recorder::obs::choose_replay_prefix_fallback_generation(
                   {8U, 6U, 7U}, true, false, generation) &&
                   generation == 6U,
               "a split segment must repeat its oldest retained texture for an unavailable prefix");
        expect(!alpha_recorder::obs::choose_replay_prefix_fallback_generation(
                   {8U, 6U, 7U}, true, true, generation),
               "fallback must stop permanently after exact generation evidence begins");
    }

    void test_replay_write_slot_preserves_pending_generations()
    {
        const std::vector<std::uint64_t> generations{
            100U, 101U, 102U, 103U};
        const std::vector<bool> valid{true, true, true, false};
        const std::vector<ReplayGenerationQueueEntry> pending{
            {0, 100U},
            {1, 101U},
        };

        const auto selected =
            alpha_recorder::obs::choose_replay_safe_texture_slot(
                generations, valid, 0U, pending);
        expect(selected.has_value() && *selected == 2U,
               "resume capture must skip ring slots needed by pending replay packets");

        const auto wrapped =
            alpha_recorder::obs::choose_replay_safe_texture_slot(
                generations, valid, 3U, pending);
        expect(wrapped.has_value() && *wrapped == 3U,
               "an unused ring slot must remain writable before wraparound");

        const auto all_protected =
            alpha_recorder::obs::choose_replay_safe_texture_slot(
                generations,
                std::vector<bool>{true, true, true, true},
                1U,
                std::vector<ReplayGenerationQueueEntry>{
                    {0, 100U},
                    {1, 101U},
                    {2, 102U},
                    {3, 103U},
                });
        expect(!all_protected.has_value(),
               "a full protected ring must drop current retention instead of overwriting replay proof");

        const auto invalid_shape =
            alpha_recorder::obs::choose_replay_safe_texture_slot(
                generations, std::vector<bool>{true}, 0U, pending);
        expect(!invalid_shape.has_value(),
               "mismatched ring metadata must fail safely");
    }

    void test_replay_queue_catches_up_without_replaying_backlog()
    {
        std::vector<ReplayGenerationQueueEntry> reordered_queue{
            {0, 100U},
            {3, 103U},
            {1, 101U},
            {2, 102U},
        };
        for (std::int64_t pts = 0; pts < 4; ++pts)
        {
            const auto presentation =
                alpha_recorder::obs::consume_latest_replay_generation(
                    reordered_queue, pts);
            expect(presentation.status ==
                       ReplayGenerationSelectionStatus::Exact,
                   "reordered encoder callbacks must be consumed by presentation PTS");
            expect(presentation.generation ==
                       100U + static_cast<std::uint64_t>(pts),
                   "callback/DTS order must not become alpha content order");
        }
        expect(reordered_queue.empty(),
               "all reordered presentation entries must be consumed once");

        std::vector<ReplayGenerationQueueEntry> queue{
            {10, 100U},
            {12, 102U},
            {11, 101U},
        };
        const auto exact =
            alpha_recorder::obs::consume_latest_replay_generation(queue, 12);
        expect(exact.status == ReplayGenerationSelectionStatus::Exact,
               "an available target PTS must be selected exactly");
        expect(exact.generation == 102U,
               "catch-up must select the newest generation for the current slot");
        expect(exact.removed_entries == 3U && exact.skipped_entries == 2U,
               "catch-up must discard obsolete backlog entries in one operation");
        expect(queue.empty(), "all entries through the current slot must be consumed");

        queue = {{20, 200U}, {21, 201U}, {24, 204U}};
        const auto latest =
            alpha_recorder::obs::consume_latest_replay_generation(queue, 23);
        expect(latest.status == ReplayGenerationSelectionStatus::LatestConfirmed,
               "a missing exact PTS must use the latest already-proven generation");
        expect(latest.pts == 21 && latest.generation == 201U,
               "latest-confirmed catch-up must not replay the oldest queue entry");
        expect(latest.removed_entries == 2U && latest.skipped_entries == 1U,
               "all older proof entries must be discarded after catch-up");
        expect(queue.size() == 1U && queue.front().pts == 24,
               "future proof must remain queued");

        const auto future_only =
            alpha_recorder::obs::consume_latest_replay_generation(queue, 23);
        expect(future_only.status == ReplayGenerationSelectionStatus::Missing &&
                   future_only.removed_entries == 0U &&
                   queue.size() == 1U,
               "future-only proof must not be consumed early");

        const auto confirmed_gap =
            alpha_recorder::obs::consume_latest_replay_generation(
                queue, 23, false, 0U, true, 24);
        expect(confirmed_gap.status == ReplayGenerationSelectionStatus::NextConfirmed,
               "a presentation gap below the safe watermark must use the next written packet");
        expect(confirmed_gap.pts == 24 && confirmed_gap.generation == 204U,
               "gap compression must preserve the next written packet generation");
        expect(queue.empty(),
               "the confirmed future packet must be consumed exactly once");

        queue = {{21, 201U}, {24, 204U}};
        const auto confirmed_gap_with_stale =
            alpha_recorder::obs::consume_latest_replay_generation(
                queue, 23, false, 0U, true, 24);
        expect(confirmed_gap_with_stale.status ==
                   ReplayGenerationSelectionStatus::NextConfirmed,
               "a safe future packet must win over stale proof for a permanent PTS gap");
        expect(confirmed_gap_with_stale.pts == 24 &&
                   confirmed_gap_with_stale.generation == 204U,
               "gap compression must advance to the next written main packet");
        expect(confirmed_gap_with_stale.removed_entries == 2U &&
                   confirmed_gap_with_stale.skipped_entries == 1U &&
                   queue.empty(),
               "gap compression must discard stale proof with the selected future packet");

        queue = {{40, 400U}, {40, 401U}, {41, 402U}};
        const auto ambiguous =
            alpha_recorder::obs::consume_latest_replay_generation(queue, 40);
        expect(ambiguous.status == ReplayGenerationSelectionStatus::Ambiguous,
               "conflicting generations for one PTS must not be selected arbitrarily");
        expect(ambiguous.removed_entries == 2U &&
                   ambiguous.skipped_entries == 2U,
               "all ambiguous evidence through the target PTS must be discarded");
        expect(queue.size() == 1U && queue.front().pts == 41,
               "future evidence must survive an ambiguous current slot");

        ReplayGenerationLedger ambiguous_ledger(1);
        const auto ambiguous_pts = ambiguous_ledger.record_next(400U);
        ambiguous_ledger.mark_ambiguous(ambiguous_pts);
        expect(ambiguous_ledger.lookup(ambiguous_pts).status ==
                   ReplayGenerationStatus::Ambiguous,
               "an emitted fallback slot must retain ambiguous input evidence");
    }

    void test_replay_queue_never_regresses_generation()
    {
        std::vector<ReplayGenerationQueueEntry> queue{
            {30, 300U},
            {31, 299U},
        };
        const auto selection =
            alpha_recorder::obs::consume_latest_replay_generation(
                queue, 31, true, 300U);
        expect(selection.status == ReplayGenerationSelectionStatus::Regressive,
               "late reordered proof must not move emitted content backwards");
        expect(selection.removed_entries == 2U && selection.skipped_entries == 2U,
               "regressive stale proof must be discarded completely");
        expect(queue.empty(), "regressive backlog must not remain to poison later slots");
    }

    void property_test_replay_queue_latency_recovery()
    {
        std::vector<ReplayGenerationQueueEntry> queue{};
        std::uint64_t last_generation = 0U;
        bool has_last_generation = false;
        for (std::int64_t target_pts = 0; target_pts < 240; ++target_pts)
        {
            const std::int64_t latency =
                target_pts >= 60 && target_pts < 120 ? 24 : 4;
            const std::int64_t arriving_pts = target_pts - latency;
            if (arriving_pts >= 0)
            {
                queue.push_back(
                    ReplayGenerationQueueEntry{arriving_pts,
                                               1000U + static_cast<std::uint64_t>(arriving_pts)});
            }

            const auto selection =
                alpha_recorder::obs::consume_latest_replay_generation(
                    queue, target_pts, has_last_generation, last_generation);
            if (selection.status == ReplayGenerationSelectionStatus::Exact ||
                selection.status == ReplayGenerationSelectionStatus::LatestConfirmed)
            {
                expect(!has_last_generation || selection.generation >= last_generation,
                       "latency recovery must keep generations monotonic");
                last_generation = selection.generation;
                has_last_generation = true;
            }
            expect(queue.size() <= 1U,
                   "delayed callbacks must not accumulate a replay backlog");
        }
        expect(has_last_generation && last_generation >= 1230U,
               "the queue must recover close to the live generation after latency drops");
    }

    void property_test_callback_permutations()
    {
        std::mt19937 generator(0xA17A5EEDU);
        for (const std::int64_t step : std::array<std::int64_t, 3>{1, 1001, 1001})
        {
            ReplayGenerationLedger ledger(step);
            std::vector<std::int64_t> pts;
            for (std::uint64_t generation = 0U; generation < 240U; ++generation)
            {
                pts.push_back(ledger.record_next(1000U + generation));
            }
            std::shuffle(pts.begin(), pts.end(), generator);
            for (const std::int64_t packet_pts : pts)
            {
                GpuTexturePacketRecord packet{};
                packet.pts = packet_pts;
                alpha_recorder::obs::merge_replay_generation(ledger, packet);
                expect(packet.has_generation &&
                           packet.emitted_generation ==
                               1000U + static_cast<std::uint64_t>(packet_pts / step),
                       "random callback permutation must preserve PTS-to-generation identity");
            }
        }
    }
}

int main()
{
    test_dense_pts_domains();
    test_nonzero_start_and_main_pts_independence();
    test_packet_callbacks_before_and_after_generation();
    test_b_frame_callback_order();
    test_repeat_and_conflict();
    test_pause_target_adjustment_reconciles_texture_phase();
    test_pause_packet_pts_epochs_map_only_accepted_inputs();
    test_split_prefix_fallback_is_bounded_to_startup();
    test_replay_write_slot_preserves_pending_generations();
    test_replay_queue_catches_up_without_replaying_backlog();
    test_replay_queue_never_regresses_generation();
    property_test_replay_queue_latency_recovery();
    property_test_callback_permutations();

    if (failures != 0)
    {
        std::cerr << failures << " replay generation ledger checks failed\n";
        return 1;
    }
    std::cout << "Replay generation ledger checks passed\n";
    return 0;
}
