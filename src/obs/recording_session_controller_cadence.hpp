#pragma once

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
        bool duplicate_previous = false;
    };

    class RawVideoCadenceTracker
    {
    public:
        [[nodiscard]] OutputFrameCadence remember(const std::uint8_t *frame_data,
                                                  std::uint64_t timestamp) noexcept
        {
            const bool duplicate_previous = frame_data != nullptr && frame_data == last_frame_data_;
            last_frame_data_ = frame_data;
            return OutputFrameCadence{timestamp, duplicate_previous};
        }

        void reset() noexcept
        {
            last_frame_data_ = nullptr;
        }

    private:
        const std::uint8_t *last_frame_data_ = nullptr;
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

} // namespace alpha_recorder::obs
