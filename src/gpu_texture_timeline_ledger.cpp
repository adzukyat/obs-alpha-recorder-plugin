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

        [[nodiscard]] std::int64_t sys_dts_presentation_bias(
            const std::vector<GpuTexturePacketRecord> &packets,
            std::int64_t pts_step,
            std::uint64_t latency_frames) noexcept
        {
            if (pts_step <= 0)
            {
                return 0;
            }

            for (const GpuTexturePacketRecord &packet : packets)
            {
                if (packet.pts > packet.dts)
                {
                    return latency_frames >= 16U ? -pts_step : pts_step;
                }
            }
            return 0;
        }

        [[nodiscard]] bool has_reordered_packets(
            const std::vector<GpuTexturePacketRecord> &packets) noexcept
        {
            return std::any_of(packets.begin(), packets.end(),
                               [](const GpuTexturePacketRecord &packet) {
                                   return packet.pts != packet.dts;
                               });
        }

        [[nodiscard]] std::uint64_t timestamp_delta_ns(std::uint64_t lhs,
                                                       std::uint64_t rhs) noexcept
        {
            return lhs >= rhs ? lhs - rhs : rhs - lhs;
        }

        [[nodiscard]] std::uint64_t frame_interval_ns(const GpuTextureTimelineInput &input) noexcept
        {
            const std::uint64_t fps_num = input.fps_num == 0U ? 60U : input.fps_num;
            const std::uint64_t fps_den = input.fps_den == 0U ? 1U : input.fps_den;
            return (1000000000ULL * fps_den + fps_num / 2U) / fps_num;
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
            std::uint64_t cts,
            std::uint64_t tolerance_ns) noexcept
        {
            const ProgramRenderRecord *match = nullptr;
            for (const ProgramRenderRecord &record : records)
            {
                if (timestamp_delta_ns(record.render_time_ns, cts) > tolerance_ns)
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

        [[nodiscard]] const ProgramRenderRecord *find_render_at_or_before_cts(
            const std::vector<ProgramRenderRecord> &records,
            std::uint64_t cts,
            std::uint64_t tolerance_ns) noexcept
        {
            const ProgramRenderRecord *match = nullptr;
            const std::uint64_t upper_bound = cts > std::numeric_limits<std::uint64_t>::max() - tolerance_ns
                                                  ? std::numeric_limits<std::uint64_t>::max()
                                                  : cts + tolerance_ns;
            for (const ProgramRenderRecord &record : records)
            {
                if (record.render_time_ns == 0U || record.render_time_ns > upper_bound)
                {
                    continue;
                }
                if (match == nullptr || record.render_time_ns > match->render_time_ns)
                {
                    match = &record;
                }
            }
            return match;
        }

        [[nodiscard]] GpuTexturePacketRecord *find_packet_by_pts(
            std::vector<GpuTexturePacketRecord> &records,
            std::int64_t pts,
            bool &ambiguous) noexcept
        {
            GpuTexturePacketRecord *match = nullptr;
            ambiguous = false;
            for (GpuTexturePacketRecord &record : records)
            {
                if (record.pts != pts)
                {
                    continue;
                }
                if (match != nullptr)
                {
                    ambiguous = true;
                    return nullptr;
                }
                match = &record;
            }
            return match;
        }

        [[nodiscard]] const GpuTexturePacketRecord *find_packet_by_pts(
            const std::vector<GpuTexturePacketRecord> &records,
            std::int64_t pts,
            bool &ambiguous) noexcept
        {
            const GpuTexturePacketRecord *match = nullptr;
            ambiguous = false;
            for (const GpuTexturePacketRecord &record : records)
            {
                if (record.pts != pts)
                {
                    continue;
                }
                if (match != nullptr)
                {
                    ambiguous = true;
                    return nullptr;
                }
                match = &record;
            }
            return match;
        }

        [[nodiscard]] TimelineSolveError resolve_packet_generation_from_cts(
            const std::vector<ProgramRenderRecord> &renders,
            GpuTexturePacketRecord &packet,
            std::uint64_t tolerance_ns) noexcept
        {
            if (!packet.has_input_cts || packet.input_cts == 0U)
            {
                return TimelineSolveError::None;
            }

            const ProgramRenderRecord *render = find_render_by_cts(renders, packet.input_cts, tolerance_ns);
            if (render == nullptr)
            {
                packet.ambiguous_generation = true;
                packet.has_generation = false;
                return TimelineSolveError::AmbiguousGeneration;
            }

            if (!render->emitted)
            {
                packet.has_generation = false;
                packet.ambiguous_generation = false;
                packet.input_generation = 0U;
                packet.emitted_generation = 0U;
                return TimelineSolveError::None;
            }

            if (packet.has_generation && packet.emitted_generation != render->emitted_generation)
            {
                packet.ambiguous_generation = true;
                packet.has_generation = false;
                return TimelineSolveError::AmbiguousGeneration;
            }

            packet.input_generation = render->generation;
            packet.emitted_generation = render->emitted_generation;
            packet.has_generation = true;
            packet.ambiguous_generation = false;
            return TimelineSolveError::None;
        }

        [[nodiscard]] const ProgramRenderRecord *find_render_by_emitted_generation(
            const std::vector<ProgramRenderRecord> &records,
            std::uint64_t generation,
            bool &ambiguous) noexcept
        {
            const ProgramRenderRecord *match = nullptr;
            ambiguous = false;
            for (const ProgramRenderRecord &record : records)
            {
                if (!record.emitted || record.emitted_generation != generation)
                {
                    continue;
                }
                if (match != nullptr)
                {
                    ambiguous = true;
                    return nullptr;
                }
                match = &record;
            }
            return match;
        }

        [[nodiscard]] bool assign_packet_generation_from_render(
            const std::vector<ProgramRenderRecord> &renders,
            GpuTexturePacketRecord &packet,
            std::uint64_t generation) noexcept
        {
            bool ambiguous = false;
            const ProgramRenderRecord *render =
                find_render_by_emitted_generation(renders, generation, ambiguous);
            if (ambiguous)
            {
                packet.ambiguous_generation = true;
                return false;
            }
            if (render == nullptr)
            {
                return false;
            }

            packet.input_generation = render->generation;
            packet.emitted_generation = render->emitted_generation;
            packet.has_generation = true;
            packet.ambiguous_generation = false;
            return true;
        }

        void fill_alpha_generations_from_render_ledger(
            const std::vector<ProgramRenderRecord> &renders,
            std::vector<GpuTexturePacketRecord> &packets,
            std::int64_t pts_step) noexcept
        {
            if (packets.empty() || pts_step <= 0)
            {
                return;
            }

            std::size_t first_proven_index = packets.size();
            for (std::size_t index = 0U; index < packets.size(); ++index)
            {
                if (packets[index].has_generation)
                {
                    first_proven_index = index;
                    break;
                }
            }
            if (first_proven_index == packets.size())
            {
                return;
            }

            for (std::size_t index = first_proven_index; index > 0U; --index)
            {
                GpuTexturePacketRecord &anchor = packets[index];
                GpuTexturePacketRecord &target = packets[index - 1U];
                if (!anchor.has_generation || target.has_generation)
                {
                    continue;
                }
                if (anchor.pts - target.pts != pts_step || anchor.emitted_generation == 0U)
                {
                    return;
                }
                if (!assign_packet_generation_from_render(renders, target,
                                                          anchor.emitted_generation - 1U))
                {
                    return;
                }
            }

            std::size_t previous_proven_index = packets.size();
            for (std::size_t index = 0U; index < packets.size(); ++index)
            {
                if (packets[index].has_generation)
                {
                    if (previous_proven_index != packets.size() &&
                        index > previous_proven_index + 1U)
                    {
                        const GpuTexturePacketRecord &previous = packets[previous_proven_index];
                        const GpuTexturePacketRecord &next = packets[index];
                        const std::int64_t pts_distance = next.pts - previous.pts;
                        if (pts_distance <= 0 || pts_distance % pts_step != 0)
                        {
                            previous_proven_index = index;
                            continue;
                        }

                        const std::uint64_t slot_distance =
                            static_cast<std::uint64_t>(pts_distance / pts_step);
                        const std::uint64_t generation_distance =
                            next.emitted_generation >= previous.emitted_generation
                                ? next.emitted_generation - previous.emitted_generation
                                : 0U;
                        if (slot_distance == generation_distance)
                        {
                            for (std::size_t fill = previous_proven_index + 1U; fill < index; ++fill)
                            {
                                const std::int64_t fill_pts_distance =
                                    packets[fill].pts - previous.pts;
                                if (fill_pts_distance <= 0 || fill_pts_distance % pts_step != 0)
                                {
                                    break;
                                }
                                const std::uint64_t fill_generation =
                                    previous.emitted_generation +
                                    static_cast<std::uint64_t>(fill_pts_distance / pts_step);
                                if (!assign_packet_generation_from_render(renders, packets[fill],
                                                                          fill_generation))
                                {
                                    break;
                                }
                            }
                        }
                    }
                    previous_proven_index = index;
                    continue;
                }

                if (previous_proven_index == packets.size())
                {
                    continue;
                }
                const GpuTexturePacketRecord &previous = packets[previous_proven_index];
                const std::int64_t pts_distance = packets[index].pts - previous.pts;
                if (pts_distance <= 0 || pts_distance % pts_step != 0)
                {
                    continue;
                }
                const std::uint64_t generation =
                    previous.emitted_generation + static_cast<std::uint64_t>(pts_distance / pts_step);
                if (!assign_packet_generation_from_render(renders, packets[index], generation))
                {
                    return;
                }
            }
        }

        struct AlphaGenerationResolution
        {
            TimelineSolveError error = TimelineSolveError::None;
            AlphaEpochSource source = AlphaEpochSource::None;
            std::uint64_t latency_frames = 0U;
            std::uint64_t latency_ns = 0U;
            std::uint64_t candidate_count = 0U;
        };

        [[nodiscard]] std::uint64_t count_sys_dts_latency_assignments(
            const GpuTextureTimelineInput &input,
            const std::vector<GpuTexturePacketRecord> &alpha_packets,
            std::int64_t alpha_step,
            std::uint64_t latency_frames,
            std::uint64_t interval_ns,
            std::int64_t presentation_bias,
            bool &valid) noexcept
        {
            valid = true;
            if (latency_frames >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / alpha_step))
            {
                valid = false;
                return 0U;
            }

            const std::int64_t pts_delay = static_cast<std::int64_t>(latency_frames) * alpha_step;
            const std::uint64_t latency_ns = latency_frames * interval_ns;
            std::uint64_t assignment_count = 0U;
            for (const GpuTexturePacketRecord &packet : alpha_packets)
            {
                if (packet.sys_dts_usec <= 0 || packet.pts < pts_delay)
                {
                    continue;
                }

                const std::uint64_t packet_sys_dts_ns =
                    static_cast<std::uint64_t>(packet.sys_dts_usec) * 1000U;
                if (packet_sys_dts_ns < latency_ns)
                {
                    continue;
                }

                bool ambiguous_target = false;
                const std::int64_t target_pts = packet.pts - pts_delay + presentation_bias;
                const GpuTexturePacketRecord *target =
                    find_packet_by_pts(alpha_packets, target_pts, ambiguous_target);
                if (ambiguous_target)
                {
                    valid = false;
                    return 0U;
                }
                if (target == nullptr)
                {
                    continue;
                }

                const std::uint64_t derived_cts = packet_sys_dts_ns - latency_ns;
                if (target->has_input_cts &&
                    timestamp_delta_ns(target->input_cts, derived_cts) > input.cts_tolerance_ns)
                {
                    valid = false;
                    return 0U;
                }

                const ProgramRenderRecord *render =
                    find_render_by_cts(input.alpha_renders, derived_cts, input.cts_tolerance_ns);
                if (render == nullptr)
                {
                    continue;
                }
                if (target->has_generation && target->emitted_generation != render->emitted_generation)
                {
                    valid = false;
                    return 0U;
                }
                if (render->emitted)
                {
                    ++assignment_count;
                }
            }
            return assignment_count;
        }

        [[nodiscard]] AlphaGenerationResolution resolve_alpha_generations(
            const GpuTextureTimelineInput &input,
            std::vector<GpuTexturePacketRecord> &alpha_packets,
            std::int64_t alpha_step) noexcept
        {
            AlphaGenerationResolution result{};

            const bool explicit_generation_seen =
                std::any_of(alpha_packets.begin(), alpha_packets.end(),
                            [](const GpuTexturePacketRecord &packet) {
                                return packet.has_generation;
                            });

            bool direct_cts_seen = false;
            for (GpuTexturePacketRecord &packet : alpha_packets)
            {
            if (!packet.has_input_cts)
            {
                continue;
            }
            direct_cts_seen = true;
            if (packet.has_generation)
            {
                continue;
            }
            const TimelineSolveError error = resolve_packet_generation_from_cts(
                input.alpha_renders, packet, input.cts_tolerance_ns);
                if (error != TimelineSolveError::None)
                {
                    result.error = error;
                    return result;
                }
            }

            const bool needs_sys_dts = !explicit_generation_seen &&
                std::any_of(alpha_packets.begin(), alpha_packets.end(),
                            [](const GpuTexturePacketRecord &packet) {
                                return !packet.has_generation && !packet.has_input_cts &&
                                       packet.sys_dts_usec > 0;
                            });
            if (!needs_sys_dts)
            {
                result.source = direct_cts_seen ? AlphaEpochSource::DirectCts : AlphaEpochSource::None;
                return result;
            }

            if (alpha_step <= 0)
            {
                result.error = TimelineSolveError::NonContiguousPts;
                return result;
            }

            std::uint64_t last_render_cts = 0U;
            for (const ProgramRenderRecord &render : input.alpha_renders)
            {
                last_render_cts = std::max(last_render_cts, render.render_time_ns);
            }

            std::uint64_t last_packet_sys_dts_ns = 0U;
            for (const GpuTexturePacketRecord &packet : alpha_packets)
            {
                if (packet.sys_dts_usec <= 0)
                {
                    continue;
                }
                last_packet_sys_dts_ns =
                    std::max(last_packet_sys_dts_ns,
                             static_cast<std::uint64_t>(packet.sys_dts_usec) * 1000U);
            }
            if (last_render_cts == 0U || last_packet_sys_dts_ns == 0U)
            {
                result.error = TimelineSolveError::UnsupportedObsTimingModel;
                return result;
            }

            const std::uint64_t interval_ns = frame_interval_ns(input);
            if (interval_ns == 0U)
            {
                result.error = TimelineSolveError::UnsupportedObsTimingModel;
                return result;
            }

            const std::uint64_t latency_ns =
                last_render_cts > last_packet_sys_dts_ns ? last_render_cts - last_packet_sys_dts_ns : 0U;
            const std::uint64_t estimated_latency_frames = (latency_ns + interval_ns / 2U) / interval_ns;
            const std::uint64_t estimated_latency_ns = estimated_latency_frames * interval_ns;
            if (timestamp_delta_ns(latency_ns, estimated_latency_ns) > input.cts_tolerance_ns)
            {
                result.error = TimelineSolveError::UnsupportedObsTimingModel;
                return result;
            }
            const std::uint64_t search_first =
                direct_cts_seen ? estimated_latency_frames
                                : (estimated_latency_frames > 8U ? estimated_latency_frames - 8U : 0U);
            const std::uint64_t search_last =
                direct_cts_seen ? estimated_latency_frames : estimated_latency_frames + 8U;
            std::uint64_t latency_frames = 0U;
            std::uint64_t best_assignment_count = 0U;
            std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
            for (std::uint64_t candidate = search_first; candidate <= search_last; ++candidate)
            {
                bool valid_candidate = false;
                const std::int64_t presentation_bias =
                    sys_dts_presentation_bias(alpha_packets, alpha_step, candidate);
                const std::uint64_t assignment_count =
                    count_sys_dts_latency_assignments(input, alpha_packets, alpha_step, candidate,
                                                      interval_ns, presentation_bias, valid_candidate);
                if (!valid_candidate || assignment_count == 0U)
                {
                    continue;
                }
                ++result.candidate_count;
                const std::uint64_t distance =
                    candidate >= estimated_latency_frames ? candidate - estimated_latency_frames
                                                          : estimated_latency_frames - candidate;
                if (distance < best_distance ||
                    (distance == best_distance && assignment_count > best_assignment_count))
                {
                    best_assignment_count = assignment_count;
                    best_distance = distance;
                    latency_frames = candidate;
                }
            }
            if (best_assignment_count == 0U)
            {
                result.error = direct_cts_seen ? TimelineSolveError::AmbiguousAlphaEpoch
                                               : TimelineSolveError::UnsupportedObsTimingModel;
                return result;
            }

            const std::int64_t pts_delay = static_cast<std::int64_t>(latency_frames) * alpha_step;
            const std::int64_t presentation_bias =
                sys_dts_presentation_bias(alpha_packets, alpha_step, latency_frames);
            const std::uint64_t rounded_latency_ns = latency_frames * interval_ns;
            std::uint64_t assignment_count = 0U;
            result.latency_frames = latency_frames;
            result.latency_ns = rounded_latency_ns;

            for (const GpuTexturePacketRecord &packet : alpha_packets)
            {
                if (packet.sys_dts_usec <= 0 || packet.pts < pts_delay)
                {
                    continue;
                }

                const std::uint64_t packet_sys_dts_ns =
                    static_cast<std::uint64_t>(packet.sys_dts_usec) * 1000U;
                if (packet_sys_dts_ns < rounded_latency_ns)
                {
                    result.error = TimelineSolveError::UnsupportedObsTimingModel;
                    return result;
                }

                const std::int64_t target_pts = packet.pts - pts_delay + presentation_bias;
                bool ambiguous_target = false;
                GpuTexturePacketRecord *target =
                    find_packet_by_pts(alpha_packets, target_pts, ambiguous_target);
                if (ambiguous_target)
                {
                    result.error = TimelineSolveError::AmbiguousAlphaEpoch;
                    return result;
                }
                if (target == nullptr)
                {
                    continue;
                }

                const std::uint64_t derived_cts = packet_sys_dts_ns - rounded_latency_ns;
                if (target->has_input_cts &&
                    timestamp_delta_ns(target->input_cts, derived_cts) > input.cts_tolerance_ns)
                {
                    result.error = TimelineSolveError::AmbiguousAlphaEpoch;
                    return result;
                }

                const ProgramRenderRecord *render =
                    find_render_by_cts(input.alpha_renders, derived_cts, input.cts_tolerance_ns);
                if (render == nullptr)
                {
                    continue;
                }
                if (!render->emitted)
                {
                    continue;
                }
                if (target->has_generation && target->emitted_generation != render->emitted_generation)
                {
                    result.error = TimelineSolveError::AmbiguousAlphaEpoch;
                    return result;
                }

                target->input_cts = derived_cts;
                target->has_input_cts = true;
                target->input_generation = render->generation;
                target->emitted_generation = render->emitted_generation;
                target->has_generation = true;
                target->ambiguous_generation = false;
                ++assignment_count;
            }

            if (assignment_count == 0U)
            {
                result.error = TimelineSolveError::UnsupportedObsTimingModel;
                return result;
            }

            fill_alpha_generations_from_render_ledger(input.alpha_renders, alpha_packets, alpha_step);
            result.source = AlphaEpochSource::SysDts;
            return result;
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

        [[nodiscard]] TimelineSolveError main_packet_content_generation(
            const GpuTextureTimelineInput &input,
            const GpuTexturePacketRecord &packet,
            std::uint64_t &generation) noexcept
        {
            generation = 0U;
            if (!packet.has_input_cts || packet.input_cts == 0U)
            {
                return TimelineSolveError::MissingMainPacketTiming;
            }

            const ProgramRenderRecord *render =
                find_render_at_or_before_cts(input.alpha_renders, packet.input_cts, input.cts_tolerance_ns);
            if (render == nullptr)
            {
                const auto first_render = std::min_element(
                    input.alpha_renders.begin(), input.alpha_renders.end(),
                    [](const ProgramRenderRecord &left,
                       const ProgramRenderRecord &right) {
                        return left.render_time_ns < right.render_time_ns;
                    });
                return first_render != input.alpha_renders.end() &&
                               packet.input_cts + input.cts_tolerance_ns <
                                   first_render->render_time_ns
                           ? TimelineSolveError::MissingPrefixContent
                           : TimelineSolveError::AmbiguousMainGeneration;
            }

            bool valid = false;
            generation = main_content_generation(*render, input.main_phase, valid);
            return valid ? TimelineSolveError::None : TimelineSolveError::MissingPrefixContent;
        }

        [[nodiscard]] const GpuTexturePacketRecord *packet_for_pts(
            const std::vector<const GpuTexturePacketRecord *> &packets,
            std::int64_t pts) noexcept
        {
            const auto found = std::find_if(packets.begin(), packets.end(),
                                            [pts](const GpuTexturePacketRecord *packet) {
                                                return packet != nullptr && packet->pts == pts;
                                            });
            return found == packets.end() ? nullptr : *found;
        }
    } // namespace

    MainPacketLedgerReconcileResult reconcile_main_packet_ledger_to_written_count(
        std::vector<GpuTexturePacketRecord> &packets,
        std::uint64_t written_packet_count) noexcept
    {
        MainPacketLedgerReconcileResult result{};
        result.callback_packet_count = static_cast<std::uint64_t>(packets.size());
        result.written_packet_count = written_packet_count;
        if (written_packet_count > result.callback_packet_count)
        {
            return result;
        }

        result.removed_unwritten_suffix_packets =
            result.callback_packet_count - written_packet_count;
        result.removed_unwritten_suffix.assign(
            packets.begin() +
                static_cast<std::ptrdiff_t>(written_packet_count),
            packets.end());
        packets.resize(static_cast<std::size_t>(written_packet_count));
        result.ok = true;
        return result;
    }

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
        case TimelineSolveError::MissingAlphaGeneration:
            return "MissingAlphaGeneration";
        case TimelineSolveError::AmbiguousGeneration:
            return "AmbiguousGeneration";
        case TimelineSolveError::AmbiguousAlphaEpoch:
            return "AmbiguousAlphaEpoch";
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

    const char *alpha_epoch_source_name(AlphaEpochSource source) noexcept
    {
        switch (source)
        {
        case AlphaEpochSource::None:
            return "none";
        case AlphaEpochSource::DirectCts:
            return "direct_cts";
        case AlphaEpochSource::SysDts:
            return "sys_dts";
        }
        return "unknown";
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
        std::vector<GpuTexturePacketRecord> alpha_sorted = sorted_by_pts(input.alpha_packets);
        const std::int64_t alpha_step = packet_pts_step(alpha_sorted);
        if (alpha_step <= 0)
        {
            result.error = TimelineSolveError::NonContiguousPts;
            return result;
        }

        const AlphaGenerationResolution generation_resolution =
            resolve_alpha_generations(input, alpha_sorted, alpha_step);
        if (generation_resolution.error != TimelineSolveError::None)
        {
            result.solution.alpha_epoch_source = generation_resolution.source;
            result.solution.alpha_latency_frames = generation_resolution.latency_frames;
            result.solution.alpha_latency_ns = generation_resolution.latency_ns;
            result.solution.alpha_epoch_candidate_count = generation_resolution.candidate_count;
            result.error = generation_resolution.error;
            return result;
        }
        if (generation_resolution.source == AlphaEpochSource::SysDts &&
            has_reordered_packets(alpha_sorted) &&
            (input.main_phase == MainContentPhase::LiveProgramGeneration ||
             generation_resolution.latency_frames >= 16U))
        {
            result.solution.alpha_epoch_source = generation_resolution.source;
            result.solution.alpha_latency_frames = generation_resolution.latency_frames;
            result.solution.alpha_latency_ns = generation_resolution.latency_ns;
            result.solution.alpha_epoch_candidate_count = generation_resolution.candidate_count;
            result.error = TimelineSolveError::UnsupportedObsTimingModel;
            return result;
        }

        std::vector<std::uint64_t> main_generations{};
        main_generations.reserve(main_sorted.size());
        for (const GpuTexturePacketRecord &packet : main_sorted)
        {
            std::uint64_t generation = 0U;
            const TimelineSolveError error =
                main_packet_content_generation(input, packet, generation);
            if (error != TimelineSolveError::None)
            {
                result.error = error;
                return result;
            }
            main_generations.push_back(generation);
        }
        const std::uint64_t main_generation = main_generations.front();

        std::uint64_t alpha_packets_with_generation = 0U;
        for (const GpuTexturePacketRecord &packet : alpha_sorted)
        {
            if (packet.ambiguous_generation)
            {
                result.solution.alpha_epoch_source = generation_resolution.source;
                result.solution.alpha_latency_frames = generation_resolution.latency_frames;
                result.solution.alpha_latency_ns = generation_resolution.latency_ns;
                result.solution.alpha_epoch_candidate_count = generation_resolution.candidate_count;
                result.solution.alpha_packet_count = static_cast<std::uint64_t>(alpha_sorted.size());
                result.solution.alpha_packets_with_generation = alpha_packets_with_generation;
                result.error = TimelineSolveError::AmbiguousGeneration;
                return result;
            }
            alpha_packets_with_generation += packet.has_generation ? 1U : 0U;
        }
        result.solution.alpha_epoch_source = generation_resolution.source;
        result.solution.alpha_latency_frames = generation_resolution.latency_frames;
        result.solution.alpha_latency_ns = generation_resolution.latency_ns;
        result.solution.alpha_epoch_candidate_count = generation_resolution.candidate_count;
        result.solution.alpha_packet_count = static_cast<std::uint64_t>(alpha_sorted.size());
        result.solution.alpha_packets_with_generation = alpha_packets_with_generation;

        std::set<std::int64_t> alpha_pts;
        std::vector<const GpuTexturePacketRecord *> alpha_visible_packets;
        for (const GpuTexturePacketRecord &packet : alpha_sorted)
        {
            alpha_pts.insert(packet.pts);
            alpha_visible_packets.push_back(&packet);
        }

        const std::uint64_t main_packet_count = static_cast<std::uint64_t>(main_sorted.size());
        if (main_packet_count == 0U)
        {
            result.error = TimelineSolveError::MissingMainPacketTiming;
            return result;
        }

        const GpuTexturePacketRecord *alpha_visible_first = nullptr;
        bool saw_prefix_generation = false;
        bool saw_tail_candidate = false;
        bool saw_generation_mismatch = false;
        const std::uint64_t fps_frames =
            std::max<std::uint64_t>(
                1U,
                (static_cast<std::uint64_t>(input.fps_num) +
                 std::max<std::uint32_t>(1U, input.fps_den) - 1U) /
                    std::max<std::uint32_t>(1U, input.fps_den));
        const std::uint64_t required_clean_suffix_frames =
            std::max<std::uint64_t>(1U, fps_frames / 2U);
        const std::uint64_t maximum_transient_mismatches =
            std::max<std::uint64_t>(1U, fps_frames / 2U);
        for (const GpuTexturePacketRecord &candidate : alpha_sorted)
        {
            if (!candidate.has_generation || candidate.emitted_generation != main_generation)
            {
                continue;
            }
            saw_prefix_generation = true;
            const std::int64_t candidate_pts = candidate.pts;
            std::uint64_t visible_packet_count = 0U;
            bool missing_generation = false;
            bool generation_mismatch = false;
            std::uint64_t generation_mismatch_count = 0U;
            std::uint64_t terminal_clean_suffix_frames = 0U;
            while (visible_packet_count < main_packet_count &&
                   visible_packet_count <=
                       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / alpha_step))
            {
                const std::int64_t pts =
                    candidate_pts + static_cast<std::int64_t>(visible_packet_count) * alpha_step;
                const GpuTexturePacketRecord *alpha_packet =
                    packet_for_pts(alpha_visible_packets, pts);
                if (alpha_packet == nullptr)
                {
                    break;
                }
                if (!alpha_packet->has_generation)
                {
                    missing_generation = true;
                    break;
                }
                if (alpha_packet->emitted_generation != main_generations[visible_packet_count])
                {
                    generation_mismatch = true;
                    ++generation_mismatch_count;
                    terminal_clean_suffix_frames = 0U;
                    if (!input.allow_transient_generation_mismatch)
                    {
                        break;
                    }
                }
                else
                {
                    ++terminal_clean_suffix_frames;
                }
                ++visible_packet_count;
            }

            if (visible_packet_count > 0U)
            {
                const std::int64_t last_visible_alpha_pts =
                    candidate_pts + static_cast<std::int64_t>(visible_packet_count - 1U) * alpha_step;
                result.solution.range.media_time = candidate_pts;
                result.solution.range.duration = last_visible_alpha_pts + alpha_step - candidate_pts;
                result.solution.main_generation = main_generation;
                result.solution.alpha_generation = main_generation;
                result.solution.alpha_pts_step = alpha_step;
                result.solution.first_visible_alpha_pts = candidate_pts;
                result.solution.main_packet_count = main_packet_count;
                result.solution.alpha_packet_count = static_cast<std::uint64_t>(alpha_sorted.size());
                result.solution.alpha_packets_with_generation = alpha_packets_with_generation;
                result.solution.alpha_epoch_source = generation_resolution.source;
                result.solution.alpha_latency_frames = generation_resolution.latency_frames;
                result.solution.alpha_latency_ns = generation_resolution.latency_ns;
                result.solution.alpha_epoch_candidate_count = generation_resolution.candidate_count;
                result.solution.recovery_certified = false;
                result.solution.transient_generation_mismatches =
                    generation_mismatch_count;
                result.solution.terminal_clean_suffix_frames =
                    terminal_clean_suffix_frames;
            }

            const bool recovery_certified =
                generation_mismatch &&
                input.allow_transient_generation_mismatch &&
                generation_mismatch_count <= maximum_transient_mismatches &&
                terminal_clean_suffix_frames >=
                    required_clean_suffix_frames;
            if (visible_packet_count == main_packet_count &&
                (!generation_mismatch || recovery_certified))
            {
                alpha_visible_first = &candidate;
                result.solution.recovery_certified =
                    recovery_certified;
                break;
            }
            saw_tail_candidate = saw_tail_candidate || visible_packet_count > 0U;
            saw_generation_mismatch = saw_generation_mismatch || generation_mismatch;
            if (missing_generation)
            {
                result.error = TimelineSolveError::MissingAlphaGeneration;
                return result;
            }
        }

        if (alpha_visible_first == nullptr)
        {
            if (!saw_prefix_generation)
            {
                result.error = TimelineSolveError::MissingPrefixContent;
                return result;
            }
            result.error = saw_generation_mismatch && !saw_tail_candidate
                               ? TimelineSolveError::MissingAlphaGeneration
                               : TimelineSolveError::MissingTailCoverage;
            return result;
        }

        if (main_packet_count >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / alpha_step))
        {
            result.error = TimelineSolveError::MissingTailCoverage;
            return result;
        }

        const std::int64_t alpha_visible_pts = alpha_visible_first->pts;
        if (!pts_range_present(alpha_pts, alpha_visible_pts, alpha_step, main_packet_count))
        {
            result.error = TimelineSolveError::MissingTailCoverage;
            return result;
        }

        result.solution.range.media_time = alpha_visible_pts;
        result.solution.range.duration =
            static_cast<std::int64_t>(main_packet_count) * alpha_step;
        result.solution.main_generation = main_generation;
        result.solution.alpha_generation = main_generation;
        result.solution.alpha_pts_step = alpha_step;
        result.solution.first_visible_alpha_pts = alpha_visible_pts;
        result.solution.main_packet_count = main_packet_count;
        result.solution.alpha_packet_count = static_cast<std::uint64_t>(alpha_sorted.size());
        result.solution.alpha_packets_with_generation = alpha_packets_with_generation;
        result.solution.alpha_epoch_source = generation_resolution.source;
        result.solution.alpha_latency_frames = generation_resolution.latency_frames;
        result.solution.alpha_latency_ns = generation_resolution.latency_ns;
        result.solution.alpha_epoch_candidate_count = generation_resolution.candidate_count;
        result.error = TimelineSolveError::None;
        return result;
    }

} // namespace alpha_recorder::obs
