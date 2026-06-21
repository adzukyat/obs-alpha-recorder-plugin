#include "alpha_alignment_engine.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace alpha_recorder::obs
{
    namespace
    {
        [[nodiscard]] std::size_t rounded_fps(std::uint32_t fps_num, std::uint32_t fps_den) noexcept
        {
            if (fps_num == 0U || fps_den == 0U)
            {
                return 60U;
            }

            return static_cast<std::size_t>((static_cast<std::uint64_t>(fps_num) +
                                             static_cast<std::uint64_t>(fps_den) - 1U) /
                                            static_cast<std::uint64_t>(fps_den));
        }

        [[nodiscard]] std::uint64_t texture_encoder_alignment_delta_ns(std::uint32_t fps_num,
                                                                       std::uint32_t fps_den) noexcept
        {
            if (fps_num == 0U)
            {
                return 133333333ULL;
            }

            const std::uint64_t frame_ns =
                (1000000000ULL * static_cast<std::uint64_t>(fps_den == 0U ? 1U : fps_den)) /
                static_cast<std::uint64_t>(fps_num);
            return std::max<std::uint64_t>(frame_ns * 8ULL, 1ULL);
        }

        [[nodiscard]] std::uint64_t texture_successor_delta_ns(std::uint32_t fps_num,
                                                               std::uint32_t fps_den) noexcept
        {
            if (fps_num == 0U)
            {
                return 25000000ULL;
            }

            const std::uint64_t frame_ns =
                (1000000000ULL * static_cast<std::uint64_t>(fps_den == 0U ? 1U : fps_den)) /
                static_cast<std::uint64_t>(fps_num);
            return std::max<std::uint64_t>(frame_ns + frame_ns / 2ULL, 1ULL);
        }
    } // namespace

    std::size_t alignment_alpha_queue_frame_limit(std::uint32_t fps_num, std::uint32_t fps_den) noexcept
    {
        return std::clamp(rounded_fps(fps_num, fps_den) * 4U, static_cast<std::size_t>(120U),
                          static_cast<std::size_t>(240U));
    }

    std::size_t alignment_output_queue_frame_limit(std::uint32_t fps_num, std::uint32_t fps_den) noexcept
    {
        return std::clamp(rounded_fps(fps_num, fps_den) * 20U, static_cast<std::size_t>(240U),
                          static_cast<std::size_t>(3600U));
    }

    std::uint64_t plausible_alignment_delta_ns(std::uint32_t fps_num, std::uint32_t fps_den) noexcept
    {
        if (fps_num == 0U)
        {
            return 50000000ULL;
        }

        const std::uint64_t frame_ns =
            (1000000000ULL * static_cast<std::uint64_t>(fps_den == 0U ? 1U : fps_den)) /
            static_cast<std::uint64_t>(fps_num);
        return std::max<std::uint64_t>(frame_ns * 3ULL, 1ULL);
    }

    void AlphaAlignmentEngine::configure(const AlphaAlignmentEngineConfig &config) noexcept
    {
        config_ = config;
    }

    const AlphaAlignmentEngineConfig &AlphaAlignmentEngine::config() const noexcept
    {
        return config_;
    }

    void AlphaAlignmentEngine::clear_pending() noexcept
    {
        pending_alpha_frames_.clear();
        pending_encoded_alpha_frames_.clear();
        pending_output_frames_.clear();
        consecutive_output_duplicate_frames_ = 0U;
        last_aligned_packet_cts_ = 0U;
    }

    void AlphaAlignmentEngine::reset_frame_history() noexcept
    {
        last_alpha_frame_.alpha.reset();
        last_alpha_frame_.timestamp = 0U;
        last_captured_alpha_frame_.alpha.reset();
        last_captured_alpha_frame_.timestamp = 0U;
        fallback_black_alpha_.reset();
    }

    void AlphaAlignmentEngine::reset_all() noexcept
    {
        clear_pending();
        reset_frame_history();
    }

    bool AlphaAlignmentEngine::has_work() const noexcept
    {
        return pending_encoded_alpha_frames_.size() > config_.encoded_reorder_frames &&
               !pending_output_frames_.empty();
    }

    std::size_t AlphaAlignmentEngine::pending_alpha_size() const noexcept
    {
        return pending_alpha_frames_.size();
    }

    std::size_t AlphaAlignmentEngine::pending_output_size() const noexcept
    {
        return pending_output_frames_.size();
    }

    std::size_t AlphaAlignmentEngine::pending_encoded_size() const noexcept
    {
        return pending_encoded_alpha_frames_.size();
    }

    void AlphaAlignmentEngine::mark_pending_encoded_texture_encoded() noexcept
    {
        for (EncodedAlphaFrame &encoded_frame : pending_encoded_alpha_frames_)
        {
            encoded_frame.texture_encoded = true;
        }
    }

    void AlphaAlignmentEngine::remember_alpha_frame(AlphaFrame frame, LivePipelineTelemetry &telemetry)
    {
        remember_timestamp_span(frame.timestamp, telemetry.first_alpha_timestamp, telemetry.last_alpha_timestamp);
        last_captured_alpha_frame_ = frame;
        pending_alpha_frames_.push_back(std::move(frame));
        ++telemetry.queued_alpha_frames;
        telemetry.max_pending_alpha_frames =
            std::max(telemetry.max_pending_alpha_frames, pending_alpha_frames_.size());
        trim_pending_alpha_frames(telemetry);
    }

    void AlphaAlignmentEngine::remember_output_frame(OutputFrameCadence frame, LivePipelineTelemetry &telemetry)
    {
        remember_timestamp_span(frame.timestamp, telemetry.first_raw_output_timestamp,
                                telemetry.last_raw_output_timestamp);
        pending_output_frames_.push_back(frame);
        ++telemetry.raw_video_frames;
        telemetry.max_pending_output_frames =
            std::max(telemetry.max_pending_output_frames, pending_output_frames_.size());
        trim_pending_output_frames(telemetry);
    }

    void AlphaAlignmentEngine::queue_packet(std::int64_t pts,
                                            std::uint64_t cts,
                                            std::uint64_t fer,
                                            std::uint64_t ferc,
                                            bool texture_encoded,
                                            LivePipelineTelemetry &telemetry)
    {
        remember_timestamp_span(cts, telemetry.first_packet_cts, telemetry.last_packet_cts);
        if (fer != 0U)
        {
            telemetry.packet_fer_cts_delta.add(signed_timestamp_delta_ns(fer, cts));
        }
        if (telemetry.last_packet_cts_for_delta != 0U)
        {
            telemetry.packet_cts_delta.add(signed_timestamp_delta_ns(cts, telemetry.last_packet_cts_for_delta));
        }
        if (fer != 0U && telemetry.last_packet_fer_for_delta != 0U)
        {
            telemetry.packet_fer_delta.add(signed_timestamp_delta_ns(fer, telemetry.last_packet_fer_for_delta));
        }
        telemetry.last_packet_cts_for_delta = cts;
        if (fer != 0U)
        {
            telemetry.last_packet_fer_for_delta = fer;
        }

        EncodedAlphaFrame encoded_frame{};
        encoded_frame.pts = pts;
        encoded_frame.cts = cts;
        encoded_frame.fer = fer;
        encoded_frame.ferc = ferc;
        encoded_frame.texture_encoded = texture_encoded;
        pending_encoded_alpha_frames_.push_back(std::move(encoded_frame));
        ++telemetry.packet_frames;
        telemetry.max_pending_encoded_frames =
            std::max(telemetry.max_pending_encoded_frames, pending_encoded_alpha_frames_.size());
    }

    AlphaAlignmentDrainResult AlphaAlignmentEngine::drain(bool drain_all,
                                                          std::size_t max_frames,
                                                          LivePipelineTelemetry &telemetry,
                                                          const WriteFrameCallback &write_frame,
                                                          const TraceCallback &trace)
    {
        AlphaAlignmentDrainResult result{};
        const auto batch_start = std::chrono::steady_clock::now();
        AlphaFrame alpha_frame{};
        std::string error_message;

        while (!pending_encoded_alpha_frames_.empty() &&
               (drain_all || pending_encoded_alpha_frames_.size() > config_.encoded_reorder_frames) &&
               result.drained_frames < max_frames)
        {
            auto selected = std::min_element(
                pending_encoded_alpha_frames_.begin(), pending_encoded_alpha_frames_.end(),
                [](const EncodedAlphaFrame &left, const EncodedAlphaFrame &right) { return left.pts < right.pts; });
            if (pending_output_frames_.empty())
            {
                if (!drain_all)
                {
                    return result;
                }

                if (!repeat_frame(RepeatReason::MissingOutput, alpha_frame, telemetry, error_message))
                {
                    result.failed = true;
                    result.error_message = error_message;
                    return result;
                }
                pending_encoded_alpha_frames_.erase(selected);
                if (!write_resolved_frame(alpha_frame, telemetry, write_frame, error_message))
                {
                    result.failed = true;
                    result.error_message = error_message;
                    return result;
                }
                ++result.drained_frames;
                result.drained_any = true;
                ++telemetry.aligned_frames;
                continue;
            }

            const std::uint64_t aligned_packet_cts = selected->cts;
            if (!resolve_output_alpha_frame(*selected, drain_all, alpha_frame, telemetry, trace, error_message))
            {
                if (!error_message.empty())
                {
                    result.failed = true;
                    result.error_message = error_message;
                }
                return result;
            }

            last_aligned_packet_cts_ = aligned_packet_cts;
            pending_encoded_alpha_frames_.erase(selected);

            if (!write_resolved_frame(alpha_frame, telemetry, write_frame, error_message))
            {
                result.failed = true;
                result.error_message = error_message;
                return result;
            }
            ++result.drained_frames;
            result.drained_any = true;
            ++telemetry.aligned_frames;
        }

        if (result.drained_frames > 0U)
        {
            telemetry.alignment_batch.add(elapsed_ns(batch_start, std::chrono::steady_clock::now()));
        }
        return result;
    }

    void AlphaAlignmentEngine::trim_pending_alpha_frames(LivePipelineTelemetry &telemetry) noexcept
    {
        while (pending_alpha_frames_.size() > config_.alpha_queue_frame_limit)
        {
            pending_alpha_frames_.pop_front();
            ++telemetry.alignment_alpha_dropped_frames;
        }
    }

    void AlphaAlignmentEngine::trim_pending_output_frames(LivePipelineTelemetry &telemetry) noexcept
    {
        while (pending_output_frames_.size() > config_.output_queue_frame_limit)
        {
            pending_output_frames_.pop_front();
            ++telemetry.alignment_output_dropped_frames;
        }
    }

    void AlphaAlignmentEngine::discard_alpha_frames_through_timestamp(std::uint64_t timestamp,
                                                                      LivePipelineTelemetry &telemetry) noexcept
    {
        while (!pending_alpha_frames_.empty() && pending_alpha_frames_.front().timestamp <= timestamp)
        {
            pending_alpha_frames_.pop_front();
            ++telemetry.alignment_alpha_dropped_frames;
        }
    }

    bool AlphaAlignmentEngine::make_repeat_frame(AlphaFrame &frame, LivePipelineTelemetry &telemetry) noexcept
    {
        if (!last_alpha_frame_.empty())
        {
            frame = last_alpha_frame_;
            return true;
        }

        if (!last_captured_alpha_frame_.empty())
        {
            frame = last_captured_alpha_frame_;
            return true;
        }

        if (config_.width == 0U || config_.height == 0U)
        {
            return false;
        }

        const std::size_t width = static_cast<std::size_t>(config_.width);
        const std::size_t height = static_cast<std::size_t>(config_.height);
        if (width > std::numeric_limits<std::size_t>::max() / height)
        {
            return false;
        }

        const std::size_t bytes = width * height;
        if (!fallback_black_alpha_ || fallback_black_alpha_->size() != bytes)
        {
            try
            {
                fallback_black_alpha_ = std::make_shared<std::vector<std::uint8_t>>(bytes, std::uint8_t{0U});
            }
            catch (...)
            {
                fallback_black_alpha_.reset();
                return false;
            }
        }

        frame = AlphaFrame{0U, fallback_black_alpha_};
        ++telemetry.alignment_black_repeats;
        return true;
    }

    bool AlphaAlignmentEngine::repeat_frame(RepeatReason reason,
                                            AlphaFrame &frame,
                                            LivePipelineTelemetry &telemetry,
                                            std::string &error_message)
    {
        if (!make_repeat_frame(frame, telemetry))
        {
            if (reason == RepeatReason::MissingOutput)
            {
                error_message =
                    "Alpha Recorder could not recover alignment because no previous alpha frame is available for a missing output cadence frame.";
            }
            else if (reason == RepeatReason::TextureStall)
            {
                error_message =
                    "Alpha Recorder could not recover alignment because no previous alpha frame is available for texture-encoder stall recovery.";
            }
            else
            {
                error_message =
                    "Alpha Recorder could not recover alignment because no previous alpha frame is available for a missing alpha frame.";
            }
            return false;
        }

        ++telemetry.alignment_repeated_frames;
        if (reason == RepeatReason::MissingOutput)
        {
            ++telemetry.alignment_missing_output_repeats;
        }
        else if (reason == RepeatReason::MissingAlpha)
        {
            ++telemetry.alignment_missing_alpha_repeats;
        }
        else
        {
            ++telemetry.alignment_texture_stall_repeats;
        }
        return true;
    }

    bool AlphaAlignmentEngine::resolve_output_alpha_frame(const EncodedAlphaFrame &encoded_frame,
                                                          bool drain_all,
                                                          AlphaFrame &frame,
                                                          LivePipelineTelemetry &telemetry,
                                                          const TraceCallback &trace,
                                                          std::string &error_message)
    {
        if (encoded_frame.cts == 0U)
        {
            error_message =
                "Alpha Recorder could not align alpha output because OBS did not provide an encoded-frame composition timestamp.";
            return false;
        }

        const TimestampFrameSelection output_selection =
            encoded_frame.texture_encoded
                ? select_frame_after_timestamp(pending_output_frames_, encoded_frame.cts, drain_all)
                : select_frame_by_timestamp(pending_output_frames_, encoded_frame.cts, drain_all);
        if (output_selection.status == TimestampFrameSelectionStatus::WaitingForMoreFrames)
        {
            return false;
        }

        if (output_selection.status == TimestampFrameSelectionStatus::NoPlausibleFrame)
        {
            consecutive_output_duplicate_frames_ = 0U;
            return repeat_frame(RepeatReason::MissingOutput, frame, telemetry, error_message);
        }

        if (encoded_frame.texture_encoded &&
            output_selection.timestamp_delta >
                texture_encoder_alignment_delta_ns(config_.fps_num, config_.fps_den))
        {
            consecutive_output_duplicate_frames_ = 0U;
            return repeat_frame(RepeatReason::MissingOutput, frame, telemetry, error_message);
        }

        const OutputFrameCadence output_frame = pending_output_frames_[output_selection.selected_index];
        telemetry.alignment_output_cts_delta.add(signed_timestamp_delta_ns(output_frame.timestamp, encoded_frame.cts));
        if (duplicate_output_uses_previous_alpha(output_frame, last_alpha_frame_, frame))
        {
            telemetry.alignment_alpha_content_delta.add(
                signed_timestamp_delta_ns(frame.timestamp, output_frame.content_timestamp));
            trace_selection(trace, "output_duplicate", encoded_frame, output_frame, frame,
                            AlignmentTraceSelection{output_selection.selected_index, 0U,
                                                    output_selection.timestamp_delta,
                                                    timestamp_delta(frame.timestamp, output_frame.content_timestamp),
                                                    false, true});
            ++consecutive_output_duplicate_frames_;
            pending_output_frames_.erase(
                pending_output_frames_.begin(),
                pending_output_frames_.begin() + static_cast<std::ptrdiff_t>(output_selection.selected_index + 1U));
            return true;
        }

        const std::uint64_t texture_successor_delta =
            texture_successor_delta_ns(config_.fps_num, config_.fps_den);
        const bool cts_advances_like_successor =
            last_aligned_packet_cts_ != 0U && encoded_frame.cts > last_aligned_packet_cts_ &&
            encoded_frame.cts - last_aligned_packet_cts_ <= texture_successor_delta;
        const bool recover_texture_stall =
            encoded_frame.texture_encoded && cts_advances_like_successor &&
            ((consecutive_output_duplicate_frames_ >= 1U &&
              output_selection.timestamp_delta <= texture_successor_delta) ||
             (consecutive_output_duplicate_frames_ >= 2U &&
              output_selection.timestamp_delta <= plausible_alignment_delta_ns(config_.fps_num, config_.fps_den)));
        if (recover_texture_stall)
        {
            ++telemetry.texture_stall_corrections;
            if (!repeat_frame(RepeatReason::TextureStall, frame, telemetry, error_message))
            {
                return false;
            }
            telemetry.alignment_alpha_content_delta.add(
                signed_timestamp_delta_ns(frame.timestamp, output_frame.content_timestamp));
            trace_selection(trace, "texture_stall", encoded_frame, output_frame, frame,
                            AlignmentTraceSelection{output_selection.selected_index, 0U,
                                                    output_selection.timestamp_delta,
                                                    timestamp_delta(frame.timestamp, output_frame.content_timestamp),
                                                    false, true});
            consecutive_output_duplicate_frames_ = 0U;
            discard_alpha_frames_through_timestamp(output_frame.content_timestamp, telemetry);
            pending_output_frames_.erase(
                pending_output_frames_.begin(),
                pending_output_frames_.begin() + static_cast<std::ptrdiff_t>(output_selection.selected_index + 1U));
            return true;
        }

        consecutive_output_duplicate_frames_ = 0U;
        const TimestampFrameSelection alpha_selection =
            select_frame_by_timestamp(pending_alpha_frames_, output_frame.content_timestamp, drain_all);
        if (alpha_selection.status == TimestampFrameSelectionStatus::WaitingForMoreFrames)
        {
            if (drain_all && pending_alpha_frames_.empty())
            {
                consecutive_output_duplicate_frames_ = 0U;
                pending_output_frames_.erase(
                    pending_output_frames_.begin(),
                    pending_output_frames_.begin() + static_cast<std::ptrdiff_t>(output_selection.selected_index + 1U));
                if (!repeat_frame(RepeatReason::MissingAlpha, frame, telemetry, error_message))
                {
                    return false;
                }
                trace_selection(trace, "missing_alpha", encoded_frame, output_frame, frame,
                                AlignmentTraceSelection{output_selection.selected_index, 0U,
                                                        output_selection.timestamp_delta,
                                                        timestamp_delta(frame.timestamp, output_frame.content_timestamp),
                                                        false, true});
                return true;
            }
            return false;
        }

        if (alpha_selection.status == TimestampFrameSelectionStatus::NoPlausibleFrame)
        {
            consecutive_output_duplicate_frames_ = 0U;
            discard_alpha_frames_through_timestamp(output_frame.content_timestamp, telemetry);
            pending_output_frames_.erase(
                pending_output_frames_.begin(),
                pending_output_frames_.begin() + static_cast<std::ptrdiff_t>(output_selection.selected_index + 1U));
            if (!repeat_frame(RepeatReason::MissingAlpha, frame, telemetry, error_message))
            {
                return false;
            }
            trace_selection(trace, "missing_alpha", encoded_frame, output_frame, frame,
                            AlignmentTraceSelection{output_selection.selected_index, 0U,
                                                    output_selection.timestamp_delta,
                                                    timestamp_delta(frame.timestamp, output_frame.content_timestamp),
                                                    false, true});
            return true;
        }

        frame = std::move(pending_alpha_frames_[alpha_selection.selected_index]);
        telemetry.alignment_alpha_content_delta.add(
            signed_timestamp_delta_ns(frame.timestamp, output_frame.content_timestamp));
        trace_selection(trace, "selected", encoded_frame, output_frame, frame,
                        AlignmentTraceSelection{output_selection.selected_index, alpha_selection.selected_index,
                                                output_selection.timestamp_delta, alpha_selection.timestamp_delta,
                                                true, false});
        pending_alpha_frames_.erase(
            pending_alpha_frames_.begin(),
            pending_alpha_frames_.begin() + static_cast<std::ptrdiff_t>(alpha_selection.selected_index + 1U));
        pending_output_frames_.erase(
            pending_output_frames_.begin(),
            pending_output_frames_.begin() + static_cast<std::ptrdiff_t>(output_selection.selected_index + 1U));
        return true;
    }

    bool AlphaAlignmentEngine::write_resolved_frame(const AlphaFrame &frame,
                                                    LivePipelineTelemetry &telemetry,
                                                    const WriteFrameCallback &write_frame,
                                                    std::string &error_message)
    {
        bool queued = false;
        if (!write_frame(frame, queued, error_message))
        {
            if (error_message.empty())
            {
                error_message = "Alpha Recorder failed to write an alpha mask frame.";
            }
            return false;
        }

        if (queued)
        {
            last_alpha_frame_ = frame;
        }
        (void)telemetry;
        return true;
    }

    void AlphaAlignmentEngine::trace_selection(const TraceCallback &trace,
                                               const char *reason,
                                               const EncodedAlphaFrame &encoded_frame,
                                               const OutputFrameCadence &output_frame,
                                               const AlphaFrame &alpha_frame,
                                               const AlignmentTraceSelection &selection) const
    {
        if (!trace)
        {
            return;
        }

        trace(AlignmentTraceEvent{reason,
                                  encoded_frame,
                                  output_frame,
                                  alpha_frame,
                                  selection,
                                  pending_alpha_frames_.size(),
                                  pending_output_frames_.size(),
                                  pending_encoded_alpha_frames_.size(),
                                  consecutive_output_duplicate_frames_});
    }

} // namespace alpha_recorder::obs
