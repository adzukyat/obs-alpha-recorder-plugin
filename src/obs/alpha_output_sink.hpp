#pragma once

#include "alpha_recorder/export_worker.hpp"

#include <cstddef>
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

    class IAlphaOutputSink
    {
    public:
        virtual ~IAlphaOutputSink() noexcept = default;

        [[nodiscard]] virtual bool open(const AlphaOutputSinkConfig &config,
                                        std::string *error_message = nullptr) noexcept = 0;
        [[nodiscard]] virtual bool close(std::string *error_message = nullptr,
                                         AlphaOutputSinkStats *stats = nullptr) noexcept = 0;
        [[nodiscard]] virtual bool is_open() const noexcept = 0;
        [[nodiscard]] virtual const std::filesystem::path &path() const noexcept = 0;
        [[nodiscard]] virtual std::uint64_t frame_count() const noexcept = 0;
    };

    class IAlphaFrameSink : public IAlphaOutputSink
    {
    public:
        [[nodiscard]] virtual bool write_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                                               std::string *error_message = nullptr,
                                               AlphaOutputFrameDisposition *disposition = nullptr) noexcept = 0;
    };

    struct AlphaEncodedPacketView
    {
        const std::uint8_t *data = nullptr;
        std::size_t size = 0U;
        std::int64_t pts = 0;
        std::int64_t dts = 0;
        std::uint64_t cts = 0U;
        bool keyframe = false;
    };

    struct AlphaVisiblePacketRange
    {
        std::int64_t media_time = 0;
        std::int64_t duration = 0;
    };

    class IAlphaPacketSink : public IAlphaOutputSink
    {
    public:
        [[nodiscard]] virtual bool write_packet(const AlphaEncodedPacketView &packet,
                                                std::string *error_message = nullptr) noexcept = 0;
        [[nodiscard]] virtual bool set_visible_range(const AlphaVisiblePacketRange &range,
                                                     std::string *error_message = nullptr) noexcept = 0;
    };

    class CpuAlphaOutputSink final : public IAlphaFrameSink
    {
    public:
        [[nodiscard]] bool open(const AlphaOutputSinkConfig &config,
                                std::string *error_message = nullptr) noexcept override
        {
            return writer_.open(config, error_message);
        }

        [[nodiscard]] bool write_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                                       std::string *error_message = nullptr,
                                       AlphaOutputFrameDisposition *disposition = nullptr) noexcept override
        {
            return writer_.write_frame(std::move(alpha), error_message, disposition);
        }

        [[nodiscard]] bool close(std::string *error_message = nullptr,
                                 AlphaOutputSinkStats *stats = nullptr) noexcept override
        {
            return writer_.close(error_message, stats);
        }

        [[nodiscard]] bool is_open() const noexcept override
        {
            return writer_.is_open();
        }

        [[nodiscard]] const std::filesystem::path &path() const noexcept override
        {
            return writer_.path();
        }

        [[nodiscard]] std::uint64_t frame_count() const noexcept override
        {
            return writer_.frame_count();
        }

    private:
        AlphaMaskVideoWriter writer_{};
    };

} // namespace alpha_recorder::obs
