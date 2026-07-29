#include "replay_generation_ledger.hpp"

#include <algorithm>
#include <limits>

namespace alpha_recorder::obs
{

    ReplayGenerationSelection consume_latest_replay_generation(
        std::vector<ReplayGenerationQueueEntry> &queue,
        std::int64_t target_pts,
        bool has_minimum_generation,
        std::uint64_t minimum_generation) noexcept
    {
        ReplayGenerationSelection result{};
        const auto selected = std::max_element(
            queue.begin(),
            queue.end(),
            [target_pts](const ReplayGenerationQueueEntry &left,
                         const ReplayGenerationQueueEntry &right) {
                const bool left_eligible = left.pts <= target_pts;
                const bool right_eligible = right.pts <= target_pts;
                if (left_eligible != right_eligible)
                {
                    return !left_eligible;
                }
                return left.pts < right.pts;
            });
        if (selected == queue.end() || selected->pts > target_pts)
        {
            return result;
        }

        result.pts = selected->pts;
        result.generation = selected->generation;
        result.status = selected->pts == target_pts
                            ? ReplayGenerationSelectionStatus::Exact
                            : ReplayGenerationSelectionStatus::LatestConfirmed;
        const bool conflicting_generation =
            std::any_of(queue.begin(),
                        queue.end(),
                        [selected](const ReplayGenerationQueueEntry &entry) {
                            return entry.pts == selected->pts &&
                                   entry.generation != selected->generation;
                        });
        if (conflicting_generation)
        {
            result.status = ReplayGenerationSelectionStatus::Ambiguous;
        }
        else if (has_minimum_generation && selected->generation < minimum_generation)
        {
            result.status = ReplayGenerationSelectionStatus::Regressive;
        }

        const std::size_t before = queue.size();
        queue.erase(
            std::remove_if(queue.begin(),
                           queue.end(),
                           [target_pts](const ReplayGenerationQueueEntry &entry) {
                               return entry.pts <= target_pts;
                           }),
            queue.end());
        result.removed_entries = before - queue.size();
        const bool selected_generation_is_usable =
            result.status == ReplayGenerationSelectionStatus::Exact ||
            result.status == ReplayGenerationSelectionStatus::LatestConfirmed;
        result.skipped_entries =
            result.removed_entries -
            (selected_generation_is_usable ? 1U : 0U);
        return result;
    }

    ReplayGenerationLedger::ReplayGenerationLedger(std::int64_t pts_step) noexcept
    {
        reset(pts_step);
    }

    void ReplayGenerationLedger::reset(std::int64_t pts_step,
                                       std::int64_t first_pts) noexcept
    {
        entries_.clear();
        next_pts_ = first_pts;
        pts_step_ = std::max<std::int64_t>(1, pts_step);
    }

    std::int64_t ReplayGenerationLedger::record_next(std::uint64_t generation) noexcept
    {
        const std::int64_t assigned_pts = next_pts_;
        record(assigned_pts, generation);
        if (next_pts_ <= std::numeric_limits<std::int64_t>::max() - pts_step_)
        {
            next_pts_ += pts_step_;
        }
        else
        {
            next_pts_ = std::numeric_limits<std::int64_t>::max();
        }
        return assigned_pts;
    }

    void ReplayGenerationLedger::record(std::int64_t pts,
                                        std::uint64_t generation) noexcept
    {
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [pts](const Entry &entry) {
                                            return entry.pts == pts;
                                        });
        if (found == entries_.end())
        {
            entries_.push_back(Entry{pts, generation, false});
            return;
        }

        if (found->generation != generation)
        {
            found->ambiguous = true;
        }
    }

    void ReplayGenerationLedger::mark_ambiguous(std::int64_t pts) noexcept
    {
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [pts](const Entry &entry) {
                                            return entry.pts == pts;
                                        });
        if (found == entries_.end())
        {
            entries_.push_back(Entry{pts, 0U, true});
            return;
        }
        found->ambiguous = true;
    }

    ReplayGenerationLookup ReplayGenerationLedger::lookup(std::int64_t pts) const noexcept
    {
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [pts](const Entry &entry) {
                                            return entry.pts == pts;
                                        });
        if (found == entries_.end())
        {
            return {};
        }
        if (found->ambiguous)
        {
            return ReplayGenerationLookup{ReplayGenerationStatus::Ambiguous, 0U};
        }
        return ReplayGenerationLookup{ReplayGenerationStatus::Proven, found->generation};
    }

    std::int64_t ReplayGenerationLedger::next_pts() const noexcept
    {
        return next_pts_;
    }

    std::int64_t ReplayGenerationLedger::pts_step() const noexcept
    {
        return pts_step_;
    }

    std::size_t ReplayGenerationLedger::size() const noexcept
    {
        return entries_.size();
    }

    std::size_t ReplayGenerationLedger::ambiguous_count() const noexcept
    {
        return static_cast<std::size_t>(
            std::count_if(entries_.begin(), entries_.end(),
                          [](const Entry &entry) {
                              return entry.ambiguous;
                          }));
    }

    void merge_replay_generation(const ReplayGenerationLedger &ledger,
                                 GpuTexturePacketRecord &packet) noexcept
    {
        const ReplayGenerationLookup lookup = ledger.lookup(packet.pts);
        if (lookup.status == ReplayGenerationStatus::Missing)
        {
            return;
        }
        if (lookup.status == ReplayGenerationStatus::Ambiguous ||
            (packet.has_generation && packet.emitted_generation != lookup.generation))
        {
            packet.has_generation = false;
            packet.ambiguous_generation = true;
            return;
        }

        packet.input_generation = lookup.generation;
        packet.emitted_generation = lookup.generation;
        packet.has_generation = true;
        packet.ambiguous_generation = false;
    }

} // namespace alpha_recorder::obs
