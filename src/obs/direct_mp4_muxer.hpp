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
    };

    struct DirectMp4MuxerStats
    {
        std::uint64_t packet_count = 0U;
        std::uint64_t keyframe_count = 0U;
        std::uint64_t packet_bytes = 0U;
        std::uint64_t muxed_packet_count = 0U;
        std::int64_t first_pts = 0;
        std::int64_t last_pts = 0;
        bool finalized = false;
    };

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

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] bool is_accepting_packets() const noexcept;
        [[nodiscard]] bool supports_visible_range() const noexcept;
        [[nodiscard]] const std::filesystem::path &path() const noexcept;
        [[nodiscard]] const DirectMp4MuxerStats &stats() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace alpha_recorder::obs
