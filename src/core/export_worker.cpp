#include "alpha_recorder/export_worker.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

        bool validate_dimensions(std::uint32_t width, std::uint32_t height, std::string *error_message)
        {
            if (width == 0U || height == 0U || width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            {
                return set_error(error_message, "Alpha Recorder received invalid mask video dimensions.");
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

            switch (config.finalization_format)
            {
            case FinalizationFormat::MaskPngMov:
                encoder.gop_size = 1;
                encoder.pix_fmt = AV_PIX_FMT_GRAY8;
                (void)av_opt_set_int(encoder.priv_data, "compression_level", 1, 0);
                return true;

            case FinalizationFormat::MaskHevcNvenc:
                encoder.gop_size = static_cast<int>(config.fps_num == 0U ? 30U : config.fps_num);
                encoder.pix_fmt = AV_PIX_FMT_YUV420P;
                (void)av_opt_set(encoder.priv_data, "preset", "lossless", 0);
                (void)av_opt_set(encoder.priv_data, "tune", "lossless", 0);
                (void)av_opt_set(encoder.priv_data, "rc", "constqp", 0);
                (void)av_opt_set_int(encoder.priv_data, "qp", 0, 0);
                return true;

            case FinalizationFormat::MaskHevcAmf:
                encoder.gop_size = static_cast<int>(config.fps_num == 0U ? 30U : config.fps_num);
                encoder.pix_fmt = AV_PIX_FMT_YUV420P;
                (void)av_opt_set(encoder.priv_data, "usage", "transcoding", 0);
                (void)av_opt_set(encoder.priv_data, "quality", "quality", 0);
                (void)av_opt_set_int(encoder.priv_data, "qp_i", 0, 0);
                (void)av_opt_set_int(encoder.priv_data, "qp_p", 0, 0);
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

    } // namespace

    struct AlphaMaskVideoWriter::Impl
    {
        std::filesystem::path output_path{};
        AVFormatContext *format = nullptr;
        AVCodecContext *encoder = nullptr;
        AVStream *video_stream = nullptr;
        AVFrame *frame = nullptr;
        std::uint64_t frame_count = 0;
        bool header_written = false;

        ~Impl()
        {
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

        if (av_frame_make_writable(impl_->frame) < 0)
        {
            return set_error(error_message, "Alpha Recorder could not make the mask video frame writable.");
        }

        if (!copy_alpha_to_frame(*impl_->frame, alpha, stride, error_message))
        {
            return false;
        }

        if (impl_->frame_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return set_error(error_message, "Alpha Recorder mask video frame count overflowed.");
        }

        impl_->frame->pts = static_cast<std::int64_t>(impl_->frame_count);
        int ret = avcodec_send_frame(impl_->encoder, impl_->frame);
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
            ret = avcodec_receive_packet(impl_->encoder, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                av_packet_free(&packet);
                ++impl_->frame_count;
                return true;
            }

            if (ret < 0)
            {
                av_packet_free(&packet);
                return set_error(error_message, std::string{"Alpha Recorder failed while retrieving a mask video packet: "} +
                                                    av_error_message(ret));
            }

            packet->stream_index = impl_->video_stream->index;
            packet->pos = -1;
            av_packet_rescale_ts(packet, impl_->encoder->time_base, impl_->video_stream->time_base);
            ret = av_interleaved_write_frame(impl_->format, packet);
            av_packet_unref(packet);
            if (ret < 0)
            {
                av_packet_free(&packet);
                return set_error(error_message, std::string{"Alpha Recorder failed while writing a mask video packet: "} +
                                                    av_error_message(ret));
            }
        }
    }

    bool AlphaMaskVideoWriter::close(std::string *error_message) noexcept
    {
        if (impl_ == nullptr)
        {
            return true;
        }

        Impl *impl = impl_;
        impl_ = nullptr;
        bool success = true;

        if (impl->encoder != nullptr)
        {
            int ret = avcodec_send_frame(impl->encoder, nullptr);
            if (ret < 0 && ret != AVERROR_EOF)
            {
                success = set_error(error_message, std::string{"Alpha Recorder failed to flush the mask video encoder: "} +
                                                       av_error_message(ret));
            }

            AVPacket *packet = av_packet_alloc();
            if (packet == nullptr)
            {
                success = set_error(error_message, "Alpha Recorder could not allocate a mask video flush packet.");
            }
            else
            {
                while (success)
                {
                    ret = avcodec_receive_packet(impl->encoder, packet);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    {
                        break;
                    }

                    if (ret < 0)
                    {
                        success = set_error(error_message, std::string{"Alpha Recorder failed while draining the mask video encoder: "} +
                                                           av_error_message(ret));
                        break;
                    }

                    packet->stream_index = impl->video_stream->index;
                    packet->pos = -1;
                    av_packet_rescale_ts(packet, impl->encoder->time_base, impl->video_stream->time_base);
                    ret = av_interleaved_write_frame(impl->format, packet);
                    av_packet_unref(packet);
                    if (ret < 0)
                    {
                        success = set_error(error_message, std::string{"Alpha Recorder failed while writing a flushed mask video packet: "} +
                                                           av_error_message(ret));
                        break;
                    }
                }
                av_packet_free(&packet);
            }
        }

        if (success && impl->format != nullptr && impl->header_written && av_write_trailer(impl->format) < 0)
        {
            success = set_error(error_message, "Alpha Recorder failed to finalize the mask video.");
        }

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

} // namespace alpha_recorder::obs
