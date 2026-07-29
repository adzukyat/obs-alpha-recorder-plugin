#pragma once

#include "gpu_texture_timeline_ledger.hpp"

#include <cstddef>
#include <cstdint>
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
    };

    enum class ReplayGenerationSelectionStatus
    {
        Missing,
        Exact,
        LatestConfirmed,
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
        std::uint64_t minimum_generation = 0U) noexcept;

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

} // namespace alpha_recorder::obs
