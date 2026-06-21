#pragma once

#include "direct_mp4_muxer.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace alpha_recorder::obs
{

    struct GpuTextureAlphaOutputSinkConfig
    {
        std::filesystem::path path{};
    };

    using GpuTextureAlphaOutputSinkStats = DirectMp4MuxerStats;

    class GpuTextureAlphaOutputSink final
    {
    public:
        GpuTextureAlphaOutputSink() = default;
        ~GpuTextureAlphaOutputSink() noexcept;

        GpuTextureAlphaOutputSink(const GpuTextureAlphaOutputSink &) = delete;
        GpuTextureAlphaOutputSink &operator=(const GpuTextureAlphaOutputSink &) = delete;

        [[nodiscard]] bool open(const GpuTextureAlphaOutputSinkConfig &config,
                                std::string *error_message = nullptr);
        [[nodiscard]] bool begin_mux(obs_output_t *output, std::string *error_message = nullptr);
        [[nodiscard]] bool set_visible_range(const AlphaVisiblePacketRange &range,
                                             std::string *error_message = nullptr);
        [[nodiscard]] bool submit_packet(encoder_packet *packet, std::string *error_message = nullptr);
        [[nodiscard]] bool finalize(std::string *error_message = nullptr);

        void close_storage() noexcept;
        void abort() noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] bool is_accepting_packets() const noexcept;
        [[nodiscard]] const std::filesystem::path &path() const noexcept;
        [[nodiscard]] const GpuTextureAlphaOutputSinkStats &stats() const noexcept;

    private:
        DirectMp4Muxer muxer_{};
    };

} // namespace alpha_recorder::obs
