#include "alpha_recorder/export_worker.hpp"

#include "alpha_recorder/manifest_writer.hpp"
#include "alpha_recorder/sidecar_reader.hpp"

#include <array>
#include <cstdint>
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
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace alpha_recorder::obs
{
    namespace
    {

        struct InputContext
        {
            AVFormatContext *format = nullptr;
            AVCodecContext *decoder = nullptr;
            int video_stream_index = -1;

            ~InputContext()
            {
                avcodec_free_context(&decoder);
                if (format != nullptr)
                {
                    avformat_close_input(&format);
                }
            }
        };

        struct OutputContext
        {
            AVFormatContext *format = nullptr;
            AVCodecContext *encoder = nullptr;
            AVStream *video_stream = nullptr;

            ~OutputContext()
            {
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

        struct FrameHolder
        {
            AVFrame *frame = nullptr;

            ~FrameHolder()
            {
                av_frame_free(&frame);
            }
        };

        struct SwsHolder
        {
            SwsContext *context = nullptr;

            ~SwsHolder()
            {
                sws_freeContext(context);
            }
        };

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
            const std::wstring native_path = path.native();
            if (native_path.empty())
            {
                return {};
            }

            const int required_size = WideCharToMultiByte(CP_UTF8, 0, native_path.c_str(), static_cast<int>(native_path.size()),
                                                          nullptr, 0, nullptr, nullptr);
            if (required_size <= 0)
            {
                return {};
            }

            std::string text(static_cast<std::size_t>(required_size), '\0');
            if (WideCharToMultiByte(CP_UTF8, 0, native_path.c_str(), static_cast<int>(native_path.size()), text.data(), required_size,
                                    nullptr, nullptr) <= 0)
            {
                return {};
            }

            return text;
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

        bool validate_supported_format(FinalizationFormat format, std::string *error_message)
        {
            const std::string_view unsupported_reason = finalization_format_export_unsupported_reason(format);
            if (!unsupported_reason.empty())
            {
                return set_error(error_message, std::string{"Alpha Recorder cannot export "} + std::string{finalization_format_display_name(format)} +
                                                    ": " + std::string{unsupported_reason});
            }

            return true;
        }

        bool open_input(InputContext &input, const std::filesystem::path &recording_path, std::string *error_message)
        {
            const std::string recording_path_text = path_to_utf8(recording_path);
            if (recording_path_text.empty())
            {
                return set_error(error_message, std::string{"Alpha Recorder could not convert the recording path: "} + recording_path.generic_string());
            }

            int ret = avformat_open_input(&input.format, recording_path_text.c_str(), nullptr, nullptr);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not open the recording file for export: "} +
                                                    recording_path.generic_string() + " (" + av_error_message(ret) + ")");
            }

            ret = avformat_find_stream_info(input.format, nullptr);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not read stream metadata from the recording: "} +
                                                    recording_path.generic_string() + " (" + av_error_message(ret) + ")");
            }

            ret = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not find a video stream in the recording: "} +
                                                    recording_path.generic_string() + " (" + av_error_message(ret) + ")");
            }

            input.video_stream_index = ret;

            AVStream *const video_stream = input.format->streams[input.video_stream_index];
            const AVCodec *const decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
            if (decoder == nullptr)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not find a decoder for the recording video codec: "} +
                                                    avcodec_get_name(video_stream->codecpar->codec_id));
            }

            input.decoder = avcodec_alloc_context3(decoder);
            if (input.decoder == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not allocate a video decoder context.");
            }

            ret = avcodec_parameters_to_context(input.decoder, video_stream->codecpar);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not initialize the video decoder context: "} +
                                                    av_error_message(ret));
            }

            ret = avcodec_open2(input.decoder, decoder, nullptr);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not open the video decoder: "} + av_error_message(ret));
            }

            return true;
        }

        bool open_output(OutputContext &output, const InputContext &input, const std::filesystem::path &output_path,
                         std::string *error_message)
        {
            const std::string output_path_text = path_to_utf8(output_path);
            if (output_path_text.empty())
            {
                return set_error(error_message, std::string{"Alpha Recorder could not convert the export path: "} + output_path.generic_string());
            }

            if (!ensure_parent_directory(output_path))
            {
                return set_error(error_message, std::string{"Alpha Recorder could not create the export directory: "} +
                                                    output_path.parent_path().generic_string());
            }

            int ret = avformat_alloc_output_context2(&output.format, nullptr, nullptr, output_path_text.c_str());
            if (ret < 0 || output.format == nullptr)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not allocate the export container: "} + av_error_message(ret));
            }

            AVStream *const input_video_stream = input.format->streams[input.video_stream_index];
            const AVRational time_base = input_video_stream->time_base;
            if (time_base.num <= 0 || time_base.den <= 0)
            {
                return set_error(error_message, "Alpha Recorder could not determine a valid timestamp base for the recording video.");
            }

            av_dict_copy(&output.format->metadata, input.format->metadata, 0);

            const AVCodec *const encoder = avcodec_find_encoder_by_name("prores_ks");
            const AVCodec *const fallback_encoder = (encoder != nullptr) ? encoder : avcodec_find_encoder(AV_CODEC_ID_PRORES);
            if (fallback_encoder == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not find a ProRes encoder in the bundled FFmpeg stack.");
            }

            output.encoder = avcodec_alloc_context3(fallback_encoder);
            if (output.encoder == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not allocate a ProRes encoder context.");
            }

            output.encoder->codec_type = AVMEDIA_TYPE_VIDEO;
            output.encoder->codec_id = fallback_encoder->id;
            output.encoder->width = input_video_stream->codecpar->width;
            output.encoder->height = input_video_stream->codecpar->height;
            output.encoder->pix_fmt = AV_PIX_FMT_YUVA444P10LE;
            output.encoder->profile = FF_PROFILE_PRORES_4444;
            output.encoder->time_base = time_base;
            output.encoder->framerate = input_video_stream->avg_frame_rate;
            output.encoder->sample_aspect_ratio = input_video_stream->sample_aspect_ratio;
            output.encoder->color_range = input_video_stream->codecpar->color_range;
            output.encoder->color_primaries = input_video_stream->codecpar->color_primaries;
            output.encoder->color_trc = input_video_stream->codecpar->color_trc;
            output.encoder->colorspace = input_video_stream->codecpar->color_space;
            output.encoder->chroma_sample_location = input_video_stream->codecpar->chroma_location;
            output.encoder->gop_size = 1;
            output.encoder->max_b_frames = 0;

            if ((output.format->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            {
                output.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            ret = avcodec_open2(output.encoder, fallback_encoder, nullptr);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not open the ProRes encoder: "} + av_error_message(ret));
            }

            output.video_stream = avformat_new_stream(output.format, fallback_encoder);
            if (output.video_stream == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not allocate the export video stream.");
            }

            ret = avcodec_parameters_from_context(output.video_stream->codecpar, output.encoder);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not configure the export video stream: "} +
                                                    av_error_message(ret));
            }

            output.video_stream->time_base = time_base;
            output.video_stream->sample_aspect_ratio = input_video_stream->sample_aspect_ratio;
            output.video_stream->disposition = input_video_stream->disposition;
            av_dict_copy(&output.video_stream->metadata, input_video_stream->metadata, 0);

            output.video_stream->codecpar->codec_tag = 0;

            if ((output.format->oformat->flags & AVFMT_NOFILE) == 0)
            {
                ret = avio_open(&output.format->pb, output_path_text.c_str(), AVIO_FLAG_WRITE);
                if (ret < 0)
                {
                    return set_error(error_message, std::string{"Alpha Recorder could not open the export file: "} + output_path.generic_string() +
                                                        " (" + av_error_message(ret) + ")");
                }
            }

            ret = avformat_write_header(output.format, nullptr);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not write the export container header: "} +
                                                    av_error_message(ret));
            }

            return true;
        }

        bool ensure_frame_buffers(FrameHolder &bgra_frame, FrameHolder &yuva_frame, int width, int height, std::string *error_message)
        {
            if (bgra_frame.frame == nullptr)
            {
                bgra_frame.frame = av_frame_alloc();
                if (bgra_frame.frame == nullptr)
                {
                    return set_error(error_message, "Alpha Recorder could not allocate a BGRA conversion frame.");
                }

                bgra_frame.frame->format = AV_PIX_FMT_BGRA;
                bgra_frame.frame->width = width;
                bgra_frame.frame->height = height;

                const int ret = av_frame_get_buffer(bgra_frame.frame, 32);
                if (ret < 0)
                {
                    return set_error(error_message, std::string{"Alpha Recorder could not allocate BGRA frame buffers: "} + av_error_message(ret));
                }
            }

            if (yuva_frame.frame == nullptr)
            {
                yuva_frame.frame = av_frame_alloc();
                if (yuva_frame.frame == nullptr)
                {
                    return set_error(error_message, "Alpha Recorder could not allocate a YUVA conversion frame.");
                }

                yuva_frame.frame->format = AV_PIX_FMT_YUVA444P10LE;
                yuva_frame.frame->width = width;
                yuva_frame.frame->height = height;

                const int ret = av_frame_get_buffer(yuva_frame.frame, 32);
                if (ret < 0)
                {
                    return set_error(error_message, std::string{"Alpha Recorder could not allocate YUVA frame buffers: "} + av_error_message(ret));
                }
            }

            return true;
        }

        bool encode_frame(OutputContext &output, AVFrame *decoded_frame, const AlphaIndexEntry &entry, const AlphaSidecarFrame &alpha_frame,
                          FrameHolder &bgra_frame, FrameHolder &yuva_frame, SwsHolder &source_to_bgra, SwsHolder &bgra_to_yuva,
                          std::string *error_message)
        {
            if (decoded_frame == nullptr || decoded_frame->format == AV_PIX_FMT_NONE || decoded_frame->width <= 0 || decoded_frame->height <= 0)
            {
                return set_error(error_message, "Alpha Recorder could not determine the decoded video frame geometry.");
            }

            if (entry.pts > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                return set_error(error_message, "Alpha Recorder encountered an out-of-range presentation timestamp while exporting.");
            }

            if (decoded_frame->pts != AV_NOPTS_VALUE && decoded_frame->pts != static_cast<std::int64_t>(entry.pts))
            {
                return set_error(error_message, "Alpha Recorder detected a timestamp mismatch between the recording and alpha sidecar.");
            }

            if (!ensure_frame_buffers(bgra_frame, yuva_frame, decoded_frame->width, decoded_frame->height, error_message))
            {
                return false;
            }

            if (source_to_bgra.context == nullptr)
            {
                source_to_bgra.context = sws_getContext(decoded_frame->width, decoded_frame->height,
                                                        static_cast<AVPixelFormat>(decoded_frame->format), decoded_frame->width,
                                                        decoded_frame->height, AV_PIX_FMT_BGRA, SWS_BICUBIC, nullptr, nullptr, nullptr);
                if (source_to_bgra.context == nullptr)
                {
                    return set_error(error_message, "Alpha Recorder could not create the source-to-BGRA conversion context.");
                }
            }

            if (bgra_to_yuva.context == nullptr)
            {
                bgra_to_yuva.context = sws_getContext(decoded_frame->width, decoded_frame->height, AV_PIX_FMT_BGRA, decoded_frame->width,
                                                      decoded_frame->height, AV_PIX_FMT_YUVA444P10LE, SWS_BICUBIC, nullptr, nullptr, nullptr);
                if (bgra_to_yuva.context == nullptr)
                {
                    return set_error(error_message, "Alpha Recorder could not create the BGRA-to-YUVA conversion context.");
                }
            }

            if (av_frame_make_writable(bgra_frame.frame) < 0 || av_frame_make_writable(yuva_frame.frame) < 0)
            {
                return set_error(error_message, "Alpha Recorder could not obtain writable export frames.");
            }

            const int source_height = sws_scale(source_to_bgra.context, decoded_frame->data, decoded_frame->linesize, 0, decoded_frame->height,
                                                bgra_frame.frame->data, bgra_frame.frame->linesize);
            if (source_height <= 0)
            {
                return set_error(error_message, "Alpha Recorder could not convert a decoded video frame to BGRA.");
            }

            for (int row = 0; row < decoded_frame->height; ++row)
            {
                std::uint8_t *const dest_row = bgra_frame.frame->data[0] + (static_cast<std::size_t>(row) *
                                                                            static_cast<std::size_t>(bgra_frame.frame->linesize[0]));
                const std::uint8_t *const alpha_row = alpha_frame.alpha_bytes.data() +
                                                      (static_cast<std::size_t>(row) * static_cast<std::size_t>(decoded_frame->width));

                for (int column = 0; column < decoded_frame->width; ++column)
                {
                    dest_row[(static_cast<std::size_t>(column) * 4U) + 3U] = alpha_row[column];
                }
            }

            const int export_height = sws_scale(bgra_to_yuva.context, bgra_frame.frame->data, bgra_frame.frame->linesize, 0,
                                                decoded_frame->height, yuva_frame.frame->data, yuva_frame.frame->linesize);
            if (export_height <= 0)
            {
                return set_error(error_message, "Alpha Recorder could not convert the alpha-masked frame for export.");
            }

            yuva_frame.frame->pts = static_cast<std::int64_t>(entry.pts);
            if (av_frame_copy_props(yuva_frame.frame, decoded_frame) < 0)
            {
                return set_error(error_message, "Alpha Recorder could not preserve the decoded frame metadata for export.");
            }

            yuva_frame.frame->pts = static_cast<std::int64_t>(entry.pts);

            int ret = avcodec_send_frame(output.encoder, yuva_frame.frame);
            if (ret < 0)
            {
                return set_error(error_message, std::string{"Alpha Recorder failed to encode a video frame: "} + av_error_message(ret));
            }

            AVPacket *packet = av_packet_alloc();
            if (packet == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not allocate an encoded packet.");
            }

            while (true)
            {
                ret = avcodec_receive_packet(output.encoder, packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    av_packet_free(&packet);
                    return true;
                }

                if (ret < 0)
                {
                    av_packet_free(&packet);
                    return set_error(error_message, std::string{"Alpha Recorder failed while retrieving an encoded packet: "} + av_error_message(ret));
                }

                packet->stream_index = output.video_stream->index;
                packet->pos = -1;
                av_packet_rescale_ts(packet, output.encoder->time_base, output.video_stream->time_base);

                ret = av_interleaved_write_frame(output.format, packet);
                av_packet_unref(packet);
                if (ret < 0)
                {
                    av_packet_free(&packet);
                    return set_error(error_message, std::string{"Alpha Recorder failed while writing an exported video packet: "} + av_error_message(ret));
                }
            }
        }

        bool flush_encoder(OutputContext &output, std::string *error_message)
        {
            int ret = avcodec_send_frame(output.encoder, nullptr);
            if (ret < 0 && ret != AVERROR_EOF)
            {
                return set_error(error_message, std::string{"Alpha Recorder failed to flush the video encoder: "} + av_error_message(ret));
            }

            AVPacket *packet = av_packet_alloc();
            if (packet == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not allocate a flush packet.");
            }

            while (true)
            {
                ret = avcodec_receive_packet(output.encoder, packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    av_packet_free(&packet);
                    return true;
                }

                if (ret < 0)
                {
                    av_packet_free(&packet);
                    return set_error(error_message, std::string{"Alpha Recorder failed while draining the video encoder: "} + av_error_message(ret));
                }

                packet->stream_index = output.video_stream->index;
                packet->pos = -1;
                av_packet_rescale_ts(packet, output.encoder->time_base, output.video_stream->time_base);

                ret = av_interleaved_write_frame(output.format, packet);
                av_packet_unref(packet);
                if (ret < 0)
                {
                    av_packet_free(&packet);
                    return set_error(error_message, std::string{"Alpha Recorder failed while writing a flushed video packet: "} + av_error_message(ret));
                }
            }
        }

    } // namespace

    bool export_completed_recording(const FinalizationExportRequest &request, std::string *error_message) noexcept
    {
        try
        {
            if (!validate_supported_format(request.finalization_format, error_message))
            {
                return false;
            }

            if (request.recording_path.empty() || request.sidecar_path.empty() || request.manifest_path.empty())
            {
                return set_error(error_message, "Alpha Recorder export received an incomplete file set.");
            }

            if (!std::filesystem::exists(request.recording_path))
            {
                return set_error(error_message, std::string{"Alpha Recorder could not find the recorded video file: "} +
                                                    request.recording_path.generic_string());
            }

            ManifestWriter manifest_reader;
            AlphaSessionSummary summary;
            if (!manifest_reader.read(request.manifest_path, summary, error_message))
            {
                return false;
            }

            const std::string expected_format = std::string{finalization_format_config_value(request.finalization_format)};
            if (summary.finalization_format != expected_format)
            {
                return set_error(error_message, std::string{"Alpha Recorder manifest finalization format mismatch: expected "} +
                                                    expected_format + ", got " + summary.finalization_format);
            }

            if (summary.sidecar_path != request.sidecar_path || summary.manifest_path != request.manifest_path)
            {
                return set_error(error_message, "Alpha Recorder manifest paths do not match the completed sidecar files.");
            }

            AlphaSidecarReader sidecar_reader;
            if (!sidecar_reader.open(request.sidecar_path, error_message))
            {
                return false;
            }

            const std::vector<AlphaIndexEntry> &entries = sidecar_reader.index_entries();
            if (summary.pair_count != entries.size())
            {
                return set_error(error_message, "Alpha Recorder manifest and sidecar record counts do not match.");
            }

            const std::filesystem::path output_path = finalization_output_path(request.sidecar_path, request.finalization_format);

            InputContext input;
            if (!open_input(input, request.recording_path, error_message))
            {
                return false;
            }

            OutputContext output;
            if (!open_output(output, input, output_path, error_message))
            {
                return false;
            }

            FrameHolder decoded_frame;
            FrameHolder bgra_frame;
            FrameHolder yuva_frame;
            SwsHolder source_to_bgra;
            SwsHolder bgra_to_yuva;
            AVPacket *packet = av_packet_alloc();
            if (packet == nullptr)
            {
                return set_error(error_message, "Alpha Recorder could not allocate an input packet buffer.");
            }

            std::size_t next_alpha_index = 0;
            bool success = true;
            while (success)
            {
                const int ret = av_read_frame(input.format, packet);
                if (ret == AVERROR_EOF)
                {
                    break;
                }

                if (ret < 0)
                {
                    success = set_error(error_message, std::string{"Alpha Recorder failed while reading the recording file: "} + av_error_message(ret));
                    break;
                }

                if (packet->stream_index == input.video_stream_index)
                {
                    int send_result = avcodec_send_packet(input.decoder, packet);
                    av_packet_unref(packet);
                    if (send_result < 0 && send_result != AVERROR(EAGAIN))
                    {
                        success = set_error(error_message, std::string{"Alpha Recorder failed to decode a video packet: "} + av_error_message(send_result));
                        break;
                    }

                    while (true)
                    {
                        send_result = avcodec_receive_frame(input.decoder, decoded_frame.frame);
                        if (send_result == AVERROR(EAGAIN) || send_result == AVERROR_EOF)
                        {
                            break;
                        }

                        if (send_result < 0)
                        {
                            success = set_error(error_message, std::string{"Alpha Recorder failed while decoding the recording video: "} +
                                                                   av_error_message(send_result));
                            break;
                        }

                        if (next_alpha_index >= entries.size())
                        {
                            success = set_error(error_message, "Alpha Recorder decoded more video frames than the sidecar contains.");
                            break;
                        }

                        const AlphaIndexEntry &entry = entries[next_alpha_index];
                        AlphaSidecarFrame alpha_frame;
                        if (!sidecar_reader.read_frame(entry, alpha_frame, error_message))
                        {
                            success = false;
                            break;
                        }

                        if (!encode_frame(output, decoded_frame.frame, entry, alpha_frame, bgra_frame, yuva_frame, source_to_bgra,
                                          bgra_to_yuva, error_message))
                        {
                            success = false;
                            break;
                        }

                        ++next_alpha_index;
                    }

                    if (!success)
                    {
                        break;
                    }
                }

                av_packet_unref(packet);
            }

            av_packet_free(&packet);
            if (!success)
            {
                return false;
            }

            int ret = avcodec_send_packet(input.decoder, nullptr);
            if (ret < 0 && ret != AVERROR_EOF)
            {
                return set_error(error_message, std::string{"Alpha Recorder failed to flush the video decoder: "} + av_error_message(ret));
            }

            while (true)
            {
                ret = avcodec_receive_frame(input.decoder, decoded_frame.frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    break;
                }

                if (ret < 0)
                {
                    return set_error(error_message, std::string{"Alpha Recorder failed while draining the video decoder: "} + av_error_message(ret));
                }

                if (next_alpha_index >= entries.size())
                {
                    return set_error(error_message, "Alpha Recorder decoded more video frames than the sidecar contains.");
                }

                const AlphaIndexEntry &entry = entries[next_alpha_index];
                AlphaSidecarFrame alpha_frame;
                if (!sidecar_reader.read_frame(entry, alpha_frame, error_message))
                {
                    return false;
                }

                if (!encode_frame(output, decoded_frame.frame, entry, alpha_frame, bgra_frame, yuva_frame, source_to_bgra, bgra_to_yuva,
                                  error_message))
                {
                    return false;
                }

                ++next_alpha_index;
            }

            if (next_alpha_index != entries.size())
            {
                return set_error(error_message, "Alpha Recorder decoded fewer video frames than the sidecar contains.");
            }

            if (!flush_encoder(output, error_message))
            {
                return false;
            }

            if (av_write_trailer(output.format) < 0)
            {
                return set_error(error_message, "Alpha Recorder failed to finalize the exported movie.");
            }

            return true;
        }
        catch (...)
        {
            return set_error(error_message, "Alpha Recorder export failed due to an unexpected error.");
        }
    }

} // namespace alpha_recorder::obs
