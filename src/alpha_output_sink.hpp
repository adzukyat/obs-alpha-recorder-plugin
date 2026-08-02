#pragma once

#include "alpha_mask_video_writer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace alpha_recorder::obs
{

    using AlphaOutputSinkConfig = AlphaMaskVideoWriterConfig;
    using AlphaOutputSinkStats = AlphaMaskVideoWriterStats;
    using AlphaOutputFrameDisposition = AlphaMaskVideoWriterFrameDisposition;

    struct AlphaVisiblePacketRange
    {
        std::int64_t media_time = 0;
        std::int64_t duration = 0;
    };

    struct AlphaMovieMuxerStats
    {
        std::uint64_t packet_count = 0U;
        std::uint64_t keyframe_count = 0U;
        std::uint64_t packet_bytes = 0U;
        std::uint64_t muxed_packet_count = 0U;
        std::uint64_t max_buffered_packet_count = 0U;
        std::uint64_t max_buffered_packet_bytes = 0U;
        std::int64_t first_pts = 0;
        std::int64_t last_pts = 0;
        bool finalized = false;
    };

    class CpuAlphaOutputSink final
    {
    public:
        [[nodiscard]] bool open(const AlphaOutputSinkConfig &config,
                                std::string *error_message = nullptr) noexcept
        {
            return writer_.open(config, error_message);
        }

        [[nodiscard]] bool write_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                                       std::string *error_message = nullptr,
                                       AlphaOutputFrameDisposition *disposition = nullptr) noexcept
        {
            return writer_.write_frame(std::move(alpha), error_message, disposition);
        }

        [[nodiscard]] bool close(std::string *error_message = nullptr,
                                 AlphaOutputSinkStats *stats = nullptr) noexcept
        {
            return writer_.close(error_message, stats);
        }

        [[nodiscard]] bool is_open() const noexcept
        {
            return writer_.is_open();
        }

        [[nodiscard]] const std::filesystem::path &path() const noexcept
        {
            return writer_.path();
        }

    private:
        AlphaMaskVideoWriter writer_{};
    };

} // namespace alpha_recorder::obs
