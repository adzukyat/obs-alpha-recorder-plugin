#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace alpha_recorder::obs
{

    struct AlphaMaskVideoWriterConfig
    {
        std::filesystem::path output_path{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t fps_num = 30;
        std::uint32_t fps_den = 1;
    };

    struct AlphaMaskVideoWriterStats
    {
        std::uint64_t enqueued_frames = 0;
        std::uint64_t encoded_frames = 0;
        std::uint64_t overflow_repeated_frames = 0;
        std::uint64_t queued_bytes_total = 0;
        std::uint64_t enqueue_time_ns_total = 0;
        std::uint64_t enqueue_time_ns_max = 0;
        std::uint64_t encode_time_ns_total = 0;
        std::uint64_t encode_time_ns_max = 0;
        std::uint64_t encode_make_writable_time_ns_total = 0;
        std::uint64_t encode_make_writable_time_ns_max = 0;
        std::uint64_t encode_copy_time_ns_total = 0;
        std::uint64_t encode_copy_time_ns_max = 0;
        std::uint64_t encode_send_time_ns_total = 0;
        std::uint64_t encode_send_time_ns_max = 0;
        std::uint64_t encode_receive_time_ns_total = 0;
        std::uint64_t encode_receive_time_ns_max = 0;
        std::uint64_t encode_packet_write_time_ns_total = 0;
        std::uint64_t encode_packet_write_time_ns_max = 0;
        std::uint64_t emitted_packets = 0;
        std::uint64_t finalize_time_ns = 0;
        std::size_t max_queued_frames = 0;
        std::size_t max_queued_bytes = 0;
        std::size_t queue_frame_limit = 0;
        std::size_t queue_byte_limit = 0;
    };

    enum class AlphaMaskVideoWriterFrameDisposition
    {
        Queued,
        RepeatedPrevious,
    };

    class AlphaMaskVideoWriter
    {
    public:
        AlphaMaskVideoWriter() noexcept = default;
        ~AlphaMaskVideoWriter() noexcept;

        AlphaMaskVideoWriter(const AlphaMaskVideoWriter &) = delete;
        AlphaMaskVideoWriter &operator=(const AlphaMaskVideoWriter &) = delete;

        [[nodiscard]] bool open(const AlphaMaskVideoWriterConfig &config,
                                std::string *error_message = nullptr) noexcept;
        [[nodiscard]] bool write_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                                       std::string *error_message = nullptr,
                                       AlphaMaskVideoWriterFrameDisposition *disposition = nullptr) noexcept;
        [[nodiscard]] bool close(std::string *error_message = nullptr) noexcept;
        [[nodiscard]] bool close(std::string *error_message,
                                 AlphaMaskVideoWriterStats *stats) noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] const std::filesystem::path &path() const noexcept;

    private:
        struct Impl;
        Impl *impl_ = nullptr;
    };

    [[nodiscard]] bool alpha_mask_video_writer_runtime_available(std::string *reason = nullptr) noexcept;
    [[nodiscard]] std::size_t alpha_mask_writer_queue_frame_limit(std::uint32_t fps_num,
                                                                  std::uint32_t fps_den) noexcept;
    [[nodiscard]] std::size_t alpha_mask_writer_queue_byte_limit(std::uint32_t width,
                                                                  std::uint32_t height,
                                                                  std::uint32_t fps_num,
                                                                  std::uint32_t fps_den) noexcept;

} // namespace alpha_recorder::obs
