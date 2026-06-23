#include "gpu_texture_timeline_ledger.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <set>

namespace alpha_recorder::obs
{
    namespace
    {
        [[nodiscard]] std::int64_t abs_i64(std::int64_t value) noexcept
        {
            return value >= 0 ? value : -(value + 1) + 1;
        }

        [[nodiscard]] std::int64_t gcd_positive(std::int64_t lhs, std::int64_t rhs) noexcept
        {
            lhs = abs_i64(lhs);
            rhs = abs_i64(rhs);
            if (lhs == 0)
            {
                return rhs;
            }
            if (rhs == 0)
            {
                return lhs;
            }
            return std::gcd(lhs, rhs);
        }

        [[nodiscard]] std::vector<GpuTexturePacketRecord> sorted_by_pts(
            const std::vector<GpuTexturePacketRecord> &packets)
        {
            std::vector<GpuTexturePacketRecord> sorted = packets;
            std::sort(sorted.begin(), sorted.end(),
                      [](const GpuTexturePacketRecord &lhs, const GpuTexturePacketRecord &rhs) {
                          if (lhs.pts != rhs.pts)
                          {
                              return lhs.pts < rhs.pts;
                          }
                          return lhs.dts < rhs.dts;
                      });
            return sorted;
        }

        [[nodiscard]] std::int64_t packet_pts_step(
            const std::vector<GpuTexturePacketRecord> &pts_sorted) noexcept
        {
            std::int64_t step = 0;
            for (std::size_t index = 1U; index < pts_sorted.size(); ++index)
            {
                const std::int64_t delta = pts_sorted[index].pts - pts_sorted[index - 1U].pts;
                if (delta > 0)
                {
                    step = gcd_positive(step, delta);
                }
            }
            return step;
        }

        [[nodiscard]] bool has_consistent_timebase(
            const std::vector<GpuTexturePacketRecord> &packets) noexcept
        {
            if (packets.empty())
            {
                return false;
            }

            const std::int32_t num = packets.front().timebase_num;
            const std::int32_t den = packets.front().timebase_den;
            if (num <= 0 || den <= 0)
            {
                return false;
            }

            return std::all_of(packets.begin(), packets.end(),
                               [num, den](const GpuTexturePacketRecord &packet) {
                                   return packet.timebase_num == num && packet.timebase_den == den;
                               });
        }

        [[nodiscard]] bool pts_range_present(const std::set<std::int64_t> &pts_values,
                                             std::int64_t first,
                                             std::int64_t step,
                                             std::uint64_t count) noexcept
        {
            if (step <= 0)
            {
                return false;
            }

            for (std::uint64_t index = 0U; index < count; ++index)
            {
                if (index > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / step))
                {
                    return false;
                }
                const std::int64_t pts = first + static_cast<std::int64_t>(index) * step;
                if (pts_values.find(pts) == pts_values.end())
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] const ProgramRenderRecord *find_render_by_cts(
            const std::vector<ProgramRenderRecord> &records,
            std::uint64_t cts) noexcept
        {
            const ProgramRenderRecord *match = nullptr;
            for (const ProgramRenderRecord &record : records)
            {
                if (record.render_time_ns != cts)
                {
                    continue;
                }
                if (match != nullptr)
                {
                    return nullptr;
                }
                match = &record;
            }
            return match;
        }

        [[nodiscard]] std::uint64_t main_content_generation(
            const ProgramRenderRecord &render,
            MainContentPhase phase,
            bool &valid) noexcept
        {
            valid = true;
            if (phase == MainContentPhase::LiveProgramGeneration)
            {
                return render.generation;
            }
            if (render.generation == 0U)
            {
                valid = false;
                return 0U;
            }
            return render.generation - 1U;
        }

        [[nodiscard]] std::uint64_t render_index_for_generation(
            const std::vector<ProgramRenderRecord> &records,
            std::uint64_t generation,
            bool &found) noexcept
        {
            found = false;
            for (std::size_t index = 0U; index < records.size(); ++index)
            {
                const ProgramRenderRecord &record = records[index];
                if (!record.emitted || record.emitted_generation != generation)
                {
                    continue;
                }
                found = true;
                return static_cast<std::uint64_t>(index);
            }
            return 0U;
        }
    } // namespace

    const char *timeline_solve_error_name(TimelineSolveError error) noexcept
    {
        switch (error)
        {
        case TimelineSolveError::None:
            return "None";
        case TimelineSolveError::MissingMainPacketTiming:
            return "MissingMainPacketTiming";
        case TimelineSolveError::MissingAlphaPacketTiming:
            return "MissingAlphaPacketTiming";
        case TimelineSolveError::MissingRenderLedger:
            return "MissingRenderLedger";
        case TimelineSolveError::MissingPrefixContent:
            return "MissingPrefixContent";
        case TimelineSolveError::AmbiguousMainGeneration:
            return "AmbiguousMainGeneration";
        case TimelineSolveError::NonContiguousPts:
            return "NonContiguousPts";
        case TimelineSolveError::TimebaseMismatch:
            return "TimebaseMismatch";
        case TimelineSolveError::MissingTailCoverage:
            return "MissingTailCoverage";
        case TimelineSolveError::UnsupportedObsTimingModel:
            return "UnsupportedObsTimingModel";
        }
        return "Unknown";
    }

    std::string timeline_solve_error_message(TimelineSolveError error)
    {
        return std::string{"Alpha Recorder GPU texture timeline could not be proven: "} +
               timeline_solve_error_name(error);
    }

    GpuTextureTimelineSolveResult solve_gpu_texture_timeline(
        const GpuTextureTimelineInput &input) noexcept
    {
        GpuTextureTimelineSolveResult result{};
        if (input.main_packets.empty())
        {
            result.error = TimelineSolveError::MissingMainPacketTiming;
            return result;
        }
        if (input.alpha_packets.empty())
        {
            result.error = TimelineSolveError::MissingAlphaPacketTiming;
            return result;
        }
        if (input.alpha_renders.empty())
        {
            result.error = TimelineSolveError::MissingRenderLedger;
            return result;
        }
        if (!has_consistent_timebase(input.alpha_packets))
        {
            result.error = TimelineSolveError::TimebaseMismatch;
            return result;
        }

        const std::vector<GpuTexturePacketRecord> main_sorted = sorted_by_pts(input.main_packets);
        const std::vector<GpuTexturePacketRecord> alpha_sorted = sorted_by_pts(input.alpha_packets);
        const GpuTexturePacketRecord &main_first = main_sorted.front();
        if (!main_first.has_cts || main_first.cts == 0U)
        {
            result.error = TimelineSolveError::MissingMainPacketTiming;
            return result;
        }

        const ProgramRenderRecord *main_render = find_render_by_cts(input.alpha_renders, main_first.cts);
        if (main_render == nullptr)
        {
            result.error = TimelineSolveError::AmbiguousMainGeneration;
            return result;
        }

        bool main_generation_valid = false;
        const std::uint64_t main_generation =
            main_content_generation(*main_render, input.main_phase, main_generation_valid);
        if (!main_generation_valid)
        {
            result.error = TimelineSolveError::MissingPrefixContent;
            return result;
        }

        bool alpha_render_found = false;
        const std::uint64_t alpha_render_index =
            render_index_for_generation(input.alpha_renders, main_generation, alpha_render_found);
        if (!alpha_render_found)
        {
            result.error = TimelineSolveError::MissingPrefixContent;
            return result;
        }

        const std::int64_t alpha_step = packet_pts_step(alpha_sorted);
        if (alpha_step <= 0)
        {
            result.error = TimelineSolveError::NonContiguousPts;
            return result;
        }

        const std::int64_t alpha_first_pts = alpha_sorted.front().pts;
        if (input.alpha_renders.size() < alpha_sorted.size())
        {
            result.error = TimelineSolveError::UnsupportedObsTimingModel;
            return result;
        }
        const std::uint64_t render_packet_offset =
            input.has_alpha_render_packet_offset
                ? input.alpha_render_packet_offset
                : static_cast<std::uint64_t>(input.alpha_renders.size() - alpha_sorted.size());
        if (alpha_render_index < render_packet_offset)
        {
            result.error = TimelineSolveError::MissingPrefixContent;
            return result;
        }
        const std::uint64_t alpha_packet_index = alpha_render_index - render_packet_offset;
        if (alpha_packet_index >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / alpha_step))
        {
            result.error = TimelineSolveError::MissingTailCoverage;
            return result;
        }
        const std::int64_t alpha_visible_pts =
            alpha_first_pts + static_cast<std::int64_t>(alpha_packet_index) * alpha_step;

        std::set<std::int64_t> alpha_pts;
        for (const GpuTexturePacketRecord &packet : alpha_sorted)
        {
            alpha_pts.insert(packet.pts);
        }

        const std::uint64_t main_packet_count = static_cast<std::uint64_t>(main_sorted.size());
        if (!pts_range_present(alpha_pts, alpha_visible_pts, alpha_step, main_packet_count))
        {
            result.error = TimelineSolveError::MissingTailCoverage;
            return result;
        }

        if (main_packet_count >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / alpha_step))
        {
            result.error = TimelineSolveError::MissingTailCoverage;
            return result;
        }

        result.solution.range.media_time = alpha_visible_pts;
        result.solution.range.duration = static_cast<std::int64_t>(main_packet_count) * alpha_step;
        result.solution.main_generation = main_generation;
        result.solution.alpha_generation = main_generation;
        result.solution.alpha_pts_step = alpha_step;
        result.solution.alpha_render_index = alpha_render_index;
        result.solution.main_packet_count = main_packet_count;
        result.solution.alpha_packet_count = static_cast<std::uint64_t>(alpha_sorted.size());
        result.error = TimelineSolveError::None;
        return result;
    }

} // namespace alpha_recorder::obs
