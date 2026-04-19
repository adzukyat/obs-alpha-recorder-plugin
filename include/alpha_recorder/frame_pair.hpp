#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace alpha_recorder
{

    struct FramePlane
    {
        std::vector<std::uint8_t> bytes{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;

        [[nodiscard]] bool has_pixels() const noexcept
        {
            return !bytes.empty();
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return bytes.size();
        }
    };

    struct FramePair
    {
        std::uint64_t sequence = 0;
        std::uint64_t pts = 0;
        FramePlane rgb{};
        FramePlane alpha{};

        [[nodiscard]] bool is_complete() const noexcept
        {
            return rgb.has_pixels() && alpha.has_pixels();
        }
    };

} // namespace alpha_recorder