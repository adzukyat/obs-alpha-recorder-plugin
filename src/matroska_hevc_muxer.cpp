#include "matroska_hevc_muxer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <numeric>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <obs.h>
#include <obs-hevc.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <util/bmem.h>
}

namespace alpha_recorder::obs
{
    namespace
    {
        void assign_error(std::string *error_message, const char *message)
        {
            if (error_message != nullptr)
            {
                *error_message = message;
            }
        }

        void assign_error(std::string *error_message, const std::string &message)
        {
            if (error_message != nullptr)
            {
                *error_message = message;
            }
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

        [[nodiscard]] bool ensure_parent_directory(const std::filesystem::path &path)
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

        struct MatroskaSample
        {
            std::int64_t pts = 0;
            std::int64_t dts = 0;
            std::int64_t duration = 0;
            bool keyframe = false;
        };

        struct PendingMatroskaPacket
        {
            std::size_t sample_index = 0U;
            std::vector<std::uint8_t> data{};
        };

        [[nodiscard]] std::int64_t fallback_sample_duration(const std::vector<MatroskaSample> &samples,
                                                            std::int32_t timebase_num) noexcept
        {
            std::uint64_t gcd = 0U;
            for (std::size_t index = 1U; index < samples.size(); ++index)
            {
                const std::int64_t delta = samples[index].dts - samples[index - 1U].dts;
                if (delta > 0)
                {
                    gcd = gcd == 0U ? static_cast<std::uint64_t>(delta)
                                    : std::gcd(gcd, static_cast<std::uint64_t>(delta));
                }
            }
            if (gcd == 0U)
            {
                gcd = timebase_num <= 0 ? 1U : static_cast<std::uint64_t>(timebase_num);
            }
            return static_cast<std::int64_t>(std::min<std::uint64_t>(
                gcd,
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
        }

        [[nodiscard]] std::int64_t rescale_timestamp(std::int64_t value,
                                                     AVRational source_time_base,
                                                     AVRational destination_time_base,
                                                     AVRounding rounding) noexcept
        {
            return av_rescale_q_rnd(
                value,
                source_time_base,
                destination_time_base,
                static_cast<AVRounding>(rounding | AV_ROUND_PASS_MINMAX));
        }

        [[nodiscard]] bool checked_range_end(const AlphaVisiblePacketRange &range,
                                             std::int64_t &end,
                                             std::string *error_message)
        {
            if (range.media_time < 0 || range.duration < 0)
            {
                assign_error(error_message, "Matroska HEVC visible range cannot be negative");
                return false;
            }
            if (range.duration == 0)
            {
                end = range.media_time;
                return true;
            }
            if (range.media_time > std::numeric_limits<std::int64_t>::max() - range.duration)
            {
                assign_error(error_message, "Matroska HEVC visible range overflows the packet timeline");
                return false;
            }
            end = range.media_time + range.duration;
            return true;
        }

        void capture_headers_from_annexb(std::vector<std::uint8_t> &header_annexb,
                                         const std::uint8_t *data,
                                         std::size_t size)
        {
            if (!header_annexb.empty() || data == nullptr || size == 0U)
            {
                return;
            }

            std::uint8_t *new_packet_data = nullptr;
            std::size_t new_packet_size = 0U;
            std::uint8_t *header_data = nullptr;
            std::size_t header_size = 0U;
            std::uint8_t *sei_data = nullptr;
            std::size_t sei_size = 0U;
            obs_extract_hevc_headers(data, size, &new_packet_data, &new_packet_size, &header_data, &header_size,
                                     &sei_data, &sei_size);
            if (header_data != nullptr && header_size > 0U)
            {
                header_annexb.assign(header_data, header_data + header_size);
            }
            bfree(new_packet_data);
            bfree(header_data);
            bfree(sei_data);
        }
    } // namespace

    struct MatroskaHevcMuxer::Impl
    {
        std::filesystem::path path{};
        bool storage_open = false;
        bool accepting_packets = false;
        obs_encoder_t *encoder = nullptr;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::int32_t timebase_num = 1;
        std::int32_t timebase_den = 60;
        bool has_pending_visible_range = false;
        AlphaVisiblePacketRange pending_visible_range{};
        std::vector<std::uint8_t> header_annexb{};
        std::vector<MatroskaSample> samples{};
        std::deque<PendingMatroskaPacket> pending_packets{};
        std::uint64_t pending_packet_bytes = 0U;
        std::size_t tail_packet_buffer_size = 256U;
        std::size_t committed_sample_count = 0U;
        AVFormatContext *format_context = nullptr;
        AVStream *stream = nullptr;
        std::int64_t timestamp_origin = 0;
        AlphaMovieMuxerStats stats{};

        void release_encoder() noexcept
        {
            if (encoder != nullptr)
            {
                obs_encoder_release(encoder);
                encoder = nullptr;
            }
        }

        void close_format_context() noexcept
        {
            if (format_context == nullptr)
            {
                stream = nullptr;
                return;
            }
            if (format_context->pb != nullptr)
            {
                avio_closep(&format_context->pb);
            }
            avformat_free_context(format_context);
            format_context = nullptr;
            stream = nullptr;
        }

        void reset_state() noexcept
        {
            close_format_context();
            release_encoder();
            path.clear();
            storage_open = false;
            accepting_packets = false;
            width = 0U;
            height = 0U;
            timebase_num = 1;
            timebase_den = 60;
            has_pending_visible_range = false;
            pending_visible_range = {};
            header_annexb.clear();
            samples.clear();
            pending_packets.clear();
            pending_packet_bytes = 0U;
            tail_packet_buffer_size = 256U;
            committed_sample_count = 0U;
            timestamp_origin = 0;
            stats = {};
        }

        [[nodiscard]] bool ensure_headers(std::string *error_message)
        {
            if (!header_annexb.empty())
            {
                return true;
            }
            if (encoder == nullptr)
            {
                assign_error(error_message, "Matroska HEVC muxer has no HEVC encoder");
                return false;
            }

            std::uint8_t *extra_data = nullptr;
            std::size_t extra_size = 0U;
            if (obs_encoder_get_extra_data(encoder, &extra_data, &extra_size) &&
                extra_data != nullptr && extra_size > 0U)
            {
                capture_headers_from_annexb(header_annexb, extra_data, extra_size);
            }
            if (header_annexb.empty())
            {
                assign_error(error_message, "Matroska HEVC muxer could not read HEVC encoder headers");
                return false;
            }
            return true;
        }

        [[nodiscard]] bool open_format_context(std::int64_t origin, std::string *error_message)
        {
            if (format_context != nullptr)
            {
                if (timestamp_origin != origin)
                {
                    assign_error(error_message,
                                 "Matroska HEVC timestamp origin changed after streaming began");
                    return false;
                }
                return true;
            }
            if (!ensure_headers(error_message))
            {
                return false;
            }
            if (header_annexb.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            {
                assign_error(error_message, "Matroska HEVC stream metadata is too large");
                return false;
            }

            const std::string output_path = path_to_utf8(path);
            int result = avformat_alloc_output_context2(&format_context, nullptr, "matroska", output_path.c_str());
            if (result < 0 || format_context == nullptr)
            {
                assign_error(error_message,
                             std::string{"could not allocate Matroska output context: "} + av_error_message(result));
                close_format_context();
                return false;
            }

            stream = avformat_new_stream(format_context, nullptr);
            if (stream == nullptr)
            {
                assign_error(error_message, "could not create the Matroska HEVC video stream");
                close_format_context();
                return false;
            }
            stream->time_base = AVRational{timebase_num, timebase_den};
            stream->avg_frame_rate = AVRational{timebase_den, timebase_num};
            stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            stream->codecpar->codec_id = AV_CODEC_ID_HEVC;
            stream->codecpar->codec_tag = 0;
            stream->codecpar->width = static_cast<int>(width);
            stream->codecpar->height = static_cast<int>(height);
            stream->codecpar->extradata = static_cast<std::uint8_t *>(
                av_mallocz(header_annexb.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (stream->codecpar->extradata == nullptr)
            {
                assign_error(error_message, "could not allocate Matroska HEVC codec private data");
                close_format_context();
                return false;
            }
            std::memcpy(stream->codecpar->extradata, header_annexb.data(), header_annexb.size());
            stream->codecpar->extradata_size = static_cast<int>(header_annexb.size());

            result = avio_open(&format_context->pb, output_path.c_str(), AVIO_FLAG_WRITE);
            if (result < 0)
            {
                assign_error(error_message,
                             std::string{"could not open the Matroska HEVC output: "} + av_error_message(result));
                close_format_context();
                return false;
            }
            result = avformat_write_header(format_context, nullptr);
            if (result < 0)
            {
                assign_error(error_message,
                             std::string{"could not write the Matroska HEVC header: "} + av_error_message(result));
                close_format_context();
                return false;
            }

            timestamp_origin = origin;
            return true;
        }

        [[nodiscard]] bool write_packet(const PendingMatroskaPacket &pending,
                                        std::int64_t visible_end,
                                        bool terminal_visible_packet,
                                        std::string *error_message)
        {
            if (pending.sample_index >= samples.size())
            {
                assign_error(error_message, "Matroska HEVC pending packet metadata is invalid");
                return false;
            }
            const MatroskaSample &sample = samples[pending.sample_index];
            if (pending.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                assign_error(error_message, "Matroska HEVC packet is too large for FFmpeg");
                return false;
            }

            AVPacket *packet = av_packet_alloc();
            if (packet == nullptr)
            {
                assign_error(error_message, "could not allocate a Matroska HEVC packet");
                return false;
            }
            int result = av_new_packet(packet, static_cast<int>(pending.data.size()));
            if (result < 0)
            {
                av_packet_free(&packet);
                assign_error(error_message,
                             std::string{"could not allocate Matroska HEVC packet data: "} + av_error_message(result));
                return false;
            }
            std::memcpy(packet->data, pending.data.data(), pending.data.size());
            packet->stream_index = stream->index;
            packet->pts = sample.pts - timestamp_origin;
            packet->dts = sample.dts - timestamp_origin;
            packet->duration = sample.duration;
            if (visible_end != std::numeric_limits<std::int64_t>::max() &&
                sample.pts < visible_end && packet->duration > visible_end - sample.pts)
            {
                packet->duration = visible_end - sample.pts;
            }
            if (sample.keyframe)
            {
                packet->flags |= AV_PKT_FLAG_KEY;
            }
            av_packet_rescale_ts(packet, AVRational{timebase_num, timebase_den}, stream->time_base);

            if (terminal_visible_packet &&
                visible_end != std::numeric_limits<std::int64_t>::max() &&
                sample.pts == sample.dts)
            {
                const AVRational packet_time_base{timebase_num, timebase_den};
                const std::int64_t source_frame_duration =
                    fallback_sample_duration(samples, timebase_num);
                const std::int64_t visible_duration =
                    visible_end - timestamp_origin;
                const std::int64_t target_visible_end =
                    rescale_timestamp(visible_duration,
                                      packet_time_base,
                                      stream->time_base,
                                      AV_ROUND_DOWN);
                const std::int64_t demux_visible_duration =
                    std::max<std::int64_t>(
                        1,
                        rescale_timestamp(source_frame_duration,
                                          packet_time_base,
                                          stream->time_base,
                                          AV_ROUND_DOWN));
                const std::int64_t target_terminal_pts =
                    target_visible_end - demux_visible_duration;
                const std::int64_t adjustment =
                    target_terminal_pts - packet->pts;

                // FFmpeg's Matroska muxer uses a millisecond timecode scale and
                // its demuxer reports the floored DefaultDuration. Concentrate
                // the sub-millisecond CFR rounding remainder in the final
                // sample so a later stream-copy remux retains the intended
                // frame-count duration instead of ending one millisecond early.
                if (adjustment >= 0 && adjustment <= 1 &&
                    target_terminal_pts > 0)
                {
                    packet->pts = target_terminal_pts;
                    packet->dts += adjustment;
                    packet->duration = demux_visible_duration;
                    if (adjustment > 0)
                    {
                        blog(LOG_INFO,
                             "[alpha_recorder_matroska] compensated terminal CFR timestamp by %lld Matroska tick(s): visible_end=%lld terminal_pts=%lld duration=%lld",
                             static_cast<long long>(adjustment),
                             static_cast<long long>(target_visible_end),
                             static_cast<long long>(packet->pts),
                             static_cast<long long>(packet->duration));
                    }
                }
            }

            result = av_interleaved_write_frame(format_context, packet);
            av_packet_free(&packet);
            if (result < 0)
            {
                assign_error(error_message,
                             std::string{"could not write a Matroska HEVC packet: "} + av_error_message(result));
                return false;
            }

            ++stats.muxed_packet_count;
            return true;
        }

        [[nodiscard]] bool flush_oldest_pending_packet(std::string *error_message)
        {
            if (pending_packets.empty())
            {
                return true;
            }
            const PendingMatroskaPacket &pending = pending_packets.front();
            if (pending.sample_index != committed_sample_count ||
                pending.sample_index >= samples.size())
            {
                assign_error(error_message, "Matroska HEVC packet order is inconsistent");
                return false;
            }
            if (committed_sample_count == 0U && !samples.front().keyframe)
            {
                assign_error(error_message, "Matroska HEVC output must start on an IDR/keyframe packet");
                return false;
            }
            if (!open_format_context(samples.front().pts, error_message) ||
                !write_packet(pending,
                              std::numeric_limits<std::int64_t>::max(),
                              false,
                              error_message))
            {
                return false;
            }
            const std::uint64_t packet_bytes = static_cast<std::uint64_t>(pending.data.size());
            pending_packets.pop_front();
            pending_packet_bytes -= packet_bytes;
            ++committed_sample_count;
            return true;
        }
    };

    MatroskaHevcMuxer::MatroskaHevcMuxer()
        : impl_(std::make_unique<Impl>())
    {
    }

    MatroskaHevcMuxer::~MatroskaHevcMuxer() noexcept
    {
        abort();
    }

    bool MatroskaHevcMuxer::open(const MatroskaHevcMuxerConfig &config,
                                 std::string *error_message)
    {
        abort();
        impl_->reset_state();

        if (config.path.empty())
        {
            assign_error(error_message, "Matroska HEVC output path is empty");
            return false;
        }
        if (!ensure_parent_directory(config.path))
        {
            assign_error(error_message, "could not create the Matroska HEVC output directory");
            return false;
        }

        impl_->path = config.path;
        impl_->tail_packet_buffer_size = std::max<std::size_t>(config.tail_packet_buffer_size, 2U);
        impl_->storage_open = true;
        return true;
    }

    bool MatroskaHevcMuxer::begin(obs_output_t *output, std::string *error_message)
    {
        if (!impl_->storage_open)
        {
            assign_error(error_message, "Matroska HEVC output storage is not open");
            return false;
        }
        if (output == nullptr)
        {
            assign_error(error_message, "Matroska HEVC output cannot mux without an OBS output");
            return false;
        }
        if (impl_->accepting_packets)
        {
            return true;
        }

        obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0U);
        if (encoder == nullptr)
        {
            assign_error(error_message, "Matroska HEVC output could not find the OBS video encoder");
            return false;
        }
        const char *codec = obs_encoder_get_codec(encoder);
        if (codec == nullptr || std::strcmp(codec, "hevc") != 0)
        {
            assign_error(error_message, "Matroska HEVC output only supports HEVC video packets");
            return false;
        }

        impl_->release_encoder();
        impl_->encoder = obs_encoder_get_ref(encoder);
        impl_->width = obs_encoder_get_width(encoder);
        impl_->height = obs_encoder_get_height(encoder);
        if (impl_->width == 0U || impl_->height == 0U)
        {
            assign_error(error_message, "Matroska HEVC output received an unsupported video size");
            return false;
        }

        impl_->accepting_packets = true;
        return true;
    }

    bool MatroskaHevcMuxer::set_visible_range(const AlphaVisiblePacketRange &range,
                                              std::string *error_message)
    {
        std::int64_t visible_end = 0;
        if (!checked_range_end(range, visible_end, error_message))
        {
            return false;
        }
        if (range.duration == 0)
        {
            impl_->pending_visible_range = {};
            impl_->has_pending_visible_range = false;
            return true;
        }
        if (impl_->samples.empty())
        {
            assign_error(error_message, "Matroska HEVC visible range contains no HEVC packets");
            return false;
        }

        const auto first_visible = std::find_if(
            impl_->samples.begin(), impl_->samples.end(),
            [&range, visible_end](const MatroskaSample &sample) {
                return sample.pts >= range.media_time && sample.pts < visible_end;
            });
        if (first_visible == impl_->samples.end() || first_visible->pts != range.media_time)
        {
            assign_error(error_message, "Matroska HEVC visible range does not start on an encoded packet");
            return false;
        }
        if (!first_visible->keyframe)
        {
            assign_error(error_message, "Matroska HEVC visible range must start on an IDR/keyframe packet");
            return false;
        }

        const std::size_t first_visible_index = static_cast<std::size_t>(
            std::distance(impl_->samples.begin(), first_visible));
        if (impl_->committed_sample_count > 0U && first_visible_index != 0U)
        {
            assign_error(error_message,
                         "Matroska HEVC visible range starts after packets already written to the file");
            return false;
        }
        for (std::size_t index = 0U; index < impl_->committed_sample_count; ++index)
        {
            const MatroskaSample &sample = impl_->samples[index];
            if (sample.pts < range.media_time || sample.pts >= visible_end)
            {
                assign_error(error_message,
                             "Matroska HEVC visible range excludes packets already written to the file");
                return false;
            }
        }

        impl_->pending_visible_range = range;
        impl_->has_pending_visible_range = true;
        return true;
    }

    bool MatroskaHevcMuxer::submit_packet(encoder_packet *packet, std::string *error_message)
    {
        if (!impl_->accepting_packets || packet == nullptr || packet->type != OBS_ENCODER_VIDEO)
        {
            return true;
        }
        if (impl_->encoder != nullptr && packet->encoder != nullptr && packet->encoder != impl_->encoder)
        {
            return true;
        }
        if (packet->timebase_den <= 0 || packet->timebase_num <= 0)
        {
            assign_error(error_message, "Matroska HEVC muxer received an invalid packet timebase");
            return false;
        }
        if (packet->size == 0U || packet->data == nullptr)
        {
            return true;
        }
        if (packet->size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            assign_error(error_message, "Matroska HEVC muxer received an oversized HEVC packet");
            return false;
        }

        capture_headers_from_annexb(impl_->header_annexb, packet->data, packet->size);

        if (impl_->samples.empty())
        {
            impl_->timebase_num = packet->timebase_num;
            impl_->timebase_den = packet->timebase_den;
            impl_->stats.first_pts = packet->pts;
        }
        else
        {
            if (impl_->timebase_num != packet->timebase_num || impl_->timebase_den != packet->timebase_den)
            {
                assign_error(error_message, "Matroska HEVC muxer received mixed HEVC packet timebases");
                return false;
            }
            MatroskaSample &previous = impl_->samples.back();
            const std::int64_t duration = packet->dts - previous.dts;
            if (duration <= 0)
            {
                assign_error(error_message, "Matroska HEVC muxer requires strictly increasing HEVC DTS values");
                return false;
            }
            previous.duration = duration;
        }

        MatroskaSample sample{};
        sample.pts = packet->pts;
        sample.dts = packet->dts;
        sample.keyframe = packet->keyframe;
        impl_->samples.push_back(sample);

        PendingMatroskaPacket pending{};
        pending.sample_index = impl_->samples.size() - 1U;
        pending.data.assign(packet->data, packet->data + packet->size);
        impl_->pending_packets.push_back(std::move(pending));
        impl_->pending_packet_bytes += static_cast<std::uint64_t>(packet->size);

        impl_->stats.last_pts = packet->pts;
        ++impl_->stats.packet_count;
        if (sample.keyframe)
        {
            ++impl_->stats.keyframe_count;
        }
        impl_->stats.packet_bytes += static_cast<std::uint64_t>(packet->size);

        while (impl_->pending_packets.size() > impl_->tail_packet_buffer_size)
        {
            if (!impl_->flush_oldest_pending_packet(error_message))
            {
                return false;
            }
        }
        impl_->stats.max_buffered_packet_count = std::max<std::uint64_t>(
            impl_->stats.max_buffered_packet_count,
            static_cast<std::uint64_t>(impl_->pending_packets.size()));
        impl_->stats.max_buffered_packet_bytes = std::max(
            impl_->stats.max_buffered_packet_bytes,
            impl_->pending_packet_bytes);
        return true;
    }

    bool MatroskaHevcMuxer::finalize(std::string *error_message)
    {
        impl_->accepting_packets = false;
        if (!impl_->storage_open || impl_->stats.finalized)
        {
            return true;
        }
        if (impl_->samples.empty())
        {
            assign_error(error_message, "Matroska HEVC muxer received no HEVC packets");
            return false;
        }

        impl_->samples.back().duration = fallback_sample_duration(impl_->samples, impl_->timebase_num);
        const std::int64_t visible_start = impl_->has_pending_visible_range
                                               ? impl_->pending_visible_range.media_time
                                               : impl_->samples.front().pts;
        std::int64_t visible_end = std::numeric_limits<std::int64_t>::max();
        if (impl_->has_pending_visible_range &&
            !checked_range_end(impl_->pending_visible_range, visible_end, error_message))
        {
            return false;
        }

        const auto first_visible = std::find_if(
            impl_->samples.begin(), impl_->samples.end(),
            [visible_start, visible_end](const MatroskaSample &sample) {
                return sample.pts >= visible_start && sample.pts < visible_end;
            });
        if (first_visible == impl_->samples.end() || !first_visible->keyframe)
        {
            assign_error(error_message, "Matroska HEVC output does not start on an IDR/keyframe packet");
            return false;
        }
        const auto last_visible = std::max_element(
            impl_->samples.begin(),
            impl_->samples.end(),
            [visible_start, visible_end](const MatroskaSample &left,
                                         const MatroskaSample &right) {
                const bool left_visible =
                    left.pts >= visible_start && left.pts < visible_end;
                const bool right_visible =
                    right.pts >= visible_start && right.pts < visible_end;
                if (left_visible != right_visible)
                {
                    return !left_visible;
                }
                return left.pts < right.pts;
            });
        const std::size_t last_visible_index =
            last_visible == impl_->samples.end()
                ? std::numeric_limits<std::size_t>::max()
                : static_cast<std::size_t>(
                      std::distance(impl_->samples.begin(), last_visible));

        const std::int64_t origin = impl_->committed_sample_count > 0U
                                        ? impl_->samples.front().pts
                                        : visible_start;
        if (!impl_->open_format_context(origin, error_message))
        {
            return false;
        }

        for (const PendingMatroskaPacket &pending : impl_->pending_packets)
        {
            const MatroskaSample &sample = impl_->samples[pending.sample_index];
            if (sample.pts < visible_start || sample.pts >= visible_end)
            {
                continue;
            }
            if (!impl_->write_packet(
                    pending,
                    visible_end,
                    pending.sample_index == last_visible_index,
                    error_message))
            {
                return false;
            }
        }
        impl_->pending_packets.clear();
        impl_->pending_packet_bytes = 0U;

        if (impl_->stats.muxed_packet_count == 0U)
        {
            assign_error(error_message, "Matroska HEVC visible range contains no muxed packets");
            return false;
        }
        const int result = av_write_trailer(impl_->format_context);
        if (result < 0)
        {
            assign_error(error_message,
                         std::string{"could not write the Matroska HEVC trailer: "} + av_error_message(result));
            return false;
        }
        impl_->close_format_context();
        impl_->stats.finalized = true;
        return true;
    }

    void MatroskaHevcMuxer::close_storage() noexcept
    {
        impl_->accepting_packets = false;
        impl_->release_encoder();
        impl_->close_format_context();
        impl_->pending_packets.clear();
        impl_->pending_packet_bytes = 0U;
        impl_->storage_open = false;
    }

    void MatroskaHevcMuxer::abort() noexcept
    {
        close_storage();
    }

    const AlphaMovieMuxerStats &MatroskaHevcMuxer::stats() const noexcept
    {
        return impl_->stats;
    }

} // namespace alpha_recorder::obs
