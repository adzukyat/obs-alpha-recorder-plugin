#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace alpha_recorder::obs
{

    struct AlphaFrame
    {
        std::uint64_t timestamp = 0U;
        std::shared_ptr<std::vector<std::uint8_t>> alpha{};

        [[nodiscard]] bool empty() const noexcept
        {
            return !alpha || alpha->empty();
        }
    };

    struct OutputFrameCadence
    {
        std::uint64_t timestamp = 0U;
        std::uint64_t content_timestamp = 0U;
        bool duplicate_previous = false;
    };

    enum class TimestampFrameSelectionStatus
    {
        Selected,
        WaitingForMoreFrames,
        NoPlausibleFrame,
    };

    struct TimestampFrameSelection
    {
        TimestampFrameSelectionStatus status = TimestampFrameSelectionStatus::WaitingForMoreFrames;
        std::size_t selected_index = 0U;
        std::uint64_t timestamp_delta = 0U;
    };

    [[nodiscard]] inline constexpr std::uint64_t timestamp_delta(std::uint64_t left,
                                                                  std::uint64_t right) noexcept
    {
        return left > right ? left - right : right - left;
    }

    class RawVideoCadenceTracker
    {
    public:
        [[nodiscard]] OutputFrameCadence remember(const std::uint8_t *frame_data,
                                                  std::uint64_t timestamp) noexcept
        {
            const bool duplicate_previous = frame_data != nullptr && frame_data == last_frame_data_;
            last_frame_data_ = frame_data;
            if (!duplicate_previous)
            {
                last_content_timestamp_ = timestamp;
            }

            return OutputFrameCadence{timestamp, duplicate_previous ? last_content_timestamp_ : timestamp,
                                      duplicate_previous};
        }

        void reset() noexcept
        {
            last_frame_data_ = nullptr;
            last_content_timestamp_ = 0U;
        }

    private:
        const std::uint8_t *last_frame_data_ = nullptr;
        std::uint64_t last_content_timestamp_ = 0U;
    };

    [[nodiscard]] inline bool duplicate_output_uses_previous_alpha(const OutputFrameCadence &output_frame,
                                                                   const AlphaFrame &last_alpha_frame,
                                                                   AlphaFrame &resolved_frame) noexcept
    {
        if (!output_frame.duplicate_previous || last_alpha_frame.empty())
        {
            return false;
        }

        resolved_frame = last_alpha_frame;
        return true;
    }

    template <typename TimestampedFrames>
    [[nodiscard]] TimestampFrameSelection select_frame_by_timestamp(
        const TimestampedFrames &frames,
        std::uint64_t timestamp,
        bool drain_all)
    {
        if (frames.empty())
        {
            return {};
        }

        if (timestamp == 0U)
        {
            return TimestampFrameSelection{TimestampFrameSelectionStatus::NoPlausibleFrame, 0U, 0U};
        }

        std::size_t selected_index = 0U;
        std::uint64_t selected_delta = timestamp_delta(frames.front().timestamp, timestamp);
        if (selected_delta == 0U)
        {
            return TimestampFrameSelection{TimestampFrameSelectionStatus::Selected, 0U, 0U};
        }

        for (std::size_t index = 1U; index < frames.size(); ++index)
        {
            const std::uint64_t candidate_delta = timestamp_delta(frames[index].timestamp, timestamp);
            if (candidate_delta <= selected_delta)
            {
                selected_index = index;
                selected_delta = candidate_delta;
            }

            if (candidate_delta == 0U)
            {
                return TimestampFrameSelection{TimestampFrameSelectionStatus::Selected, index, 0U};
            }
        }

        if (!drain_all && frames.back().timestamp < timestamp)
        {
            return TimestampFrameSelection{TimestampFrameSelectionStatus::WaitingForMoreFrames, selected_index,
                                           selected_delta};
        }

        return TimestampFrameSelection{TimestampFrameSelectionStatus::NoPlausibleFrame, selected_index,
                                       selected_delta};
    }

    template <typename TimestampedFrames>
    [[nodiscard]] TimestampFrameSelection select_frame_after_timestamp(
        const TimestampedFrames &frames,
        std::uint64_t timestamp,
        bool drain_all)
    {
        if (frames.empty())
        {
            return {};
        }

        if (timestamp == 0U)
        {
            return TimestampFrameSelection{TimestampFrameSelectionStatus::NoPlausibleFrame, 0U, 0U};
        }

        std::size_t nearest_index = 0U;
        std::uint64_t nearest_delta = timestamp_delta(frames.front().timestamp, timestamp);
        for (std::size_t index = 0U; index < frames.size(); ++index)
        {
            const std::uint64_t candidate_delta = timestamp_delta(frames[index].timestamp, timestamp);
            if (candidate_delta < nearest_delta)
            {
                nearest_index = index;
                nearest_delta = candidate_delta;
            }

            if (frames[index].timestamp > timestamp)
            {
                return TimestampFrameSelection{TimestampFrameSelectionStatus::Selected, index, candidate_delta};
            }
        }

        if (!drain_all)
        {
            return TimestampFrameSelection{TimestampFrameSelectionStatus::WaitingForMoreFrames, nearest_index,
                                           nearest_delta};
        }

        return TimestampFrameSelection{TimestampFrameSelectionStatus::NoPlausibleFrame, nearest_index,
                                       nearest_delta};
    }

} // namespace alpha_recorder::obs
