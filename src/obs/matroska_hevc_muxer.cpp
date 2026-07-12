#include "matroska_hevc_muxer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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
            std::uint64_t offset = 0U;
            std::uint32_t size = 0U;
            std::int64_t pts = 0;
            std::int64_t dts = 0;
            std::int64_t duration = 0;
            bool keyframe = false;
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

        [[nodiscard]] bool finalize_sample_durations(std::vector<MatroskaSample> &samples,
                                                     std::int32_t timebase_num,
                                                     std::string *error_message)
        {
            if (samples.empty())
            {
                assign_error(error_message, "Matroska HEVC muxer received no HEVC packets");
                return false;
            }

            const std::int64_t last_duration = fallback_sample_duration(samples, timebase_num);
            for (std::size_t index = 0U; index < samples.size(); ++index)
            {
                std::int64_t duration = last_duration;
                if (index + 1U < samples.size())
                {
                    duration = samples[index + 1U].dts - samples[index].dts;
                    if (duration <= 0)
                    {
                        assign_error(error_message, "Matroska HEVC muxer requires strictly increasing HEVC DTS values");
                        return false;
                    }
                }
                samples[index].duration = duration;
            }
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
        std::filesystem::path spool_path{};
        std::ofstream spool_writer{};
        bool storage_open = false;
        bool accepting_packets = false;
        bool first_packet = true;
        obs_encoder_t *encoder = nullptr;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::int32_t timebase_num = 1;
        std::int32_t timebase_den = 60;
        bool has_pending_visible_range = false;
        AlphaVisiblePacketRange pending_visible_range{};
        std::vector<std::uint8_t> header_annexb{};
        std::vector<MatroskaSample> samples{};
        DirectMp4MuxerStats stats{};

        void reset_state() noexcept
        {
            path.clear();
            spool_path.clear();
            storage_open = false;
            accepting_packets = false;
            first_packet = true;
            width = 0U;
            height = 0U;
            timebase_num = 1;
            timebase_den = 60;
            has_pending_visible_range = false;
            pending_visible_range = {};
            header_annexb.clear();
            samples.clear();
            stats = {};
        }

        void release_encoder() noexcept
        {
            if (encoder != nullptr)
            {
                obs_encoder_release(encoder);
                encoder = nullptr;
            }
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
            if (obs_encoder_get_extra_data(encoder, &extra_data, &extra_size) && extra_data != nullptr && extra_size > 0U)
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

        impl_->spool_path = config.path;
        impl_->spool_path += ".spool";
        impl_->spool_writer.open(impl_->spool_path, std::ios::binary | std::ios::trunc);
        if (!impl_->spool_writer)
        {
            assign_error(error_message, "could not open the Matroska HEVC packet spool");
            return false;
        }

        impl_->path = config.path;
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
        if (range.media_time < 0 || range.duration < 0)
        {
            assign_error(error_message, "Matroska HEVC visible range cannot be negative");
            return false;
        }

        impl_->pending_visible_range = range;
        impl_->has_pending_visible_range = range.duration > 0;
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
        if (packet->size > std::numeric_limits<std::uint32_t>::max())
        {
            assign_error(error_message, "Matroska HEVC muxer received an oversized HEVC packet");
            return false;
        }

        capture_headers_from_annexb(impl_->header_annexb, packet->data, packet->size);

        if (impl_->first_packet)
        {
            impl_->timebase_num = packet->timebase_num;
            impl_->timebase_den = packet->timebase_den;
            impl_->stats.first_pts = packet->pts;
            impl_->first_packet = false;
        }
        else if (impl_->timebase_num != packet->timebase_num || impl_->timebase_den != packet->timebase_den)
        {
            assign_error(error_message, "Matroska HEVC muxer received mixed HEVC packet timebases");
            return false;
        }

        const std::uint64_t offset = static_cast<std::uint64_t>(impl_->spool_writer.tellp());
        impl_->spool_writer.write(reinterpret_cast<const char *>(packet->data),
                                  static_cast<std::streamsize>(packet->size));
        if (!impl_->spool_writer)
        {
            assign_error(error_message, "Matroska HEVC muxer could not write an HEVC packet to the spool");
            return false;
        }

        MatroskaSample sample{};
        sample.offset = offset;
        sample.size = static_cast<std::uint32_t>(packet->size);
        sample.pts = packet->pts;
        sample.dts = packet->dts;
        sample.keyframe = packet->keyframe;
        impl_->samples.push_back(sample);

        impl_->stats.last_pts = packet->pts;
        ++impl_->stats.packet_count;
        ++impl_->stats.muxed_packet_count;
        if (sample.keyframe)
        {
            ++impl_->stats.keyframe_count;
        }
        impl_->stats.packet_bytes += static_cast<std::uint64_t>(sample.size);
        return true;
    }

    bool MatroskaHevcMuxer::finalize(std::string *error_message)
    {
        impl_->accepting_packets = false;
        if (!impl_->storage_open || impl_->stats.finalized)
        {
            return true;
        }
        if (impl_->spool_writer.is_open())
        {
            impl_->spool_writer.close();
        }
        if (!impl_->ensure_headers(error_message))
        {
            return false;
        }
        if (!finalize_sample_durations(impl_->samples, impl_->timebase_num, error_message))
        {
            return false;
        }

        const std::int64_t first_pts = impl_->samples.front().pts;
        const std::int64_t visible_start = impl_->has_pending_visible_range ? impl_->pending_visible_range.media_time : first_pts;
        const std::int64_t visible_end = impl_->has_pending_visible_range
                                             ? impl_->pending_visible_range.media_time + impl_->pending_visible_range.duration
                                             : std::numeric_limits<std::int64_t>::max();
        std::vector<std::size_t> visible_indices;
        visible_indices.reserve(impl_->samples.size());
        for (std::size_t index = 0U; index < impl_->samples.size(); ++index)
        {
            const MatroskaSample &sample = impl_->samples[index];
            if (sample.pts >= visible_start && sample.pts < visible_end)
            {
                visible_indices.push_back(index);
            }
        }
        if (visible_indices.empty())
        {
            assign_error(error_message, "Matroska HEVC visible range contains no HEVC packets");
            return false;
        }
        if (!impl_->samples[visible_indices.front()].keyframe)
        {
            assign_error(error_message, "Matroska HEVC visible range must start on an IDR/keyframe packet");
            return false;
        }

        AVFormatContext *format_context = nullptr;
        const std::string output_path = path_to_utf8(impl_->path);
        int result = avformat_alloc_output_context2(&format_context, nullptr, "matroska", output_path.c_str());
        if (result < 0 || format_context == nullptr)
        {
            assign_error(error_message, std::string{"could not allocate Matroska output context: "} + av_error_message(result));
            return false;
        }

        auto close_format_context = [&format_context]() noexcept {
            if (format_context != nullptr)
            {
                if (format_context->pb != nullptr)
                {
                    avio_closep(&format_context->pb);
                }
                avformat_free_context(format_context);
                format_context = nullptr;
            }
        };

        AVStream *stream = avformat_new_stream(format_context, nullptr);
        if (stream == nullptr)
        {
            close_format_context();
            assign_error(error_message, "could not create the Matroska HEVC video stream");
            return false;
        }
        stream->time_base = AVRational{impl_->timebase_num, impl_->timebase_den};
        stream->avg_frame_rate = AVRational{impl_->timebase_den, impl_->timebase_num};
        stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        stream->codecpar->codec_id = AV_CODEC_ID_HEVC;
        stream->codecpar->codec_tag = 0;
        stream->codecpar->width = static_cast<int>(impl_->width);
        stream->codecpar->height = static_cast<int>(impl_->height);
        stream->codecpar->extradata = static_cast<std::uint8_t *>(av_mallocz(impl_->header_annexb.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (stream->codecpar->extradata == nullptr)
        {
            close_format_context();
            assign_error(error_message, "could not allocate Matroska HEVC codec private data");
            return false;
        }
        std::memcpy(stream->codecpar->extradata, impl_->header_annexb.data(), impl_->header_annexb.size());
        stream->codecpar->extradata_size = static_cast<int>(impl_->header_annexb.size());

        result = avio_open(&format_context->pb, output_path.c_str(), AVIO_FLAG_WRITE);
        if (result < 0)
        {
            close_format_context();
            assign_error(error_message, std::string{"could not open the Matroska HEVC output: "} + av_error_message(result));
            return false;
        }
        result = avformat_write_header(format_context, nullptr);
        if (result < 0)
        {
            close_format_context();
            assign_error(error_message, std::string{"could not write the Matroska HEVC header: "} + av_error_message(result));
            return false;
        }

        std::ifstream spool_reader{impl_->spool_path, std::ios::binary};
        if (!spool_reader)
        {
            close_format_context();
            assign_error(error_message, "could not reopen the Matroska HEVC packet spool");
            return false;
        }

        std::vector<std::uint8_t> packet_buffer;
        for (const std::size_t index : visible_indices)
        {
            const MatroskaSample &sample = impl_->samples[index];
            packet_buffer.resize(sample.size);
            spool_reader.seekg(static_cast<std::streamoff>(sample.offset), std::ios::beg);
            spool_reader.read(reinterpret_cast<char *>(packet_buffer.data()),
                              static_cast<std::streamsize>(packet_buffer.size()));
            if (!spool_reader)
            {
                close_format_context();
                assign_error(error_message, "could not read an HEVC packet from the Matroska spool");
                return false;
            }

            AVPacket *av_packet = av_packet_alloc();
            if (av_packet == nullptr)
            {
                close_format_context();
                assign_error(error_message, "could not allocate a Matroska HEVC packet");
                return false;
            }
            result = av_new_packet(av_packet, static_cast<int>(packet_buffer.size()));
            if (result < 0)
            {
                av_packet_free(&av_packet);
                close_format_context();
                assign_error(error_message, std::string{"could not allocate Matroska HEVC packet data: "} + av_error_message(result));
                return false;
            }
            std::memcpy(av_packet->data, packet_buffer.data(), packet_buffer.size());
            av_packet->stream_index = stream->index;
            av_packet->pts = sample.pts - visible_start;
            av_packet->dts = sample.dts - visible_start;
            av_packet->duration = sample.duration;
            if (sample.keyframe)
            {
                av_packet->flags |= AV_PKT_FLAG_KEY;
            }
            av_packet_rescale_ts(av_packet, AVRational{impl_->timebase_num, impl_->timebase_den}, stream->time_base);

            result = av_interleaved_write_frame(format_context, av_packet);
            av_packet_free(&av_packet);
            if (result < 0)
            {
                close_format_context();
                assign_error(error_message, std::string{"could not write a Matroska HEVC packet: "} + av_error_message(result));
                return false;
            }
        }

        result = av_write_trailer(format_context);
        if (result < 0)
        {
            close_format_context();
            assign_error(error_message, std::string{"could not write the Matroska HEVC trailer: "} + av_error_message(result));
            return false;
        }
        close_format_context();

        impl_->stats.finalized = true;
        return true;
    }

    void MatroskaHevcMuxer::close_storage() noexcept
    {
        impl_->accepting_packets = false;
        impl_->release_encoder();
        if (impl_->spool_writer.is_open())
        {
            impl_->spool_writer.close();
        }
        if (!impl_->spool_path.empty())
        {
            std::error_code error;
            std::filesystem::remove(impl_->spool_path, error);
        }
        impl_->storage_open = false;
    }

    void MatroskaHevcMuxer::abort() noexcept
    {
        close_storage();
    }

    bool MatroskaHevcMuxer::is_open() const noexcept
    {
        return impl_->storage_open;
    }

    bool MatroskaHevcMuxer::is_accepting_packets() const noexcept
    {
        return impl_->accepting_packets;
    }

    const std::filesystem::path &MatroskaHevcMuxer::path() const noexcept
    {
        return impl_->path;
    }

    const DirectMp4MuxerStats &MatroskaHevcMuxer::stats() const noexcept
    {
        return impl_->stats;
    }

} // namespace alpha_recorder::obs
