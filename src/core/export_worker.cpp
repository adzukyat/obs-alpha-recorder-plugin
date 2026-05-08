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
        constexpr std::size_t kMaxQueuedMaskFrames = 16U;
        constexpr std::size_t kMaxQueuedMaskBytes = 192U * 1024U * 1024U;

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
            case HevcEncoderPreset::Fast:
                return "p2";
            case HevcEncoderPreset::Balanced:
                return "p3";
            case HevcEncoderPreset::Quality:
                return "p5";
            }

            return "p3";
        }

        const char *amf_quality_value(HevcEncoderPreset preset) noexcept
        {
            switch (preset)
            {
            case HevcEncoderPreset::Fast:
                return "speed";
            case HevcEncoderPreset::Balanced:
                return "balanced";
            case HevcEncoderPreset::Quality:
                return "quality";
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
                if (config.hevc_encoder.quality_profile == HevcQualityProfile::Lossless)
                {
                    (void)av_opt_set(encoder.priv_data, "preset", "lossless", 0);
                    (void)av_opt_set(encoder.priv_data, "tune", "lossless", 0);
                    (void)av_opt_set(encoder.priv_data, "rc", "constqp", 0);
                    (void)av_opt_set_int(encoder.priv_data, "qp", 0, 0);
                    return true;
                }

                (void)av_opt_set(encoder.priv_data, "preset", nvenc_preset_value(config.hevc_encoder.preset), 0);
                (void)av_opt_set(encoder.priv_data, "tune", config.hevc_encoder.quality_profile == HevcQualityProfile::Fast ? "ll" : "hq", 0);
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
        std::size_t queued_bytes = 0;
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
            if (encoder == nullptr || frame == nullptr)
            {
                return set_error(error_message, "Alpha Recorder mask video writer is not open.");
            }

            if (av_frame_make_writable(frame) < 0)
            {
                return set_error(error_message, "Alpha Recorder could not make the mask video frame writable.");
            }

            if (!copy_alpha_to_frame(*frame, alpha, stride, error_message))
            {
                return false;
            }

            if (frame_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                return set_error(error_message, "Alpha Recorder mask video frame count overflowed.");
            }

            frame->pts = static_cast<std::int64_t>(frame_count);
            int ret = avcodec_send_frame(encoder, frame);
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
                ret = avcodec_receive_packet(encoder, packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    av_packet_free(&packet);
                    ++frame_count;
                    const std::uint64_t encode_ns = elapsed_ns(encode_start, std::chrono::steady_clock::now());
                    std::lock_guard<std::mutex> lock(mutex);
                    ++stats.encoded_frames;
                    stats.encode_time_ns_total += encode_ns;
                    stats.encode_time_ns_max = std::max(stats.encode_time_ns_max, encode_ns);
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
                ret = av_interleaved_write_frame(format, packet);
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
                    queued_bytes -= queued_frame.alpha ? queued_frame.alpha->size() : 0U;
                }

                std::string error_message;
                if (!queued_frame.alpha ||
                    !encode_frame(queued_frame.alpha->data(), queued_frame.stride, &error_message))
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    worker_failed = true;
                    accepting_frames = false;
                    worker_error = error_message.empty() ? "Alpha Recorder failed to encode an alpha mask frame." : std::move(error_message);
                    queued_bytes = 0;
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

        bool enqueue_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                           std::uint32_t stride,
                           std::string *error_message) noexcept
        {
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

            if (queued_frames.size() >= kMaxQueuedMaskFrames ||
                alpha->size() > kMaxQueuedMaskBytes ||
                queued_bytes > kMaxQueuedMaskBytes - alpha->size())
            {
                accepting_frames = false;
                stop_requested = true;
                worker_failed = true;
                worker_error = "Alpha Recorder alpha mask encoder could not keep up; aborting alpha mask generation to protect the main OBS recording.";
                while (!queued_frames.empty())
                {
                    queued_frames.pop();
                }
                queued_bytes = 0;
                condition.notify_all();
                return set_error(error_message, worker_error);
            }

            const auto enqueue_start = std::chrono::steady_clock::now();
            const std::size_t alpha_bytes = alpha->size();
            queued_bytes += alpha_bytes;
            queued_frames.push(QueuedFrame{std::move(alpha), stride});
            const std::uint64_t enqueue_ns = elapsed_ns(enqueue_start, std::chrono::steady_clock::now());
            ++stats.enqueued_frames;
            stats.queued_bytes_total += alpha_bytes;
            stats.enqueue_time_ns_total += enqueue_ns;
            stats.enqueue_time_ns_max = std::max(stats.enqueue_time_ns_max, enqueue_ns);
            stats.max_queued_frames = std::max(stats.max_queued_frames, queued_frames.size());
            stats.max_queued_bytes = std::max(stats.max_queued_bytes, queued_bytes);
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

    bool AlphaMaskVideoWriter::write_frame(const std::uint8_t *alpha, std::uint32_t stride, std::string *error_message) noexcept
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

        return impl_->enqueue_frame(std::move(queued_alpha), static_cast<std::uint32_t>(impl_->encoder->width), error_message);
    }

    bool AlphaMaskVideoWriter::write_frame(std::vector<std::uint8_t> alpha, std::string *error_message) noexcept
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
                                    static_cast<std::uint32_t>(impl_->encoder->width), error_message);
    }

    bool AlphaMaskVideoWriter::write_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha, std::string *error_message) noexcept
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

        return impl_->enqueue_frame(std::move(alpha), static_cast<std::uint32_t>(impl_->encoder->width), error_message);
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

} // namespace alpha_recorder::obs
