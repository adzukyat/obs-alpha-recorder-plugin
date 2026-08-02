#pragma once

#include "alpha_output_sink.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct encoder_packet;
struct obs_output;
using obs_output_t = struct obs_output;

namespace alpha_recorder::obs
{

    struct DirectMp4MuxerConfig
    {
        std::filesystem::path path{};
        bool quicktime_flavor = false;
    };

    using DirectMp4MuxerStats = AlphaMovieMuxerStats;

    class DirectMp4Muxer final
    {
    public:
        DirectMp4Muxer();
        ~DirectMp4Muxer() noexcept;

        DirectMp4Muxer(const DirectMp4Muxer &) = delete;
        DirectMp4Muxer &operator=(const DirectMp4Muxer &) = delete;

        [[nodiscard]] bool open(const DirectMp4MuxerConfig &config,
                                std::string *error_message = nullptr);
        [[nodiscard]] bool begin(obs_output_t *output, std::string *error_message = nullptr);
        [[nodiscard]] bool set_visible_range(const AlphaVisiblePacketRange &range,
                                             std::string *error_message = nullptr);
        [[nodiscard]] bool submit_packet(encoder_packet *packet,
                                         std::string *error_message = nullptr);
        [[nodiscard]] bool finalize(std::string *error_message = nullptr);

        void close_storage() noexcept;
        void abort() noexcept;

        [[nodiscard]] const DirectMp4MuxerStats &stats() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace alpha_recorder::obs
