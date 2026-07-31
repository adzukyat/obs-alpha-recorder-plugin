#include "replay_generation_ledger.hpp"

#include <algorithm>
#include <limits>

namespace alpha_recorder::obs
{

    bool choose_replay_prefix_fallback_generation(
        const std::vector<std::uint64_t> &retained_generations,
        bool repeat_missing_prefix,
        bool has_exact_generation_evidence,
        std::uint64_t &generation) noexcept
    {
        if (!repeat_missing_prefix || has_exact_generation_evidence ||
            retained_generations.empty())
        {
            return false;
        }

        generation =
            *std::min_element(retained_generations.begin(),
                              retained_generations.end());
        return true;
    }

    std::optional<std::size_t> choose_replay_safe_texture_slot(
        const std::vector<std::uint64_t> &texture_generations,
        const std::vector<bool> &texture_valid,
        std::size_t start_index,
        const std::vector<ReplayGenerationQueueEntry> &pending_generations) noexcept
    {
        if (texture_generations.empty() ||
            texture_generations.size() != texture_valid.size())
        {
            return std::nullopt;
        }

        for (std::size_t offset = 0U; offset < texture_generations.size(); ++offset)
        {
            const std::size_t candidate =
                (start_index + offset) % texture_generations.size();
            const bool protects_pending_generation =
                texture_valid[candidate] &&
                std::any_of(
                    pending_generations.begin(),
                    pending_generations.end(),
                    [&texture_generations, candidate](
                        const ReplayGenerationQueueEntry &entry) {
                        return entry.generation ==
                               texture_generations[candidate];
                    });
            if (!protects_pending_generation)
            {
                return candidate;
            }
        }
        return std::nullopt;
    }

    std::int64_t replay_pause_target_adjustment_pts(
        std::int64_t dropped_input_delta_pts,
        std::int64_t pts_step) noexcept
    {
        if (dropped_input_delta_pts < 0 ||
            pts_step <= 0 ||
            (dropped_input_delta_pts % pts_step) != 0)
        {
            return 0;
        }

        return pts_step - dropped_input_delta_pts;
    }

    std::int64_t replay_input_pts_for_packet(
        std::int64_t packet_pts,
        const std::vector<ReplayPacketPtsEpoch> &epochs) noexcept
    {
        std::int64_t input_pts_offset = 0;
        std::int64_t selected_epoch_pts =
            std::numeric_limits<std::int64_t>::min();
        for (const ReplayPacketPtsEpoch &epoch : epochs)
        {
            if (epoch.first_packet_pts <= packet_pts &&
                epoch.first_packet_pts >= selected_epoch_pts)
            {
                selected_epoch_pts = epoch.first_packet_pts;
                input_pts_offset = epoch.input_pts_offset;
            }
        }

        if (input_pts_offset > 0 &&
            packet_pts >
                std::numeric_limits<std::int64_t>::max() -
                    input_pts_offset)
        {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (input_pts_offset < 0 &&
            packet_pts <
                std::numeric_limits<std::int64_t>::min() -
                    input_pts_offset)
        {
            return std::numeric_limits<std::int64_t>::min();
        }
        return packet_pts + input_pts_offset;
    }

    ReplayGenerationSelection consume_latest_replay_generation(
        std::vector<ReplayGenerationQueueEntry> &queue,
        std::int64_t target_pts,
        bool has_minimum_generation,
        std::uint64_t minimum_generation,
        bool has_safe_pts_watermark,
        std::int64_t safe_pts_watermark) noexcept
    {
        ReplayGenerationSelection result{};
        auto selected = std::find_if(
            queue.begin(),
            queue.end(),
            [target_pts](const ReplayGenerationQueueEntry &entry) {
                return entry.pts == target_pts;
            });
        if (selected != queue.end())
        {
            result.status = ReplayGenerationSelectionStatus::Exact;
        }
        else if (has_safe_pts_watermark && safe_pts_watermark >= target_pts)
        {
            selected = std::min_element(
                queue.begin(),
                queue.end(),
                [target_pts, safe_pts_watermark](
                    const ReplayGenerationQueueEntry &left,
                    const ReplayGenerationQueueEntry &right) {
                    const bool left_eligible =
                        left.pts > target_pts && left.pts <= safe_pts_watermark;
                    const bool right_eligible =
                        right.pts > target_pts && right.pts <= safe_pts_watermark;
                    if (left_eligible != right_eligible)
                    {
                        return left_eligible;
                    }
                    return left.pts < right.pts;
                });
            if (selected != queue.end() &&
                selected->pts > target_pts &&
                selected->pts <= safe_pts_watermark)
            {
                result.status = ReplayGenerationSelectionStatus::NextConfirmed;
            }
            else
            {
                selected = queue.end();
            }
        }

        if (selected == queue.end())
        {
            selected = std::max_element(
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
            if (selected != queue.end() && selected->pts <= target_pts)
            {
                result.status = ReplayGenerationSelectionStatus::LatestConfirmed;
            }
        }

        if (selected == queue.end() ||
            result.status == ReplayGenerationSelectionStatus::Missing)
        {
            return result;
        }

        result.pts = selected->pts;
        result.generation = selected->generation;
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
        const std::int64_t removal_limit =
            result.status == ReplayGenerationSelectionStatus::NextConfirmed
                ? result.pts
                : target_pts;
        queue.erase(
            std::remove_if(queue.begin(),
                           queue.end(),
                           [removal_limit](const ReplayGenerationQueueEntry &entry) {
                               return entry.pts <= removal_limit;
                           }),
            queue.end());
        result.removed_entries = before - queue.size();
        const bool selected_generation_is_usable =
            result.status == ReplayGenerationSelectionStatus::Exact ||
            result.status == ReplayGenerationSelectionStatus::LatestConfirmed ||
            result.status == ReplayGenerationSelectionStatus::NextConfirmed;
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
        merge_replay_generation(ledger, packet.pts, packet);
    }

    void merge_replay_generation(const ReplayGenerationLedger &ledger,
                                 std::int64_t input_pts,
                                 GpuTexturePacketRecord &packet) noexcept
    {
        const ReplayGenerationLookup lookup = ledger.lookup(input_pts);
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
