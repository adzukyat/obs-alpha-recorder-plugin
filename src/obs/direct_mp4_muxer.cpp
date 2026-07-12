#include "direct_mp4_muxer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstring>
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
#include <util/bmem.h>
#include <util/buffered-file-serializer.h>
}

namespace alpha_recorder::obs
{
    namespace
    {
        constexpr std::uint32_t kMovieTimescale = 1000U;
        constexpr std::uint64_t kMp4EpochOffset = 0x7C25B080ULL;

        void assign_error(std::string *error_message, const char *message)
        {
            if (error_message != nullptr)
            {
                *error_message = message;
            }
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

        class ByteWriter
        {
        public:
            [[nodiscard]] const std::vector<std::uint8_t> &bytes() const noexcept
            {
                return bytes_;
            }

            [[nodiscard]] std::size_t position() const noexcept
            {
                return bytes_.size();
            }

            void u8(std::uint8_t value)
            {
                bytes_.push_back(value);
            }

            void u16(std::uint16_t value)
            {
                bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
                bytes_.push_back(static_cast<std::uint8_t>(value));
            }

            void u24(std::uint32_t value)
            {
                bytes_.push_back(static_cast<std::uint8_t>(value >> 16));
                bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
                bytes_.push_back(static_cast<std::uint8_t>(value));
            }

            void u32(std::uint32_t value)
            {
                bytes_.push_back(static_cast<std::uint8_t>(value >> 24));
                bytes_.push_back(static_cast<std::uint8_t>(value >> 16));
                bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
                bytes_.push_back(static_cast<std::uint8_t>(value));
            }

            void i32(std::int32_t value)
            {
                u32(static_cast<std::uint32_t>(value));
            }

            void u64(std::uint64_t value)
            {
                u32(static_cast<std::uint32_t>(value >> 32));
                u32(static_cast<std::uint32_t>(value));
            }

            void i64(std::int64_t value)
            {
                u64(static_cast<std::uint64_t>(value));
            }

            void fourcc(const char (&text)[5])
            {
                bytes_.insert(bytes_.end(), text, text + 4);
            }

            void raw(const void *data, std::size_t size)
            {
                if (data == nullptr || size == 0U)
                {
                    return;
                }
                const auto *begin = static_cast<const std::uint8_t *>(data);
                bytes_.insert(bytes_.end(), begin, begin + size);
            }

            void zeros(std::size_t size)
            {
                bytes_.insert(bytes_.end(), size, 0U);
            }

            [[nodiscard]] std::size_t begin_box(const char (&type)[5])
            {
                const std::size_t start = position();
                u32(0);
                fourcc(type);
                return start;
            }

            [[nodiscard]] std::size_t begin_full_box(const char (&type)[5], std::uint8_t version,
                                                     std::uint32_t flags)
            {
                const std::size_t start = begin_box(type);
                u8(version);
                u24(flags);
                return start;
            }

            void end_box(std::size_t start)
            {
                const std::size_t size = position() - start;
                patch_u32(start, static_cast<std::uint32_t>(size));
            }

        private:
            void patch_u32(std::size_t offset, std::uint32_t value)
            {
                bytes_[offset + 0U] = static_cast<std::uint8_t>(value >> 24);
                bytes_[offset + 1U] = static_cast<std::uint8_t>(value >> 16);
                bytes_[offset + 2U] = static_cast<std::uint8_t>(value >> 8);
                bytes_[offset + 3U] = static_cast<std::uint8_t>(value);
            }

            std::vector<std::uint8_t> bytes_{};
        };

        [[nodiscard]] bool serializer_write_all(serializer &s, const void *data, std::size_t size)
        {
            return size == 0U || s_write(&s, data, size) == size;
        }

        [[nodiscard]] bool serializer_write_u32(serializer &s, std::uint32_t value)
        {
            const std::array<std::uint8_t, 4U> data{
                static_cast<std::uint8_t>(value >> 24),
                static_cast<std::uint8_t>(value >> 16),
                static_cast<std::uint8_t>(value >> 8),
                static_cast<std::uint8_t>(value),
            };
            return serializer_write_all(s, data.data(), data.size());
        }

        [[nodiscard]] bool serializer_write_u64(serializer &s, std::uint64_t value)
        {
            return serializer_write_u32(s, static_cast<std::uint32_t>(value >> 32)) &&
                   serializer_write_u32(s, static_cast<std::uint32_t>(value));
        }

        [[nodiscard]] std::uint64_t scale_u64(std::uint64_t value, std::uint64_t numerator,
                                              std::uint64_t denominator) noexcept
        {
            if (denominator == 0U)
            {
                return 0U;
            }
            const std::uint64_t quotient = value / denominator;
            const std::uint64_t remainder = value % denominator;
            return quotient * numerator + (remainder * numerator) / denominator;
        }

        [[nodiscard]] std::size_t find_start_code(const std::uint8_t *data, std::size_t size,
                                                  std::size_t offset) noexcept
        {
            if (data == nullptr || size < 3U || offset >= size)
            {
                return size;
            }
            for (std::size_t index = offset; index + 3U <= size; ++index)
            {
                if (data[index] == 0U && data[index + 1U] == 0U && data[index + 2U] == 1U)
                {
                    return index;
                }
                if (index + 4U <= size && data[index] == 0U && data[index + 1U] == 0U &&
                    data[index + 2U] == 0U && data[index + 3U] == 1U)
                {
                    return index;
                }
            }
            return size;
        }

        [[nodiscard]] std::size_t start_code_length(const std::uint8_t *data, std::size_t size,
                                                    std::size_t offset) noexcept
        {
            if (offset + 3U <= size && data[offset] == 0U && data[offset + 1U] == 0U &&
                data[offset + 2U] == 1U)
            {
                return 3U;
            }
            if (offset + 4U <= size && data[offset] == 0U && data[offset + 1U] == 0U &&
                data[offset + 2U] == 0U && data[offset + 3U] == 1U)
            {
                return 4U;
            }
            return 0U;
        }

        [[nodiscard]] std::vector<std::uint8_t> rbsp_from_nal(const std::vector<std::uint8_t> &nal)
        {
            std::vector<std::uint8_t> rbsp;
            rbsp.reserve(nal.size());
            int zero_count = 0;
            for (const std::uint8_t value : nal)
            {
                if (zero_count >= 2 && value == 0x03U)
                {
                    zero_count = 0;
                    continue;
                }
                rbsp.push_back(value);
                if (value == 0U)
                {
                    ++zero_count;
                }
                else
                {
                    zero_count = 0;
                }
            }
            return rbsp;
        }

        [[nodiscard]] std::uint8_t hevc_nal_type(const std::vector<std::uint8_t> &nal) noexcept
        {
            if (nal.empty())
            {
                return 0U;
            }
            return static_cast<std::uint8_t>((nal[0] & 0x7EU) >> 1U);
        }

        struct HevcParameterSets
        {
            std::vector<std::vector<std::uint8_t>> vps{};
            std::vector<std::vector<std::uint8_t>> sps{};
            std::vector<std::vector<std::uint8_t>> pps{};
        };

        void append_unique(std::vector<std::vector<std::uint8_t>> &values,
                           std::vector<std::uint8_t> value)
        {
            const auto existing = std::find(values.begin(), values.end(), value);
            if (existing == values.end())
            {
                values.push_back(std::move(value));
            }
        }

        [[nodiscard]] HevcParameterSets parse_parameter_sets(const std::vector<std::uint8_t> &annexb)
        {
            HevcParameterSets sets;
            const std::uint8_t *data = annexb.data();
            const std::size_t size = annexb.size();
            std::size_t start_code = find_start_code(data, size, 0U);
            while (start_code < size)
            {
                const std::size_t nal_start = start_code + start_code_length(data, size, start_code);
                const std::size_t next_start_code = find_start_code(data, size, nal_start);
                if (nal_start < next_start_code && next_start_code <= size)
                {
                    std::vector<std::uint8_t> nal{data + nal_start, data + next_start_code};
                    while (!nal.empty() && nal.back() == 0U)
                    {
                        nal.pop_back();
                    }

                    switch (hevc_nal_type(nal))
                    {
                    case OBS_HEVC_NAL_VPS:
                        append_unique(sets.vps, std::move(nal));
                        break;
                    case OBS_HEVC_NAL_SPS:
                        append_unique(sets.sps, std::move(nal));
                        break;
                    case OBS_HEVC_NAL_PPS:
                        append_unique(sets.pps, std::move(nal));
                        break;
                    default:
                        break;
                    }
                }
                start_code = next_start_code;
            }
            return sets;
        }

        struct HevcProfileTierLevel
        {
            std::uint8_t profile_byte = 0x01U;
            std::uint32_t compatibility_flags = 0U;
            std::array<std::uint8_t, 6U> constraint_indicator_flags{};
            std::uint8_t level_idc = 0U;
            std::uint8_t num_temporal_layers = 1U;
            bool temporal_id_nested = true;
        };

        [[nodiscard]] bool parse_profile_tier_level_from_sps(const std::vector<std::uint8_t> &sps,
                                                             HevcProfileTierLevel &profile) noexcept
        {
            const std::vector<std::uint8_t> rbsp = rbsp_from_nal(sps);
            if (rbsp.size() < 15U)
            {
                return false;
            }

            profile.num_temporal_layers = static_cast<std::uint8_t>(((rbsp[2U] >> 1U) & 0x07U) + 1U);
            profile.temporal_id_nested = (rbsp[2U] & 0x01U) != 0U;
            profile.profile_byte = rbsp[3U];
            profile.compatibility_flags = (static_cast<std::uint32_t>(rbsp[4U]) << 24U) |
                                          (static_cast<std::uint32_t>(rbsp[5U]) << 16U) |
                                          (static_cast<std::uint32_t>(rbsp[6U]) << 8U) |
                                          static_cast<std::uint32_t>(rbsp[7U]);
            std::copy_n(rbsp.begin() + 8U, profile.constraint_indicator_flags.size(),
                        profile.constraint_indicator_flags.begin());
            profile.level_idc = rbsp[14U];
            return true;
        }

        [[nodiscard]] bool build_hvcc(const std::vector<std::uint8_t> &annexb,
                                      std::vector<std::uint8_t> &hvcc,
                                      std::string *error_message)
        {
            const HevcParameterSets sets = parse_parameter_sets(annexb);
            if (sets.vps.empty() || sets.sps.empty() || sets.pps.empty())
            {
                assign_error(error_message, "Direct MP4 muxer could not find HEVC VPS/SPS/PPS headers");
                return false;
            }

            HevcProfileTierLevel profile{};
            if (!parse_profile_tier_level_from_sps(sets.sps.front(), profile))
            {
                assign_error(error_message, "Direct MP4 muxer could not parse the HEVC SPS profile");
                return false;
            }

            ByteWriter writer;
            writer.u8(1U);
            writer.u8(profile.profile_byte);
            writer.u32(profile.compatibility_flags);
            writer.raw(profile.constraint_indicator_flags.data(), profile.constraint_indicator_flags.size());
            writer.u8(profile.level_idc);
            writer.u16(0xF000U);
            writer.u8(0xFCU);
            writer.u8(0xFCU | 1U);
            writer.u8(0xF8U);
            writer.u8(0xF8U);
            writer.u16(0U);
            writer.u8(static_cast<std::uint8_t>(((profile.num_temporal_layers & 0x07U) << 3U) |
                                                (profile.temporal_id_nested ? 0x04U : 0U) | 0x03U));
            writer.u8(3U);

            const auto write_array = [&writer](std::uint8_t nal_type,
                                               const std::vector<std::vector<std::uint8_t>> &nals) {
                writer.u8(static_cast<std::uint8_t>(0x80U | (nal_type & 0x3FU)));
                writer.u16(static_cast<std::uint16_t>(nals.size()));
                for (const std::vector<std::uint8_t> &nal : nals)
                {
                    writer.u16(static_cast<std::uint16_t>(nal.size()));
                    writer.raw(nal.data(), nal.size());
                }
            };

            write_array(OBS_HEVC_NAL_VPS, sets.vps);
            write_array(OBS_HEVC_NAL_SPS, sets.sps);
            write_array(OBS_HEVC_NAL_PPS, sets.pps);
            hvcc = writer.bytes();
            return true;
        }

        struct Sample
        {
            std::uint64_t offset = 0U;
            std::uint32_t size = 0U;
            std::int64_t pts = 0;
            std::int64_t dts = 0;
            std::uint32_t duration = 0U;
            std::int32_t composition_offset = 0;
            bool keyframe = false;
        };

        [[nodiscard]] std::uint32_t fallback_sample_duration(const std::vector<Sample> &samples,
                                                             std::uint32_t timebase_num) noexcept
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
                gcd = timebase_num == 0U ? 1U : timebase_num;
            }
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(gcd, std::numeric_limits<std::uint32_t>::max()));
        }

        [[nodiscard]] bool finalize_sample_durations(std::vector<Sample> &samples,
                                                     std::uint32_t timebase_num,
                                                     std::uint64_t &track_duration,
                                                     std::string *error_message)
        {
            if (samples.empty())
            {
                assign_error(error_message, "Direct MP4 muxer received no HEVC packets");
                return false;
            }

            const std::uint32_t last_duration = fallback_sample_duration(samples, timebase_num);
            track_duration = 0U;
            for (std::size_t index = 0U; index < samples.size(); ++index)
            {
                std::uint32_t duration = last_duration;
                if (index + 1U < samples.size())
                {
                    const std::int64_t delta = samples[index + 1U].dts - samples[index].dts;
                    if (delta <= 0 || delta > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
                    {
                        assign_error(error_message, "Direct MP4 muxer requires strictly increasing HEVC DTS values");
                        return false;
                    }
                    duration = static_cast<std::uint32_t>(delta);
                }
                samples[index].duration = duration;
                track_duration += duration;

                const std::int64_t composition_offset = samples[index].pts - samples[index].dts;
                if (composition_offset < std::numeric_limits<std::int32_t>::min() ||
                    composition_offset > std::numeric_limits<std::int32_t>::max())
                {
                    assign_error(error_message, "Direct MP4 muxer HEVC composition offset is out of range");
                    return false;
                }
                samples[index].composition_offset = static_cast<std::int32_t>(composition_offset);
            }
            return true;
        }

        [[nodiscard]] std::uint64_t sample_duration_sum(const std::vector<Sample> &samples) noexcept
        {
            std::uint64_t duration = 0U;
            for (const Sample &sample : samples)
            {
                duration += sample.duration;
            }
            return duration;
        }

        [[nodiscard]] bool trim_samples_to_media_duration(std::vector<Sample> &samples,
                                                          std::int64_t first_dts,
                                                          std::uint64_t media_duration,
                                                          std::uint64_t &track_duration,
                                                          std::string *error_message)
        {
            if (samples.empty())
            {
                assign_error(error_message, "Direct MP4 muxer cannot trim an empty sample table");
                return false;
            }
            if (media_duration == 0U)
            {
                assign_error(error_message, "Direct MP4 muxer cannot trim to an empty media duration");
                return false;
            }

            std::size_t keep_count = 0U;
            for (std::size_t index = 0U; index < samples.size(); ++index)
            {
                const std::int64_t media_dts = samples[index].dts - first_dts;
                if (media_dts < 0)
                {
                    assign_error(error_message, "Direct MP4 muxer sample DTS precedes the first sample DTS");
                    return false;
                }
                if (static_cast<std::uint64_t>(media_dts) >= media_duration)
                {
                    break;
                }
                keep_count = index + 1U;
            }
            if (keep_count == 0U)
            {
                assign_error(error_message, "Direct MP4 visible range contains no muxed HEVC samples");
                return false;
            }

            samples.resize(keep_count);
            track_duration = sample_duration_sum(samples);
            if (track_duration < media_duration)
            {
                assign_error(error_message, "Direct MP4 visible range is not covered by the muxed HEVC samples");
                return false;
            }
            if (track_duration > media_duration)
            {
                Sample &last_sample = samples.back();
                const std::uint64_t duration_before_last = track_duration - last_sample.duration;
                if (duration_before_last >= media_duration)
                {
                    assign_error(error_message, "Direct MP4 visible range does not end on a sample boundary");
                    return false;
                }
                const std::uint64_t trimmed_last_duration = media_duration - duration_before_last;
                if (trimmed_last_duration == 0U ||
                    trimmed_last_duration > std::numeric_limits<std::uint32_t>::max())
                {
                    assign_error(error_message, "Direct MP4 trimmed sample duration is out of range");
                    return false;
                }
                last_sample.duration = static_cast<std::uint32_t>(trimmed_last_duration);
                track_duration = media_duration;
            }
            return true;
        }

        void write_mvhd(ByteWriter &writer, std::uint64_t creation_time,
                        std::uint32_t movie_timescale,
                        std::uint64_t movie_duration)
        {
            const std::size_t start = writer.begin_full_box("mvhd", 0, 0);
            writer.u32(static_cast<std::uint32_t>(creation_time));
            writer.u32(static_cast<std::uint32_t>(creation_time));
            writer.u32(movie_timescale);
            writer.u32(static_cast<std::uint32_t>(std::min<std::uint64_t>(movie_duration, std::numeric_limits<std::uint32_t>::max())));
            writer.u32(0x00010000U);
            writer.u16(0x0100U);
            writer.u16(0U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0x00010000U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0x00010000U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0x40000000U);
            writer.zeros(24U);
            writer.u32(2U);
            writer.end_box(start);
        }

        void write_tkhd(ByteWriter &writer, std::uint64_t creation_time, std::uint64_t movie_duration,
                        std::uint32_t width, std::uint32_t height)
        {
            const std::size_t start = writer.begin_full_box("tkhd", 0, 0x000007U);
            writer.u32(static_cast<std::uint32_t>(creation_time));
            writer.u32(static_cast<std::uint32_t>(creation_time));
            writer.u32(1U);
            writer.u32(0U);
            writer.u32(static_cast<std::uint32_t>(std::min<std::uint64_t>(movie_duration, std::numeric_limits<std::uint32_t>::max())));
            writer.u32(0U);
            writer.u32(0U);
            writer.u16(0U);
            writer.u16(0U);
            writer.u16(0U);
            writer.u16(0U);
            writer.u32(0x00010000U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0x00010000U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0x40000000U);
            writer.u32(width << 16U);
            writer.u32(height << 16U);
            writer.end_box(start);
        }

        void write_elst(ByteWriter &writer, std::uint64_t segment_duration, std::int64_t media_time)
        {
            const bool version1 = segment_duration > std::numeric_limits<std::uint32_t>::max() ||
                                  media_time > std::numeric_limits<std::int32_t>::max();
            const std::size_t start = writer.begin_full_box("elst", version1 ? 1U : 0U, 0);
            writer.u32(1U);
            if (version1)
            {
                writer.u64(segment_duration);
                writer.i64(media_time);
            }
            else
            {
                writer.u32(static_cast<std::uint32_t>(segment_duration));
                writer.i32(static_cast<std::int32_t>(media_time));
            }
            writer.u16(1U);
            writer.u16(0U);
            writer.end_box(start);
        }

        void write_edts(ByteWriter &writer, std::uint64_t segment_duration, std::int64_t media_time)
        {
            const std::size_t start = writer.begin_box("edts");
            write_elst(writer, segment_duration, media_time);
            writer.end_box(start);
        }

        void write_mdhd(ByteWriter &writer, std::uint64_t creation_time, std::uint32_t timescale,
                        std::uint64_t track_duration)
        {
            const std::size_t start = writer.begin_full_box("mdhd", 0, 0);
            writer.u32(static_cast<std::uint32_t>(creation_time));
            writer.u32(static_cast<std::uint32_t>(creation_time));
            writer.u32(timescale);
            writer.u32(static_cast<std::uint32_t>(std::min<std::uint64_t>(track_duration, std::numeric_limits<std::uint32_t>::max())));
            writer.u16(0x55C4U);
            writer.u16(0U);
            writer.end_box(start);
        }

        void write_hdlr(ByteWriter &writer)
        {
            const std::size_t start = writer.begin_full_box("hdlr", 0, 0);
            writer.u32(0U);
            writer.fourcc("vide");
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0U);
            constexpr char name[] = "VideoHandler";
            writer.raw(name, sizeof(name));
            writer.end_box(start);
        }

        void write_vmhd(ByteWriter &writer)
        {
            const std::size_t start = writer.begin_full_box("vmhd", 0, 1U);
            writer.u16(0U);
            writer.u16(0U);
            writer.u16(0U);
            writer.u16(0U);
            writer.end_box(start);
        }

        void write_dinf(ByteWriter &writer)
        {
            const std::size_t dinf = writer.begin_box("dinf");
            const std::size_t dref = writer.begin_full_box("dref", 0, 0);
            writer.u32(1U);
            const std::size_t url = writer.begin_full_box("url ", 0, 1U);
            writer.end_box(url);
            writer.end_box(dref);
            writer.end_box(dinf);
        }

        void write_hvc1(ByteWriter &writer, std::uint32_t width, std::uint32_t height,
                        const std::vector<std::uint8_t> &hvcc)
        {
            const std::size_t hvc1 = writer.begin_box("hvc1");
            writer.zeros(6U);
            writer.u16(1U);
            writer.u16(0U);
            writer.u16(0U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u32(0U);
            writer.u16(static_cast<std::uint16_t>(width));
            writer.u16(static_cast<std::uint16_t>(height));
            writer.u32(0x00480000U);
            writer.u32(0x00480000U);
            writer.u32(0U);
            writer.u16(1U);
            writer.zeros(32U);
            writer.u16(0x0018U);
            writer.u16(0xFFFFU);
            const std::size_t hvcc_box = writer.begin_box("hvcC");
            writer.raw(hvcc.data(), hvcc.size());
            writer.end_box(hvcc_box);
            writer.end_box(hvc1);
        }

        void write_stsd(ByteWriter &writer, std::uint32_t width, std::uint32_t height,
                        const std::vector<std::uint8_t> &hvcc)
        {
            const std::size_t start = writer.begin_full_box("stsd", 0, 0);
            writer.u32(1U);
            write_hvc1(writer, width, height, hvcc);
            writer.end_box(start);
        }

        void write_stts(ByteWriter &writer, const std::vector<Sample> &samples)
        {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
            for (const Sample &sample : samples)
            {
                if (entries.empty() || entries.back().second != sample.duration)
                {
                    entries.emplace_back(1U, sample.duration);
                }
                else
                {
                    ++entries.back().first;
                }
            }

            const std::size_t start = writer.begin_full_box("stts", 0, 0);
            writer.u32(static_cast<std::uint32_t>(entries.size()));
            for (const auto &[count, duration] : entries)
            {
                writer.u32(count);
                writer.u32(duration);
            }
            writer.end_box(start);
        }

        void write_ctts(ByteWriter &writer, const std::vector<Sample> &samples)
        {
            std::vector<std::pair<std::uint32_t, std::int32_t>> entries;
            bool has_offset = false;
            for (const Sample &sample : samples)
            {
                has_offset = has_offset || sample.composition_offset != 0;
                if (entries.empty() || entries.back().second != sample.composition_offset)
                {
                    entries.emplace_back(1U, sample.composition_offset);
                }
                else
                {
                    ++entries.back().first;
                }
            }
            if (!has_offset)
            {
                return;
            }

            const std::size_t start = writer.begin_full_box("ctts", 1U, 0);
            writer.u32(static_cast<std::uint32_t>(entries.size()));
            for (const auto &[count, offset] : entries)
            {
                writer.u32(count);
                writer.i32(offset);
            }
            writer.end_box(start);
        }

        void write_stss(ByteWriter &writer, const std::vector<Sample> &samples)
        {
            std::vector<std::uint32_t> sync_samples;
            for (std::size_t index = 0U; index < samples.size(); ++index)
            {
                if (samples[index].keyframe)
                {
                    sync_samples.push_back(static_cast<std::uint32_t>(index + 1U));
                }
            }
            if (sync_samples.empty())
            {
                return;
            }

            const std::size_t start = writer.begin_full_box("stss", 0, 0);
            writer.u32(static_cast<std::uint32_t>(sync_samples.size()));
            for (const std::uint32_t sample_index : sync_samples)
            {
                writer.u32(sample_index);
            }
            writer.end_box(start);
        }

        void write_stsc(ByteWriter &writer, const std::vector<Sample> &samples)
        {
            const std::size_t start = writer.begin_full_box("stsc", 0, 0);
            writer.u32(1U);
            writer.u32(1U);
            writer.u32(static_cast<std::uint32_t>(samples.size()));
            writer.u32(1U);
            writer.end_box(start);
        }

        void write_stsz(ByteWriter &writer, const std::vector<Sample> &samples)
        {
            const std::size_t start = writer.begin_full_box("stsz", 0, 0);
            writer.u32(0U);
            writer.u32(static_cast<std::uint32_t>(samples.size()));
            for (const Sample &sample : samples)
            {
                writer.u32(sample.size);
            }
            writer.end_box(start);
        }

        void write_co64(ByteWriter &writer, std::uint64_t first_sample_offset)
        {
            const std::size_t start = writer.begin_full_box("co64", 0, 0);
            writer.u32(1U);
            writer.u64(first_sample_offset);
            writer.end_box(start);
        }

        void write_stbl(ByteWriter &writer, std::uint32_t width, std::uint32_t height,
                        const std::vector<std::uint8_t> &hvcc,
                        const std::vector<Sample> &samples)
        {
            const std::size_t start = writer.begin_box("stbl");
            write_stsd(writer, width, height, hvcc);
            write_stts(writer, samples);
            write_ctts(writer, samples);
            write_stss(writer, samples);
            write_stsc(writer, samples);
            write_stsz(writer, samples);
            write_co64(writer, samples.front().offset);
            writer.end_box(start);
        }

        void write_minf(ByteWriter &writer, std::uint32_t width, std::uint32_t height,
                        const std::vector<std::uint8_t> &hvcc,
                        const std::vector<Sample> &samples)
        {
            const std::size_t start = writer.begin_box("minf");
            write_vmhd(writer);
            write_dinf(writer);
            write_stbl(writer, width, height, hvcc, samples);
            writer.end_box(start);
        }

        void write_mdia(ByteWriter &writer, std::uint64_t creation_time, std::uint32_t timescale,
                        std::uint64_t track_duration, std::uint32_t width, std::uint32_t height,
                        const std::vector<std::uint8_t> &hvcc,
                        const std::vector<Sample> &samples)
        {
            const std::size_t start = writer.begin_box("mdia");
            write_mdhd(writer, creation_time, timescale, track_duration);
            write_hdlr(writer);
            write_minf(writer, width, height, hvcc, samples);
            writer.end_box(start);
        }

        void write_trak(ByteWriter &writer, std::uint64_t creation_time, std::uint64_t movie_duration,
                        std::uint32_t timescale, std::uint64_t track_duration,
                        std::int64_t edit_media_time, bool has_edit_range,
                        std::uint32_t width, std::uint32_t height,
                        const std::vector<std::uint8_t> &hvcc,
                        const std::vector<Sample> &samples)
        {
            const std::size_t start = writer.begin_box("trak");
            write_tkhd(writer, creation_time, movie_duration, width, height);
            if (has_edit_range)
            {
                write_edts(writer, movie_duration, edit_media_time);
            }
            write_mdia(writer, creation_time, timescale, track_duration, width, height, hvcc, samples);
            writer.end_box(start);
        }

        [[nodiscard]] std::vector<std::uint8_t> build_moov(std::uint64_t creation_time,
                                                           std::uint32_t movie_timescale,
                                                           std::uint64_t movie_duration,
                                                           std::uint32_t timescale,
                                                           std::uint64_t track_duration,
                                                           std::int64_t edit_media_time,
                                                           bool has_edit_range,
                                                           std::uint32_t width,
                                                           std::uint32_t height,
                                                           const std::vector<std::uint8_t> &hvcc,
                                                           const std::vector<Sample> &samples)
        {
            ByteWriter writer;
            const std::size_t start = writer.begin_box("moov");
            write_mvhd(writer, creation_time, movie_timescale, movie_duration);
            write_trak(writer, creation_time, movie_duration, timescale, track_duration,
                       edit_media_time, has_edit_range, width, height, hvcc, samples);
            writer.end_box(start);
            return writer.bytes();
        }

    } // namespace

    struct DirectMp4Muxer::Impl
    {
        std::filesystem::path path{};
        serializer mp4_serializer{};
        bool serializer_open = false;
        bool accepting_packets = false;
        bool header_written = false;
        bool first_packet = true;
        bool quicktime_flavor = false;
        obs_encoder_t *encoder = nullptr;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::uint32_t timebase_num = 1U;
        std::uint32_t timebase_den = 60U;
        std::uint64_t creation_time = 0U;
        std::uint64_t mdat_header_offset = 0U;
        std::uint64_t mdat_payload_offset = 0U;
        std::uint64_t mdat_payload_size = 0U;
        bool has_pending_visible_range = false;
        AlphaVisiblePacketRange pending_visible_range{};
        std::vector<std::uint8_t> header_annexb{};
        std::vector<Sample> samples{};
        DirectMp4MuxerStats stats{};

        void reset_state() noexcept
        {
            path.clear();
            accepting_packets = false;
            header_written = false;
            first_packet = true;
            quicktime_flavor = false;
            width = 0U;
            height = 0U;
            timebase_num = 1U;
            timebase_den = 60U;
            creation_time = 0U;
            mdat_header_offset = 0U;
            mdat_payload_offset = 0U;
            mdat_payload_size = 0U;
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

        [[nodiscard]] bool write_file_header(std::string *error_message)
        {
            if (header_written)
            {
                return true;
            }

            ByteWriter ftyp;
            const std::size_t start = ftyp.begin_box("ftyp");
            if (quicktime_flavor)
            {
                ftyp.fourcc("qt  ");
                ftyp.u32(0x00000200U);
                ftyp.fourcc("qt  ");
                ftyp.fourcc("hvc1");
            }
            else
            {
                ftyp.fourcc("isom");
                ftyp.u32(0x00000200U);
                ftyp.fourcc("isom");
                ftyp.fourcc("iso2");
                ftyp.fourcc("mp41");
                ftyp.fourcc("hvc1");
            }
            ftyp.end_box(start);
            if (!serializer_write_all(mp4_serializer, ftyp.bytes().data(), ftyp.bytes().size()))
            {
                assign_error(error_message, "could not write the Direct MP4 ftyp box");
                return false;
            }

            const int64_t mdat_pos = serializer_get_pos(&mp4_serializer);
            if (mdat_pos < 0)
            {
                assign_error(error_message, "could not query the Direct MP4 output position");
                return false;
            }
            mdat_header_offset = static_cast<std::uint64_t>(mdat_pos);
            if (!serializer_write_u32(mp4_serializer, 1U) ||
                !serializer_write_all(mp4_serializer, "mdat", 4U) ||
                !serializer_write_u64(mp4_serializer, 0U))
            {
                assign_error(error_message, "could not write the Direct MP4 mdat header");
                return false;
            }
            const int64_t payload_pos = serializer_get_pos(&mp4_serializer);
            if (payload_pos < 0)
            {
                assign_error(error_message, "could not query the Direct MP4 mdat payload position");
                return false;
            }
            mdat_payload_offset = static_cast<std::uint64_t>(payload_pos);
            header_written = true;
            return true;
        }

        void capture_headers_from_annexb(const std::uint8_t *data, std::size_t size)
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

        [[nodiscard]] bool ensure_headers(std::string *error_message)
        {
            if (!header_annexb.empty())
            {
                return true;
            }
            if (encoder == nullptr)
            {
                assign_error(error_message, "Direct MP4 muxer has no HEVC encoder");
                return false;
            }

            std::uint8_t *extra_data = nullptr;
            std::size_t extra_size = 0U;
            if (obs_encoder_get_extra_data(encoder, &extra_data, &extra_size) && extra_data != nullptr && extra_size > 0U)
            {
                capture_headers_from_annexb(extra_data, extra_size);
            }
            if (header_annexb.empty())
            {
                assign_error(error_message, "Direct MP4 muxer could not read HEVC encoder headers");
                return false;
            }
            return true;
        }
    };

    DirectMp4Muxer::DirectMp4Muxer()
        : impl_(std::make_unique<Impl>())
    {
    }

    DirectMp4Muxer::~DirectMp4Muxer() noexcept
    {
        abort();
    }

    bool DirectMp4Muxer::open(const DirectMp4MuxerConfig &config,
                              std::string *error_message)
    {
        abort();
        impl_->reset_state();

        if (config.path.empty())
        {
            assign_error(error_message, "Direct MP4 output path is empty");
            return false;
        }

        const std::filesystem::path parent = config.path.parent_path();
        if (!parent.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                assign_error(error_message, "could not create the Direct MP4 output directory");
                return false;
            }
        }

        const std::string output_path = path_to_utf8(config.path);
        if (!buffered_file_serializer_init_defaults(&impl_->mp4_serializer, output_path.c_str()))
        {
            assign_error(error_message, "could not open the Direct MP4 mux output");
            return false;
        }

        impl_->path = config.path;
        impl_->quicktime_flavor = config.quicktime_flavor;
        impl_->serializer_open = true;
        return true;
    }

    bool DirectMp4Muxer::begin(obs_output_t *output, std::string *error_message)
    {
        if (!impl_->serializer_open)
        {
            assign_error(error_message, "Direct MP4 output storage is not open");
            return false;
        }
        if (output == nullptr)
        {
            assign_error(error_message, "Direct MP4 output cannot mux without an OBS output");
            return false;
        }
        if (impl_->accepting_packets)
        {
            return true;
        }

        obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0U);
        if (encoder == nullptr)
        {
            assign_error(error_message, "Direct MP4 output could not find the OBS video encoder");
            return false;
        }
        const char *codec = obs_encoder_get_codec(encoder);
        if (codec == nullptr || std::strcmp(codec, "hevc") != 0)
        {
            assign_error(error_message, "Direct MP4 output only supports HEVC video packets");
            return false;
        }

        impl_->release_encoder();
        impl_->encoder = obs_encoder_get_ref(encoder);
        impl_->width = obs_encoder_get_width(encoder);
        impl_->height = obs_encoder_get_height(encoder);
        if (impl_->width == 0U || impl_->height == 0U ||
            impl_->width > std::numeric_limits<std::uint16_t>::max() ||
            impl_->height > std::numeric_limits<std::uint16_t>::max())
        {
            assign_error(error_message, "Direct MP4 output received an unsupported video size");
            return false;
        }

        impl_->creation_time = static_cast<std::uint64_t>(std::time(nullptr)) + kMp4EpochOffset;
        if (!impl_->write_file_header(error_message))
        {
            return false;
        }
        impl_->accepting_packets = true;
        return true;
    }

    bool DirectMp4Muxer::set_visible_range(const AlphaVisiblePacketRange &range,
                                           std::string *error_message)
    {
        if (range.media_time < 0 || range.duration < 0)
        {
            assign_error(error_message, "Direct MP4 visible range cannot be negative");
            return false;
        }

        impl_->pending_visible_range = range;
        impl_->has_pending_visible_range = range.duration > 0;
        return true;
    }

    bool DirectMp4Muxer::submit_packet(encoder_packet *packet, std::string *error_message)
    {
        if (!impl_->accepting_packets || packet == nullptr || packet->type != OBS_ENCODER_VIDEO)
        {
            return true;
        }
        if (impl_->encoder != nullptr && packet->encoder != nullptr && packet->encoder != impl_->encoder)
        {
            return true;
        }
        if (!impl_->header_written && !impl_->write_file_header(error_message))
        {
            return false;
        }
        if (packet->timebase_den <= 0 || packet->timebase_num <= 0)
        {
            assign_error(error_message, "Direct MP4 muxer received an invalid packet timebase");
            return false;
        }
        if (packet->size > std::numeric_limits<std::uint32_t>::max())
        {
            assign_error(error_message, "Direct MP4 muxer received an oversized HEVC packet");
            return false;
        }

        impl_->capture_headers_from_annexb(packet->data, packet->size);

        encoder_packet parsed_packet{};
        obs_parse_hevc_packet(&parsed_packet, packet);
        if (parsed_packet.data == nullptr || parsed_packet.size == 0U)
        {
            obs_encoder_packet_release(&parsed_packet);
            return true;
        }
        if (parsed_packet.size > std::numeric_limits<std::uint32_t>::max())
        {
            obs_encoder_packet_release(&parsed_packet);
            assign_error(error_message, "Direct MP4 muxer parsed an oversized HEVC packet");
            return false;
        }

        if (impl_->first_packet)
        {
            impl_->timebase_num = static_cast<std::uint32_t>(packet->timebase_num);
            impl_->timebase_den = static_cast<std::uint32_t>(packet->timebase_den);
            impl_->stats.first_pts = packet->pts;
            impl_->first_packet = false;
        }
        else if (impl_->timebase_num != static_cast<std::uint32_t>(packet->timebase_num) ||
                 impl_->timebase_den != static_cast<std::uint32_t>(packet->timebase_den))
        {
            obs_encoder_packet_release(&parsed_packet);
            assign_error(error_message, "Direct MP4 muxer received mixed HEVC packet timebases");
            return false;
        }

        const int64_t offset = serializer_get_pos(&impl_->mp4_serializer);
        if (offset < 0)
        {
            obs_encoder_packet_release(&parsed_packet);
            assign_error(error_message, "Direct MP4 muxer could not query the output packet offset");
            return false;
        }
        if (!serializer_write_all(impl_->mp4_serializer, parsed_packet.data, parsed_packet.size))
        {
            obs_encoder_packet_release(&parsed_packet);
            assign_error(error_message, "Direct MP4 muxer could not write an HEVC packet");
            return false;
        }

        Sample sample{};
        sample.offset = static_cast<std::uint64_t>(offset);
        sample.size = static_cast<std::uint32_t>(parsed_packet.size);
        sample.pts = packet->pts;
        sample.dts = packet->dts;
        sample.keyframe = parsed_packet.keyframe || packet->keyframe;
        impl_->samples.push_back(sample);

        impl_->stats.last_pts = packet->pts;
        ++impl_->stats.packet_count;
        ++impl_->stats.muxed_packet_count;
        if (sample.keyframe)
        {
            ++impl_->stats.keyframe_count;
        }
        impl_->stats.packet_bytes += static_cast<std::uint64_t>(sample.size);
        impl_->mdat_payload_size += static_cast<std::uint64_t>(sample.size);
        obs_encoder_packet_release(&parsed_packet);
        return true;
    }

    bool DirectMp4Muxer::finalize(std::string *error_message)
    {
        impl_->accepting_packets = false;
        if (!impl_->serializer_open || impl_->stats.finalized)
        {
            return true;
        }
        if (!impl_->ensure_headers(error_message))
        {
            return false;
        }

        std::uint64_t track_duration = 0U;
        if (!finalize_sample_durations(impl_->samples, impl_->timebase_num, track_duration, error_message))
        {
            return false;
        }

        std::vector<std::uint8_t> hvcc;
        if (!build_hvcc(impl_->header_annexb, hvcc, error_message))
        {
            return false;
        }

        std::vector<Sample> table_samples = impl_->samples;
        std::uint64_t table_track_duration = track_duration;
        std::int64_t edit_media_time = 0;
        std::uint64_t visible_duration = track_duration;
        const bool has_edit_range = impl_->has_pending_visible_range;
        if (has_edit_range)
        {
            const std::int64_t first_dts = impl_->samples.front().dts;
            const std::int64_t media_time = impl_->pending_visible_range.media_time - first_dts;
            if (media_time < 0)
            {
                assign_error(error_message, "Direct MP4 visible range starts before the first muxed sample");
                return false;
            }
            edit_media_time = media_time;
            visible_duration = static_cast<std::uint64_t>(impl_->pending_visible_range.duration);
            if (visible_duration == 0U ||
                static_cast<std::uint64_t>(edit_media_time) >
                    std::numeric_limits<std::uint64_t>::max() - visible_duration ||
                static_cast<std::uint64_t>(edit_media_time) + visible_duration > track_duration)
            {
                assign_error(error_message, "Direct MP4 visible range is outside the muxed HEVC samples");
                return false;
            }
            const std::uint64_t visible_end = static_cast<std::uint64_t>(edit_media_time) + visible_duration;
            if (!trim_samples_to_media_duration(table_samples, first_dts, visible_end, table_track_duration,
                                                error_message))
            {
                return false;
            }
        }

        const bool write_edit_range = has_edit_range &&
                                      (edit_media_time != 0 ||
                                       visible_duration != table_track_duration);
        const std::uint32_t movie_timescale = kMovieTimescale;
        const std::uint64_t movie_duration = scale_u64(visible_duration, movie_timescale, impl_->timebase_den);
        const std::vector<std::uint8_t> moov =
            build_moov(impl_->creation_time, movie_timescale, movie_duration, impl_->timebase_den,
                       table_track_duration, edit_media_time, write_edit_range, impl_->width, impl_->height,
                       hvcc, table_samples);

        const int64_t end_pos = serializer_get_pos(&impl_->mp4_serializer);
        if (end_pos < 0)
        {
            assign_error(error_message, "Direct MP4 muxer could not query the output end position");
            return false;
        }
        if (serializer_seek(&impl_->mp4_serializer, static_cast<std::int64_t>(impl_->mdat_header_offset),
                            SERIALIZE_SEEK_START) < 0 ||
            !serializer_write_u32(impl_->mp4_serializer, 1U) ||
            !serializer_write_all(impl_->mp4_serializer, "mdat", 4U) ||
            !serializer_write_u64(impl_->mp4_serializer, impl_->mdat_payload_size + 16U) ||
            serializer_seek(&impl_->mp4_serializer, end_pos, SERIALIZE_SEEK_START) < 0)
        {
            assign_error(error_message, "Direct MP4 muxer could not finalize the mdat header");
            return false;
        }
        if (!serializer_write_all(impl_->mp4_serializer, moov.data(), moov.size()))
        {
            assign_error(error_message, "Direct MP4 muxer could not write the moov box");
            return false;
        }

        impl_->stats.finalized = true;
        return true;
    }

    void DirectMp4Muxer::close_storage() noexcept
    {
        impl_->accepting_packets = false;
        impl_->release_encoder();
        if (impl_->serializer_open)
        {
            buffered_file_serializer_free(&impl_->mp4_serializer);
            impl_->serializer_open = false;
        }
    }

    void DirectMp4Muxer::abort() noexcept
    {
        close_storage();
    }

    bool DirectMp4Muxer::is_open() const noexcept
    {
        return impl_->serializer_open;
    }

    bool DirectMp4Muxer::is_accepting_packets() const noexcept
    {
        return impl_->accepting_packets;
    }

    bool DirectMp4Muxer::supports_visible_range() const noexcept
    {
        return true;
    }

    const std::filesystem::path &DirectMp4Muxer::path() const noexcept
    {
        return impl_->path;
    }

    const DirectMp4MuxerStats &DirectMp4Muxer::stats() const noexcept
    {
        return impl_->stats;
    }

} // namespace alpha_recorder::obs
