#pragma once

#include "alpha_recorder/plugin.hpp"
#include "alpha_output_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct encoder_packet;
struct obs_output;
using obs_output_t = struct obs_output;

namespace alpha_recorder::obs
{

    struct GpuTextureAlphaOutputSinkConfig
    {
        std::filesystem::path path{};
        AlphaMovieContainer container = AlphaMovieContainer::Mp4;
        std::size_t tail_packet_buffer_size = 256U;
    };

    using GpuTextureAlphaOutputSinkStats = AlphaMovieMuxerStats;

    class GpuTextureAlphaOutputSink final
    {
    public:
        class Muxer;

        GpuTextureAlphaOutputSink();
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

        [[nodiscard]] const GpuTextureAlphaOutputSinkStats &stats() const noexcept;

    private:
        std::unique_ptr<Muxer> muxer_{};
        AlphaMovieMuxerStats empty_stats_{};
    };

} // namespace alpha_recorder::obs
