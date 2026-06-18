#include "alpha_recorder/export_worker.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace alpha_recorder::obs
{
    namespace
    {
        constexpr std::size_t kMinimumQueuedMaskFrames = 16U;
        constexpr std::size_t kMaximumQueuedMaskFrames = 120U;
        constexpr std::size_t kMinimumQueuedMaskBytes = 192U * 1024U * 1024U;
        constexpr std::size_t kMaximumQueuedMaskBytes = std::size_t{2048U} * 1024U * 1024U;

        bool set_error(std::string *error_message, std::string message)
        {
            if (error_message != nullptr)
            {
                *error_message = std::move(message);
            }

            return false;
        }

        std::string av_error_message(int error_code)
        {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
            av_strerror(error_code, buffer.data(), buffer.size());
            return std::string{buffer.data()};
        }

#ifdef _WIN32
        std::string path_to_utf8(const std::filesystem::path &path)
        {
            return path.u8string();
        }
#else
        std::string path_to_utf8(const std::filesystem::path &path)
        {
            return path.string();
        }
#endif

        bool ensure_parent_directory(const std::filesystem::path &path)
        {
            const std::filesystem::path parent_path = path.parent_path();
            if (parent_path.empty())
            {
                return true;
            }

            std::error_code error;
            std::filesystem::create_directories(parent_path, error);
            return !error;
        }

        std::size_t mask_frame_bytes(std::uint32_t width, std::uint32_t height) noexcept
        {
            if (width == 0U || height == 0U)
            {
                return 0U;
            }

            const std::size_t row_bytes = static_cast<std::size_t>(width);
            const std::size_t rows = static_cast<std::size_t>(height);
            if (row_bytes > std::numeric_limits<std::size_t>::max() / rows)
            {
                return std::numeric_limits<std::size_t>::max();
            }

            return row_bytes * rows;
        }

        std::size_t queued_mask_frame_limit_for_rate(std::uint32_t fps_num, std::uint32_t fps_den) noexcept
        {
            if (fps_num == 0U || fps_den == 0U)
            {
                return kMinimumQueuedMaskFrames;
            }

            const std::uint64_t rounded_fps =
                (static_cast<std::uint64_t>(fps_num) + static_cast<std::uint64_t>(fps_den) - 1U) /
                static_cast<std::uint64_t>(fps_den);
            return std::clamp(static_cast<std::size_t>(rounded_fps), kMinimumQueuedMaskFrames,
                              kMaximumQueuedMaskFrames);
        }

        std::size_t queued_mask_byte_limit_for_dimensions(std::uint32_t width,
                                                          std::uint32_t height,
                                                          std::uint32_t fps_num,
                                                          std::uint32_t fps_den) noexcept
        {
            const std::size_t frame_bytes = mask_frame_bytes(width, height);
            if (frame_bytes == 0U)
            {
                return kMinimumQueuedMaskBytes;
            }

            const std::size_t frame_limit = queued_mask_frame_limit_for_rate(fps_num, fps_den);
            const std::size_t target_bytes =
                frame_bytes > std::numeric_limits<std::size_t>::max() / frame_limit
                    ? std::numeric_limits<std::size_t>::max()
                    : frame_bytes * frame_limit;

            return std::clamp(target_bytes, kMinimumQueuedMaskBytes, kMaximumQueuedMaskBytes);
        }

        const AVCodec *select_output_encoder(FinalizationFormat format)
        {
            switch (format)
            {
            case FinalizationFormat::MaskPngMov:
                return avcodec_find_encoder(AV_CODEC_ID_PNG);

            case FinalizationFormat::MaskHevcNvenc:
                return avcodec_find_encoder_by_name("hevc_nvenc");

            case FinalizationFormat::MaskHevcAmf:
                return avcodec_find_encoder_by_name("hevc_amf");
            }

            return nullptr;
        }

        AlphaMaskVideoWriterConfig capability_probe_config(FinalizationFormat format)
        {
            AlphaMaskVideoWriterConfig config{};
            config.finalization_format = format;
            config.width = 1920U;
            config.height = 1080U;
            config.fps_num = 30U;
            config.fps_den = 1U;
            return config;
        }

        bool validate_dimensions(std::uint32_t width, std::uint32_t height, std::string *error_message)
        {
            if (width == 0U || height == 0U || width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            {
                return set_error(error_message, "Alpha Recorder received invalid mask video dimensions.");
            }

            return true;
        }

        const char *nvenc_preset_value(HevcEncoderPreset preset) noexcept
        {
            switch (preset)
            {
            case HevcEncoderPreset::NvencLossless:
                return "lossless";
            case HevcEncoderPreset::NvencP1:
                return "p1";
            case HevcEncoderPreset::NvencP2:
                return "p2";
            case HevcEncoderPreset::NvencP3:
                return "p3";
            case HevcEncoderPreset::NvencP4:
                return "p4";
            case HevcEncoderPreset::NvencP5:
                return "p5";
            case HevcEncoderPreset::NvencP6:
                return "p6";
            case HevcEncoderPreset::NvencP7:
                return "p7";
            case HevcEncoderPreset::AmfSpeed:
            case HevcEncoderPreset::AmfBalanced:
            case HevcEncoderPreset::AmfQuality:
                break;
            }

            return "p3";
        }

        const char *amf_quality_value(HevcEncoderPreset preset) noexcept
        {
            switch (preset)
            {
            case HevcEncoderPreset::AmfSpeed:
                return "speed";
            case HevcEncoderPreset::AmfBalanced:
                return "balanced";
            case HevcEncoderPreset::AmfQuality:
                return "quality";
            case HevcEncoderPreset::NvencLossless:
            case HevcEncoderPreset::NvencP1:
            case HevcEncoderPreset::NvencP2:
            case HevcEncoderPreset::NvencP3:
            case HevcEncoderPreset::NvencP4:
            case HevcEncoderPreset::NvencP5:
            case HevcEncoderPreset::NvencP6:
            case HevcEncoderPreset::NvencP7:
                break;
            }

            return "balanced";
        }

        std::uint32_t profile_default_cq(HevcQualityProfile profile, std::uint32_t requested_cq) noexcept
        {
            if (requested_cq <= 51U)
            {
                return requested_cq;
            }

            switch (profile)
            {
            case HevcQualityProfile::Lossless:
                return 0U;
            case HevcQualityProfile::HighQuality:
                return 19U;
            case HevcQualityProfile::Balanced:
                return 23U;
            case HevcQualityProfile::Fast:
                return 28U;
            }

            return 19U;
        }

        int configured_gop_size(const AlphaMaskVideoWriterConfig &config) noexcept
        {
            const std::uint32_t default_gop = config.fps_num == 0U ? 30U : config.fps_num;
            const std::uint32_t requested_gop = clamp_hevc_gop_size(config.hevc_encoder.gop_size);
            return static_cast<int>(requested_gop == 0U ? default_gop : requested_gop);
        }

        int configured_b_frames(const AlphaMaskVideoWriterConfig &config) noexcept
        {
            if (config.hevc_encoder.quality_profile == HevcQualityProfile::Lossless)
            {
                return 0;
            }

            return static_cast<int>(clamp_hevc_b_frames(config.hevc_encoder.b_frames));
        }

        int hevc_nvenc_split_encode_value(HevcNvencSplitEncodeMode mode) noexcept
        {
            switch (mode)
            {
            case HevcNvencSplitEncodeMode::Auto:
                return 0;
            case HevcNvencSplitEncodeMode::Disabled:
                return 15;
            case HevcNvencSplitEncodeMode::Forced:
                return 1;
            case HevcNvencSplitEncodeMode::TwoWay:
                return 2;
            case HevcNvencSplitEncodeMode::ThreeWay:
                return 3;
            }

            return 0;
        }

        bool set_encoder_int_option(AVCodecContext &encoder,
                                    const char *name,
                                    std::int64_t value,
                                    bool allow_missing,
                                    std::string *error_message)
        {
            const int ret = av_opt_set_int(encoder.priv_data, name, value, 0);
            if (ret == AVERROR_OPTION_NOT_FOUND && allow_missing)
            {
                return true;
            }
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not configure NVENC option '"} +
                                                    name + "': " + av_error_message(ret));
            }

            return true;
        }

        bool configure_encoder(AVCodecContext &encoder, const AVCodec &codec, const AlphaMaskVideoWriterConfig &config,
                               std::string *error_message)
        {
            (void)codec;
            encoder.codec_type = AVMEDIA_TYPE_VIDEO;
            encoder.codec_id = codec.id;
            encoder.width = static_cast<int>(config.width);
            encoder.height = static_cast<int>(config.height);
            encoder.time_base = AVRational{static_cast<int>(config.fps_den == 0U ? 1U : config.fps_den),
                                           static_cast<int>(config.fps_num == 0U ? 30U : config.fps_num)};
            encoder.framerate = AVRational{static_cast<int>(config.fps_num == 0U ? 30U : config.fps_num),
                                           static_cast<int>(config.fps_den == 0U ? 1U : config.fps_den)};
            encoder.max_b_frames = 0;
            encoder.color_range = AVCOL_RANGE_JPEG;
            encoder.color_primaries = AVCOL_PRI_BT709;
            encoder.color_trc = AVCOL_TRC_BT709;
            encoder.colorspace = AVCOL_SPC_BT709;
            encoder.thread_count = 0;

            switch (config.finalization_format)
            {
            case FinalizationFormat::MaskPngMov:
                encoder.gop_size = 1;
                encoder.pix_fmt = AV_PIX_FMT_GRAY8;
                (void)av_opt_set_int(encoder.priv_data, "compression_level", 0, 0);
                return true;

            case FinalizationFormat::MaskHevcNvenc:
                encoder.gop_size = configured_gop_size(config);
                encoder.max_b_frames = configured_b_frames(config);
                encoder.pix_fmt = AV_PIX_FMT_YUV420P;
                if (config.hevc_encoder.nvenc_gpu_index >= 0 &&
                    !set_encoder_int_option(encoder, "gpu", config.hevc_encoder.nvenc_gpu_index, false, error_message))
                {
                    return false;
                }
                if (config.hevc_encoder.nvenc_split_encode != HevcNvencSplitEncodeMode::Auto &&
                    !set_encoder_int_option(encoder, "split_encode_mode",
                                            hevc_nvenc_split_encode_value(config.hevc_encoder.nvenc_split_encode),
                                            false, error_message))
                {
                    return false;
                }
                if (config.hevc_encoder.quality_profile == HevcQualityProfile::Lossless)
                {
                    (void)av_opt_set(encoder.priv_data, "preset", "lossless", 0);
                    (void)av_opt_set(encoder.priv_data, "tune", "lossless", 0);
                    (void)av_opt_set(encoder.priv_data, "rc", "constqp", 0);
                    (void)av_opt_set_int(encoder.priv_data, "qp", 0, 0);
                    return true;
                }

                (void)av_opt_set(encoder.priv_data, "preset", nvenc_preset_value(config.hevc_encoder.preset), 0);
                (void)av_opt_set(encoder.priv_data, "tune", hevc_nvenc_tune_config_value(config.hevc_encoder.nvenc_tune).data(), 0);
                (void)av_opt_set(encoder.priv_data, "rc", "vbr", 0);
                (void)av_opt_set_int(encoder.priv_data, "cq", profile_default_cq(config.hevc_encoder.quality_profile,
                                                                                  config.hevc_encoder.quality_cq),
                                     0);
                if (const std::uint32_t lookahead = clamp_hevc_lookahead(config.hevc_encoder.lookahead); lookahead > 0U)
                {
                    (void)av_opt_set_int(encoder.priv_data, "rc-lookahead", lookahead, 0);
                }
                if (config.hevc_encoder.adaptive_quantization)
                {
                    (void)av_opt_set_int(encoder.priv_data, "spatial-aq", 1, 0);
                    (void)av_opt_set_int(encoder.priv_data, "temporal-aq", 1, 0);
                }
                return true;

            case FinalizationFormat::MaskHevcAmf:
                encoder.gop_size = configured_gop_size(config);
                encoder.max_b_frames = configured_b_frames(config);
                encoder.pix_fmt = AV_PIX_FMT_YUV420P;
                (void)av_opt_set(encoder.priv_data, "usage", "transcoding", 0);
                if (config.hevc_encoder.quality_profile == HevcQualityProfile::Lossless)
                {
                    (void)av_opt_set(encoder.priv_data, "quality", "quality", 0);
                    (void)av_opt_set_int(encoder.priv_data, "qp_i", 0, 0);
                    (void)av_opt_set_int(encoder.priv_data, "qp_p", 0, 0);
                    return true;
                }

                (void)av_opt_set(encoder.priv_data, "quality", amf_quality_value(config.hevc_encoder.preset), 0);
                (void)av_opt_set(encoder.priv_data, "rc", "cqp", 0);
                (void)av_opt_set_int(encoder.priv_data, "qp_i", profile_default_cq(config.hevc_encoder.quality_profile,
                                                                                   config.hevc_encoder.quality_cq),
                                     0);
                (void)av_opt_set_int(encoder.priv_data, "qp_p", profile_default_cq(config.hevc_encoder.quality_profile,
                                                                                   config.hevc_encoder.quality_cq),
                                     0);
                if (const std::uint32_t lookahead = clamp_hevc_lookahead(config.hevc_encoder.lookahead); lookahead > 0U)
                {
                    (void)av_opt_set(encoder.priv_data, "preanalysis", "true", 0);
                    (void)av_opt_set_int(encoder.priv_data, "pa_lookahead_buffer_depth", lookahead, 0);
                }
                if (config.hevc_encoder.adaptive_quantization)
                {
                    (void)av_opt_set(encoder.priv_data, "vbaq", "true", 0);
                }
                return true;
            }

            return set_error(error_message, "Alpha Recorder received an unsupported mask format.");
        }

        void fill_chroma_plane(std::uint8_t *data, int linesize, int width, int height, std::uint8_t value)
        {
            for (int row = 0; row < height; ++row)
            {
                std::uint8_t *const dest = data + (static_cast<std::size_t>(row) * static_cast<std::size_t>(linesize));
                for (int column = 0; column < width; ++column)
                {
                    dest[column] = value;
                }
            }
        }

        bool copy_alpha_to_frame(AVFrame &frame, const std::uint8_t *alpha, std::uint32_t stride, std::string *error_message)
        {
            if (alpha == nullptr || stride < static_cast<std::uint32_t>(frame.width))
            {
                return set_error(error_message, "Alpha Recorder received an invalid alpha mask frame.");
            }

            const AVPixelFormat format = static_cast<AVPixelFormat>(frame.format);
            if (format == AV_PIX_FMT_GRAY8)
            {
                for (int row = 0; row < frame.height; ++row)
                {
                    const std::uint8_t *const src = alpha + (static_cast<std::size_t>(row) * static_cast<std::size_t>(stride));
                    std::uint8_t *const dest = frame.data[0] + (static_cast<std::size_t>(row) * static_cast<std::size_t>(frame.linesize[0]));
                    std::copy(src, src + frame.width, dest);
                }
                return true;
            }

            if (format == AV_PIX_FMT_YUV420P)
            {
                for (int row = 0; row < frame.height; ++row)
                {
                    const std::uint8_t *const src = alpha + (static_cast<std::size_t>(row) * static_cast<std::size_t>(stride));
                    std::uint8_t *const dest = frame.data[0] + (static_cast<std::size_t>(row) * static_cast<std::size_t>(frame.linesize[0]));
                    for (int column = 0; column < frame.width; ++column)
                    {
                        dest[column] = src[column];
                    }
                }

                fill_chroma_plane(frame.data[1], frame.linesize[1], (frame.width + 1) / 2, (frame.height + 1) / 2, 128U);
                fill_chroma_plane(frame.data[2], frame.linesize[2], (frame.width + 1) / 2, (frame.height + 1) / 2, 128U);
                return true;
            }

            return set_error(error_message, "Alpha Recorder configured an unsupported mask pixel format.");
        }

        [[nodiscard]] std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start,
                                               std::chrono::steady_clock::time_point end) noexcept
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        }

    } // namespace

    struct AlphaMaskVideoWriter::Impl
    {
        struct QueuedFrame
        {
            std::shared_ptr<const std::vector<std::uint8_t>> alpha{};
            std::uint32_t stride = 0;
            std::uint64_t repeat_count = 0;
        };

        std::filesystem::path output_path{};
        AVFormatContext *format = nullptr;
        AVCodecContext *encoder = nullptr;
        AVStream *video_stream = nullptr;
        AVFrame *frame = nullptr;
        std::uint64_t frame_count = 0;
        bool header_written = false;
        bool accepting_frames = false;
        bool stop_requested = false;
        bool worker_failed = false;
        std::size_t max_queued_frames = kMinimumQueuedMaskFrames;
        std::size_t max_queued_bytes = kMinimumQueuedMaskBytes;
        std::size_t queued_output_frames = 0;
        std::size_t queued_actual_frames = 0;
        std::size_t queued_bytes = 0;
        bool has_accepted_reference_frame = false;
        std::shared_ptr<const std::vector<std::uint8_t>> repeat_reference_alpha{};
        std::uint32_t repeat_reference_stride = 0;
        std::string worker_error{};
        std::queue<QueuedFrame> queued_frames{};
        AlphaMaskVideoWriterStats stats{};
        std::mutex mutex{};
        std::condition_variable condition{};
        std::thread worker{};

        ~Impl()
        {
            stop_worker(nullptr);
            av_frame_free(&frame);
            avcodec_free_context(&encoder);
            if (format != nullptr)
            {
                if ((format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr)
                {
                    avio_closep(&format->pb);
                }
                avformat_free_context(format);
            }
        }

        bool encode_frame(const std::uint8_t *alpha, std::uint32_t stride, std::string *error_message) noexcept
        {
            const auto encode_start = std::chrono::steady_clock::now();
            std::uint64_t make_writable_ns = 0;
            std::uint64_t copy_ns = 0;
            std::uint64_t send_ns = 0;
            std::uint64_t receive_ns_total = 0;
            std::uint64_t receive_ns_max = 0;
            std::uint64_t packet_write_ns_total = 0;
            std::uint64_t packet_write_ns_max = 0;
            std::uint64_t emitted_packets = 0;
            if (encoder == nullptr || frame == nullptr)
            {
                return set_error(error_message, "Alpha Recorder mask video writer is not open.");
            }

            const auto make_writable_start = std::chrono::steady_clock::now();
            if (av_frame_make_writable(frame) < 0)
            {
                return set_error(error_message, "Alpha Recorder could not make the mask video frame writable.");
            }
            make_writable_ns = elapsed_ns(make_writable_start, std::chrono::steady_clock::now());

            const auto copy_start = std::chrono::steady_clock::now();
            if (!copy_alpha_to_frame(*frame, alpha, stride, error_message))
            {
                return false;
            }
            copy_ns = elapsed_ns(copy_start, std::chrono::steady_clock::now());

            if (frame_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                return set_error(error_message, "Alpha Recorder mask video frame count overflowed.");
            }

            frame->pts = static_cast<std::int64_t>(frame_count);
            const auto send_start = std::chrono::steady_clock::now();
            int ret = avcodec_send_frame(encoder, frame);
            send_ns = elapsed_ns(send_start, std::chrono::steady_clock::now());
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder failed to encode a mask video frame: "} +
                                                    av_error_message(ret));
            }

            AVPacket *packet = av_packet_alloc();
            if (packet == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not allocate a mask video packet.");
            }

            while (true)
            {
                const auto receive_start = std::chrono::steady_clock::now();
                ret = avcodec_receive_packet(encoder, packet);
                const std::uint64_t receive_ns = elapsed_ns(receive_start, std::chrono::steady_clock::now());
                receive_ns_total += receive_ns;
                receive_ns_max = std::max(receive_ns_max, receive_ns);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    av_packet_free(&packet);
                    ++frame_count;
                    const std::uint64_t encode_ns = elapsed_ns(encode_start, std::chrono::steady_clock::now());
                    std::lock_guard<std::mutex> lock(mutex);
                    ++stats.encoded_frames;
                    stats.encode_time_ns_total += encode_ns;
                    stats.encode_time_ns_max = std::max(stats.encode_time_ns_max, encode_ns);
                    stats.encode_make_writable_time_ns_total += make_writable_ns;
                    stats.encode_make_writable_time_ns_max =
                        std::max(stats.encode_make_writable_time_ns_max, make_writable_ns);
                    stats.encode_copy_time_ns_total += copy_ns;
                    stats.encode_copy_time_ns_max = std::max(stats.encode_copy_time_ns_max, copy_ns);
                    stats.encode_send_time_ns_total += send_ns;
                    stats.encode_send_time_ns_max = std::max(stats.encode_send_time_ns_max, send_ns);
                    stats.encode_receive_time_ns_total += receive_ns_total;
                    stats.encode_receive_time_ns_max = std::max(stats.encode_receive_time_ns_max, receive_ns_max);
                    stats.encode_packet_write_time_ns_total += packet_write_ns_total;
                    stats.encode_packet_write_time_ns_max =
                        std::max(stats.encode_packet_write_time_ns_max, packet_write_ns_max);
                    stats.emitted_packets += emitted_packets;
                    return true;
                }

                if (ret < 0)
                {
                    av_packet_free(&packet);
                    return set_error(error_message, std::string{"Alpha Recorder failed while retrieving a mask video packet: "} +
                                                        av_error_message(ret));
                }

                packet->stream_index = video_stream->index;
                packet->pos = -1;
                av_packet_rescale_ts(packet, encoder->time_base, video_stream->time_base);
                const auto packet_write_start = std::chrono::steady_clock::now();
                ret = av_interleaved_write_frame(format, packet);
                const std::uint64_t packet_write_ns = elapsed_ns(packet_write_start, std::chrono::steady_clock::now());
                packet_write_ns_total += packet_write_ns;
                packet_write_ns_max = std::max(packet_write_ns_max, packet_write_ns);
                ++emitted_packets;
                av_packet_unref(packet);
                if (ret < 0)
                {
                    av_packet_free(&packet);
                    return set_error(error_message, std::string{"Alpha Recorder failed while writing a mask video packet: "} +
                                                        av_error_message(ret));
                }
            }
        }

        bool drain_encoder(std::string *error_message) noexcept
        {
            const auto finalize_start = std::chrono::steady_clock::now();
            if (encoder != nullptr)
            {
                int ret = avcodec_send_frame(encoder, nullptr);
                if (ret < 0 && ret != AVERROR_EOF)
                {
                    return set_error(error_message, std::string{"Alpha Recorder failed to flush the mask video encoder: "} +
                                                        av_error_message(ret));
                }

                AVPacket *packet = av_packet_alloc();
                if (packet == nullptr)
                {
                    return set_error(error_message, "Alpha Recorder could not allocate a mask video flush packet.");
                }

                while (true)
                {
                    ret = avcodec_receive_packet(encoder, packet);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        break;
                    }

                    if (ret < 0)
                    {
                        av_packet_free(&packet);
                        return set_error(error_message, std::string{"Alpha Recorder failed while draining the mask video encoder: "} +
                                                            av_error_message(ret));
                    }

                    packet->stream_index = video_stream->index;
                    packet->pos = -1;
                    av_packet_rescale_ts(packet, encoder->time_base, video_stream->time_base);
                    ret = av_interleaved_write_frame(format, packet);
                    av_packet_unref(packet);
                    if (ret < 0)
                    {
                        av_packet_free(&packet);
                        return set_error(error_message, std::string{"Alpha Recorder failed while writing a flushed mask video packet: "} +
                                                            av_error_message(ret));
                    }
                }
                av_packet_free(&packet);
            }

            if (format != nullptr && header_written && av_write_trailer(format) < 0)
            {
                return set_error(error_message, "Alpha Recorder failed to finalize the mask video.");
            }

            header_written = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                stats.finalize_time_ns += elapsed_ns(finalize_start, std::chrono::steady_clock::now());
            }
            return true;
        }

        void run_worker() noexcept
        {
            while (true)
            {
                QueuedFrame queued_frame{};
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [this]() {
                        return stop_requested || worker_failed || !queued_frames.empty();
                    });

                    if ((stop_requested || worker_failed) && queued_frames.empty())
                    {
                        break;
                    }

                    queued_frame = std::move(queued_frames.front());
                    queued_frames.pop();
                    if (queued_frame.alpha)
                    {
                        queued_bytes -= queued_frame.alpha->size();
                        --queued_actual_frames;
                        --queued_output_frames;
                    }
                    else
                    {
                        queued_output_frames -= static_cast<std::size_t>(
                            std::min<std::uint64_t>(queued_frame.repeat_count, queued_output_frames));
                    }
                }

                std::string error_message;
                bool encoded = true;
                if (queued_frame.alpha)
                {
                    encoded = encode_frame(queued_frame.alpha->data(), queued_frame.stride, &error_message);
                    if (encoded)
                    {
                        repeat_reference_alpha = queued_frame.alpha;
                        repeat_reference_stride = queued_frame.stride;
                    }
                }
                else if (queued_frame.repeat_count > 0U && repeat_reference_alpha)
                {
                    for (std::uint64_t index = 0; index < queued_frame.repeat_count; ++index)
                    {
                        if (!encode_frame(repeat_reference_alpha->data(), repeat_reference_stride, &error_message))
                        {
                            encoded = false;
                            break;
                        }
                    }
                }
                else
                {
                    encoded = false;
                    error_message = "Alpha Recorder could not repeat a previous alpha mask frame.";
                }

                if (!encoded)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    worker_failed = true;
                    accepting_frames = false;
                    worker_error = error_message.empty() ? "Alpha Recorder failed to encode an alpha mask frame." : std::move(error_message);
                    queued_bytes = 0;
                    queued_output_frames = 0;
                    queued_actual_frames = 0;
                    queued_frames = {};
                    condition.notify_all();
                    break;
                }
            }

            std::string finalize_error;
            const bool finalized = drain_encoder(&finalize_error);
            std::lock_guard<std::mutex> lock(mutex);
            accepting_frames = false;
            if (!finalized && !worker_failed)
            {
                worker_failed = true;
                worker_error = finalize_error.empty() ? "Alpha Recorder failed to finalize the mask video." : std::move(finalize_error);
            }
            condition.notify_all();
        }

        void start_worker()
        {
            accepting_frames = true;
            stop_requested = false;
            worker_failed = false;
            worker_error.clear();
            worker = std::thread{[this]() { run_worker(); }};
        }

        void update_queue_stats_after_enqueue(std::uint64_t enqueue_ns, std::size_t alpha_bytes) noexcept
        {
            ++stats.enqueued_frames;
            stats.queued_bytes_total += alpha_bytes;
            stats.enqueue_time_ns_total += enqueue_ns;
            stats.enqueue_time_ns_max = std::max(stats.enqueue_time_ns_max, enqueue_ns);
            stats.max_queued_frames = std::max(stats.max_queued_frames, queued_output_frames);
            stats.max_queued_bytes = std::max(stats.max_queued_bytes, queued_bytes);
        }

        void enqueue_repeat_frame_locked() noexcept
        {
            if (!queued_frames.empty() && !queued_frames.back().alpha && queued_frames.back().repeat_count > 0U)
            {
                ++queued_frames.back().repeat_count;
            }
            else
            {
                queued_frames.push(QueuedFrame{nullptr, 0U, 1U});
            }
            ++queued_output_frames;
            ++stats.overflow_repeated_frames;
        }

        bool enqueue_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                           std::uint32_t stride,
                           std::string *error_message,
                           AlphaMaskVideoWriterFrameDisposition *disposition) noexcept
        {
            if (disposition != nullptr)
            {
                *disposition = AlphaMaskVideoWriterFrameDisposition::Queued;
            }
            if (!alpha)
            {
                return set_error(error_message, "Alpha Recorder received an invalid alpha mask frame.");
            }

            std::lock_guard<std::mutex> lock(mutex);
            if (worker_failed)
            {
                return set_error(error_message, worker_error.empty() ? "Alpha Recorder failed to encode the mask video." : worker_error);
            }

            if (!accepting_frames || stop_requested)
            {
                return set_error(error_message, "Alpha Recorder mask video writer is closing.");
            }

            const auto enqueue_start = std::chrono::steady_clock::now();
            const std::size_t alpha_bytes = alpha->size();
            const bool can_repeat_previous = has_accepted_reference_frame;
            const bool full_alpha_queue_full =
                queued_actual_frames >= max_queued_frames ||
                alpha_bytes > max_queued_bytes ||
                queued_bytes > max_queued_bytes - alpha_bytes;
            if (full_alpha_queue_full && can_repeat_previous)
            {
                enqueue_repeat_frame_locked();
                if (disposition != nullptr)
                {
                    *disposition = AlphaMaskVideoWriterFrameDisposition::RepeatedPrevious;
                }
                const std::uint64_t enqueue_ns = elapsed_ns(enqueue_start, std::chrono::steady_clock::now());
                update_queue_stats_after_enqueue(enqueue_ns, 0U);
                condition.notify_one();
                return true;
            }

            queued_bytes += alpha_bytes;
            ++queued_actual_frames;
            ++queued_output_frames;
            has_accepted_reference_frame = true;
            queued_frames.push(QueuedFrame{std::move(alpha), stride, 0U});
            const std::uint64_t enqueue_ns = elapsed_ns(enqueue_start, std::chrono::steady_clock::now());
            update_queue_stats_after_enqueue(enqueue_ns, alpha_bytes);
            condition.notify_one();
            return true;
        }

        bool stop_worker(std::string *error_message, AlphaMaskVideoWriterStats *out_stats = nullptr) noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                accepting_frames = false;
                stop_requested = true;
                condition.notify_all();
            }

            if (worker.joinable())
            {
                worker.join();
            }

            std::lock_guard<std::mutex> lock(mutex);
            if (out_stats != nullptr)
            {
                *out_stats = stats;
            }
            if (worker_failed)
            {
                return set_error(error_message, worker_error.empty() ? "Alpha Recorder failed to write the mask video." : worker_error);
            }

            return true;
        }
    };

    AlphaMaskVideoWriter::~AlphaMaskVideoWriter() noexcept
    {
        (void)close(nullptr);
    }

    bool AlphaMaskVideoWriter::open(const AlphaMaskVideoWriterConfig &config, std::string *error_message) noexcept
    {
        (void)close(nullptr);

        try
        {
            if (config.output_path.empty())
            {
                return set_error(error_message, "Alpha Recorder received an empty mask video path.");
            }

            if (!validate_dimensions(config.width, config.height, error_message))
            {
                return false;
            }

            if (!ensure_parent_directory(config.output_path))
            {
                return set_error(error_message, std::string{"Alpha Recorder could not create the mask video directory: "} +
                                                    config.output_path.parent_path().generic_string());
            }

            const AVCodec *const encoder = select_output_encoder(config.finalization_format);
            if (encoder == nullptr)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not find "} +
                                                    std::string{finalization_format_display_name(config.finalization_format)} +
                                                    " in the bundled FFmpeg stack.");
            }

            auto *impl = new Impl{};
            impl->output_path = config.output_path;
            impl->max_queued_frames = queued_mask_frame_limit_for_rate(config.fps_num, config.fps_den);
            impl->max_queued_bytes =
                queued_mask_byte_limit_for_dimensions(config.width, config.height, config.fps_num, config.fps_den);
            impl->stats.queue_frame_limit = impl->max_queued_frames;
            impl->stats.queue_byte_limit = impl->max_queued_bytes;
            const std::string output_path_text = path_to_utf8(config.output_path);
            int ret = avformat_alloc_output_context2(&impl->format, nullptr, nullptr, output_path_text.c_str());
            if (ret < 0 || impl->format == nullptr)
            {
                delete impl;
                return set_error(error_message, std::string{"Alpha Recorder could not allocate the mask video container: "} +
                                                    av_error_message(ret));
            }

            impl->encoder = avcodec_alloc_context3(encoder);
            if (impl->encoder == nullptr)
            {
                delete impl;
                return set_error(error_message, "Alpha Recorder could not allocate the mask video encoder.");
            }

            if (!configure_encoder(*impl->encoder, *encoder, config, error_message))
            {
                delete impl;
                return false;
            }

            if ((impl->format->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            {
                impl->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            ret = avcodec_open2(impl->encoder, encoder, nullptr);
            if (ret < 0)
            {
                delete impl;
                return set_error(error_message, std::string{"Alpha Recorder could not open the mask video encoder: "} +
                                                    av_error_message(ret));
            }

            if (config.finalization_format == FinalizationFormat::MaskHevcNvenc && impl->encoder->priv_data != nullptr)
            {
                std::int64_t option_value = 0;
                if (av_opt_get_int(impl->encoder->priv_data, "split_encode_mode", 0, &option_value) >= 0)
                {
                    impl->stats.nvenc_split_encode_option_available = true;
                    impl->stats.nvenc_split_encode_option_value = option_value;
                }
                if (av_opt_get_int(impl->encoder->priv_data, "gpu", 0, &option_value) >= 0)
                {
                    impl->stats.nvenc_gpu_option_available = true;
                    impl->stats.nvenc_gpu_option_value = option_value;
                }
            }

            impl->video_stream = avformat_new_stream(impl->format, encoder);
            if (impl->video_stream == nullptr)
            {
                delete impl;
                return set_error(error_message, "Alpha Recorder could not allocate the mask video stream.");
            }

            ret = avcodec_parameters_from_context(impl->video_stream->codecpar, impl->encoder);
            if (ret < 0)
            {
                delete impl;
                return set_error(error_message, std::string{"Alpha Recorder could not configure the mask video stream: "} +
                                                    av_error_message(ret));
            }

            impl->video_stream->time_base = impl->encoder->time_base;
            impl->video_stream->codecpar->codec_tag = 0;

            if ((impl->format->oformat->flags & AVFMT_NOFILE) == 0)
            {
                ret = avio_open(&impl->format->pb, output_path_text.c_str(), AVIO_FLAG_WRITE);
                if (ret < 0)
                {
                    delete impl;
                    return set_error(error_message, std::string{"Alpha Recorder could not open the mask video file: "} +
                                                        config.output_path.generic_string() + " (" + av_error_message(ret) + ")");
                }
            }

            ret = avformat_write_header(impl->format, nullptr);
            if (ret < 0)
            {
                delete impl;
                return set_error(error_message, std::string{"Alpha Recorder could not write the mask video header: "} +
                                                    av_error_message(ret));
            }

            impl->header_written = true;
            impl->frame = av_frame_alloc();
            if (impl->frame == nullptr)
            {
                delete impl;
                return set_error(error_message, "Alpha Recorder could not allocate a mask video frame.");
            }

            impl->frame->format = impl->encoder->pix_fmt;
            impl->frame->width = impl->encoder->width;
            impl->frame->height = impl->encoder->height;
            ret = av_frame_get_buffer(impl->frame, 32);
            if (ret < 0)
            {
                delete impl;
                return set_error(error_message, std::string{"Alpha Recorder could not allocate mask video frame buffers: "} +
                                                    av_error_message(ret));
            }

            impl->start_worker();
            impl_ = impl;
            return true;
        }
        catch (const std::exception &ex)
        {
            return set_error(error_message, std::string{"Alpha Recorder failed to open the mask video writer: "} + ex.what());
        }
        catch (...)
        {
            return set_error(error_message, "Alpha Recorder failed to open the mask video writer.");
        }
    }

    bool AlphaMaskVideoWriter::write_frame(const std::uint8_t *alpha,
                                           std::uint32_t stride,
                                           std::string *error_message,
                                           AlphaMaskVideoWriterFrameDisposition *disposition) noexcept
    {
        if (impl_ == nullptr || impl_->encoder == nullptr || impl_->frame == nullptr)
        {
            return set_error(error_message, "Alpha Recorder mask video writer is not open.");
        }

        if (alpha == nullptr || stride < static_cast<std::uint32_t>(impl_->encoder->width))
        {
            return set_error(error_message, "Alpha Recorder received an invalid alpha mask frame.");
        }

        const std::size_t row_bytes = static_cast<std::size_t>(impl_->encoder->width);
        const std::size_t frame_bytes = row_bytes * static_cast<std::size_t>(impl_->encoder->height);
        if (impl_->encoder->height != 0 && frame_bytes / static_cast<std::size_t>(impl_->encoder->height) != row_bytes)
        {
            return set_error(error_message, "Alpha Recorder received invalid mask video dimensions.");
        }

        auto queued_alpha = std::make_shared<std::vector<std::uint8_t>>(frame_bytes);
        for (int row = 0; row < impl_->encoder->height; ++row)
        {
            const std::uint8_t *const source = alpha + (static_cast<std::size_t>(row) * static_cast<std::size_t>(stride));
            std::uint8_t *const dest = queued_alpha->data() + (static_cast<std::size_t>(row) * row_bytes);
            std::copy(source, source + row_bytes, dest);
        }

        return impl_->enqueue_frame(std::move(queued_alpha), static_cast<std::uint32_t>(impl_->encoder->width),
                                    error_message, disposition);
    }

    bool AlphaMaskVideoWriter::write_frame(std::vector<std::uint8_t> alpha,
                                           std::string *error_message,
                                           AlphaMaskVideoWriterFrameDisposition *disposition) noexcept
    {
        if (impl_ == nullptr || impl_->encoder == nullptr || impl_->frame == nullptr)
        {
            return set_error(error_message, "Alpha Recorder mask video writer is not open.");
        }

        const std::size_t row_bytes = static_cast<std::size_t>(impl_->encoder->width);
        const std::size_t frame_bytes = row_bytes * static_cast<std::size_t>(impl_->encoder->height);
        if (impl_->encoder->height != 0 && frame_bytes / static_cast<std::size_t>(impl_->encoder->height) != row_bytes)
        {
            return set_error(error_message, "Alpha Recorder received invalid mask video dimensions.");
        }

        if (alpha.size() != frame_bytes)
        {
            return set_error(error_message, "Alpha Recorder received an invalid alpha mask frame.");
        }

        return impl_->enqueue_frame(std::make_shared<std::vector<std::uint8_t>>(std::move(alpha)),
                                    static_cast<std::uint32_t>(impl_->encoder->width), error_message, disposition);
    }

    bool AlphaMaskVideoWriter::write_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                                           std::string *error_message,
                                           AlphaMaskVideoWriterFrameDisposition *disposition) noexcept
    {
        if (impl_ == nullptr || impl_->encoder == nullptr || impl_->frame == nullptr)
        {
            return set_error(error_message, "Alpha Recorder mask video writer is not open.");
        }

        const std::size_t row_bytes = static_cast<std::size_t>(impl_->encoder->width);
        const std::size_t frame_bytes = row_bytes * static_cast<std::size_t>(impl_->encoder->height);
        if (impl_->encoder->height != 0 && frame_bytes / static_cast<std::size_t>(impl_->encoder->height) != row_bytes)
        {
            return set_error(error_message, "Alpha Recorder received invalid mask video dimensions.");
        }

        if (!alpha || alpha->size() != frame_bytes)
        {
            return set_error(error_message, "Alpha Recorder received an invalid alpha mask frame.");
        }

        return impl_->enqueue_frame(std::move(alpha), static_cast<std::uint32_t>(impl_->encoder->width),
                                    error_message, disposition);
    }

    bool AlphaMaskVideoWriter::close(std::string *error_message) noexcept
    {
        return close(error_message, nullptr);
    }

    bool AlphaMaskVideoWriter::close(std::string *error_message, AlphaMaskVideoWriterStats *stats) noexcept
    {
        if (impl_ == nullptr)
        {
            if (stats != nullptr)
            {
                *stats = {};
            }
            return true;
        }

        Impl *impl = impl_;
        impl_ = nullptr;
        const bool success = impl->stop_worker(error_message, stats);
        delete impl;
        return success;
    }

    bool AlphaMaskVideoWriter::is_open() const noexcept
    {
        return impl_ != nullptr;
    }

    const std::filesystem::path &AlphaMaskVideoWriter::path() const noexcept
    {
        static const std::filesystem::path empty_path{};
        return impl_ == nullptr ? empty_path : impl_->output_path;
    }

    std::uint64_t AlphaMaskVideoWriter::frame_count() const noexcept
    {
        return impl_ == nullptr ? 0U : impl_->frame_count;
    }

    bool finalization_format_runtime_available(FinalizationFormat format, std::string *reason) noexcept
    {
        try
        {
            if (!finalization_format_is_supported(format))
            {
                return set_error(reason, "unsupported finalization format");
            }

            if (select_output_encoder(format) == nullptr)
            {
                return set_error(reason, std::string{finalization_format_display_name(format)} +
                                             " is not available in the bundled FFmpeg stack");
            }

            if (format == FinalizationFormat::MaskHevcNvenc || format == FinalizationFormat::MaskHevcAmf)
            {
                const AVCodec *const encoder = select_output_encoder(format);
                AVCodecContext *context = avcodec_alloc_context3(encoder);
                if (context == nullptr)
                {
                    return set_error(reason, std::string{finalization_format_display_name(format)} +
                                                 " could not allocate an FFmpeg encoder context");
                }

                std::string configure_error;
                const AlphaMaskVideoWriterConfig probe_config = capability_probe_config(format);
                bool available = configure_encoder(*context, *encoder, probe_config, &configure_error);
                if (available)
                {
                    const int ret = avcodec_open2(context, encoder, nullptr);
                    if (ret < 0)
                    {
                        available = false;
                        configure_error = std::string{finalization_format_display_name(format)} +
                                          " could not open on this system: " + av_error_message(ret);
                    }
                }

                avcodec_free_context(&context);
                if (!available)
                {
                    return set_error(reason, configure_error.empty()
                                                 ? std::string{finalization_format_display_name(format)} +
                                                       " could not open on this system"
                                                 : configure_error);
                }
            }

            if (reason != nullptr)
            {
                reason->clear();
            }
            return true;
        }
        catch (const std::exception &ex)
        {
            return set_error(reason, std::string{"finalization capability check failed: "} + ex.what());
        }
        catch (...)
        {
            return set_error(reason, "finalization capability check failed.");
        }
    }

    bool hevc_nvenc_split_encode_runtime_available(HevcNvencSplitEncodeMode mode, std::string *reason) noexcept
    {
        try
        {
            if (mode == HevcNvencSplitEncodeMode::Auto)
            {
                if (reason != nullptr)
                {
                    reason->clear();
                }
                return true;
            }

            const AVCodec *const encoder = select_output_encoder(FinalizationFormat::MaskHevcNvenc);
            if (encoder == nullptr)
            {
                return set_error(reason, "HEVC NVENC is not available in the bundled FFmpeg stack.");
            }

            AVCodecContext *context = avcodec_alloc_context3(encoder);
            if (context == nullptr)
            {
                return set_error(reason, "HEVC NVENC could not allocate an FFmpeg encoder context.");
            }

            const AVOption *const splitEncodeOption =
                context->priv_data == nullptr
                    ? nullptr
                    : av_opt_find(context->priv_data, "split_encode_mode", nullptr, 0, AV_OPT_SEARCH_CHILDREN);
            if (splitEncodeOption == nullptr)
            {
                avcodec_free_context(&context);
                return set_error(reason, "HEVC NVENC Split Encode is not available in the bundled FFmpeg stack.");
            }

            std::string configure_error;
            const bool available = set_encoder_int_option(*context, "split_encode_mode",
                                                          hevc_nvenc_split_encode_value(mode), false,
                                                          &configure_error);
            avcodec_free_context(&context);
            if (!available)
            {
                return set_error(reason, configure_error.empty()
                                             ? "HEVC NVENC Split Encode could not be configured."
                                             : configure_error);
            }

            if (reason != nullptr)
            {
                reason->clear();
            }
            return true;
        }
        catch (const std::exception &ex)
        {
            return set_error(reason, std::string{"HEVC NVENC Split Encode capability check failed: "} + ex.what());
        }
        catch (...)
        {
            return set_error(reason, "HEVC NVENC Split Encode capability check failed.");
        }
    }

    bool hevc_nvenc_encoder_settings_runtime_available(const HevcEncoderSettings &settings,
                                                       std::string *reason) noexcept
    {
        try
        {
            if (!hevc_nvenc_split_encode_runtime_available(settings.nvenc_split_encode, reason))
            {
                return false;
            }

            const AVCodec *const encoder = select_output_encoder(FinalizationFormat::MaskHevcNvenc);
            if (encoder == nullptr)
            {
                return set_error(reason, "HEVC NVENC is not available in the bundled FFmpeg stack.");
            }

            AVCodecContext *context = avcodec_alloc_context3(encoder);
            if (context == nullptr)
            {
                return set_error(reason, "HEVC NVENC could not allocate an FFmpeg encoder context.");
            }

            AlphaMaskVideoWriterConfig probe_config = capability_probe_config(FinalizationFormat::MaskHevcNvenc);
            probe_config.hevc_encoder = settings;

            std::string configure_error;
            bool available = configure_encoder(*context, *encoder, probe_config, &configure_error);
            if (available)
            {
                const int ret = avcodec_open2(context, encoder, nullptr);
                if (ret < 0)
                {
                    available = false;
                    configure_error = "HEVC NVENC could not open with the selected settings: " +
                                      av_error_message(ret);
                }
            }

            avcodec_free_context(&context);
            if (!available)
            {
                return set_error(reason, configure_error.empty()
                                             ? "HEVC NVENC could not open with the selected settings."
                                             : configure_error);
            }

            if (reason != nullptr)
            {
                reason->clear();
            }
            return true;
        }
        catch (const std::exception &ex)
        {
            return set_error(reason, std::string{"HEVC NVENC selected settings capability check failed: "} +
                                         ex.what());
        }
        catch (...)
        {
            return set_error(reason, "HEVC NVENC selected settings capability check failed.");
        }
    }

    FinalizationFormat preferred_runtime_finalization_format() noexcept
    {
        for (const FinalizationFormat format : {FinalizationFormat::MaskHevcNvenc, FinalizationFormat::MaskHevcAmf,
                                                FinalizationFormat::MaskPngMov})
        {
            if (finalization_format_runtime_available(format))
            {
                return format;
            }
        }

        return finalization_format_default();
    }

    std::size_t alpha_mask_writer_queue_frame_limit(std::uint32_t fps_num, std::uint32_t fps_den) noexcept
    {
        return queued_mask_frame_limit_for_rate(fps_num, fps_den);
    }

    std::size_t alpha_mask_writer_queue_byte_limit(std::uint32_t width,
                                                   std::uint32_t height,
                                                   std::uint32_t fps_num,
                                                   std::uint32_t fps_den) noexcept
    {
        return queued_mask_byte_limit_for_dimensions(width, height, fps_num, fps_den);
    }

} // namespace alpha_recorder::obs
