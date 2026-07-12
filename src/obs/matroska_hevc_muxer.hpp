#pragma once

#include "alpha_output_sink.hpp"
#include "direct_mp4_muxer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct encoder_packet;
struct obs_output;
using obs_output_t = struct obs_output;

namespace alpha_recorder::obs
{

    struct MatroskaHevcMuxerConfig
    {
        std::filesystem::path path{};
    };

    class MatroskaHevcMuxer final
    {
    public:
        MatroskaHevcMuxer();
        ~MatroskaHevcMuxer() noexcept;

        MatroskaHevcMuxer(const MatroskaHevcMuxer &) = delete;
        MatroskaHevcMuxer &operator=(const MatroskaHevcMuxer &) = delete;

        [[nodiscard]] bool open(const MatroskaHevcMuxerConfig &config,
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
        [[nodiscard]] const std::filesystem::path &path() const noexcept;
        [[nodiscard]] const DirectMp4MuxerStats &stats() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace alpha_recorder::obs
