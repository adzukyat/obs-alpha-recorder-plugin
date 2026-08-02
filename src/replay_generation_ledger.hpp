#pragma once

#include "gpu_texture_timeline_ledger.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace alpha_recorder::obs
{

    enum class ReplayGenerationStatus
    {
        Missing,
        Proven,
        Ambiguous,
    };

    struct ReplayGenerationLookup
    {
        ReplayGenerationStatus status = ReplayGenerationStatus::Missing;
        std::uint64_t generation = 0U;
    };

    struct ReplayGenerationQueueEntry
    {
        std::int64_t pts = 0;
        std::uint64_t generation = 0U;
        std::uint64_t input_cts = 0U;
    };

    struct ReplayPacketPtsEpoch
    {
        std::int64_t first_packet_pts = 0;
        std::int64_t input_pts_offset = 0;
    };

    enum class ReplayGenerationSelectionStatus
    {
        Missing,
        Exact,
        LatestConfirmed,
        NextConfirmed,
        Ambiguous,
        Regressive,
    };

    struct ReplayGenerationSelection
    {
        ReplayGenerationSelectionStatus status = ReplayGenerationSelectionStatus::Missing;
        std::int64_t pts = 0;
        std::uint64_t generation = 0U;
        std::size_t removed_entries = 0U;
        std::size_t skipped_entries = 0U;
    };

    [[nodiscard]] ReplayGenerationSelection consume_latest_replay_generation(
        std::vector<ReplayGenerationQueueEntry> &queue,
        std::int64_t target_pts,
        bool has_minimum_generation = false,
        std::uint64_t minimum_generation = 0U,
        bool has_safe_pts_watermark = false,
        std::int64_t safe_pts_watermark = 0) noexcept;

    [[nodiscard]] bool choose_replay_prefix_fallback_generation(
        const std::vector<std::uint64_t> &retained_generations,
        bool repeat_missing_prefix,
        bool has_exact_generation_evidence,
        std::uint64_t &generation) noexcept;

    [[nodiscard]] std::optional<std::size_t>
    choose_replay_safe_texture_slot(
        const std::vector<std::uint64_t> &texture_generations,
        const std::vector<bool> &texture_valid,
        std::size_t start_index,
        const std::vector<ReplayGenerationQueueEntry> &pending_generations) noexcept;

    [[nodiscard]] std::int64_t replay_pause_target_adjustment_pts(
        std::int64_t dropped_input_delta_pts,
        std::int64_t pts_step) noexcept;

    [[nodiscard]] std::int64_t replay_input_pts_for_packet(
        std::int64_t packet_pts,
        const std::vector<ReplayPacketPtsEpoch> &epochs) noexcept;

    class ReplayGenerationLedger
    {
    public:
        explicit ReplayGenerationLedger(std::int64_t pts_step = 1) noexcept;

        void reset(std::int64_t pts_step = 1, std::int64_t first_pts = 0) noexcept;

        [[nodiscard]] std::int64_t record_next(std::uint64_t generation) noexcept;
        void record(std::int64_t pts, std::uint64_t generation) noexcept;
        void mark_ambiguous(std::int64_t pts) noexcept;

        [[nodiscard]] ReplayGenerationLookup lookup(std::int64_t pts) const noexcept;
        [[nodiscard]] std::int64_t next_pts() const noexcept;
        [[nodiscard]] std::int64_t pts_step() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::size_t ambiguous_count() const noexcept;

    private:
        struct Entry
        {
            std::int64_t pts = 0;
            std::uint64_t generation = 0U;
            bool ambiguous = false;
        };

        std::vector<Entry> entries_{};
        std::int64_t next_pts_ = 0;
        std::int64_t pts_step_ = 1;
    };

    void merge_replay_generation(const ReplayGenerationLedger &ledger,
                                 GpuTexturePacketRecord &packet) noexcept;
    void merge_replay_generation(const ReplayGenerationLedger &ledger,
                                 std::int64_t input_pts,
                                 GpuTexturePacketRecord &packet) noexcept;

} // namespace alpha_recorder::obs
