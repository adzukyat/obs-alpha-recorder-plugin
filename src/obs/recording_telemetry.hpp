#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>

namespace alpha_recorder::obs
{

    [[nodiscard]] inline std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start,
                                                  std::chrono::steady_clock::time_point end) noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    struct TimingSummary
    {
        std::uint64_t count = 0;
        std::uint64_t total_ns = 0;
        std::uint64_t max_ns = 0;

        void add(std::uint64_t ns) noexcept
        {
            ++count;
            total_ns += ns;
            max_ns = std::max(max_ns, ns);
        }
    };

    [[nodiscard]] inline std::uint64_t abs_i64_to_u64(std::int64_t value) noexcept
    {
        if (value >= 0)
        {
            return static_cast<std::uint64_t>(value);
        }
        if (value == std::numeric_limits<std::int64_t>::min())
        {
            return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL;
        }
        return static_cast<std::uint64_t>(-value);
    }

    struct SignedDeltaSummary
    {
        std::uint64_t count = 0;
        std::uint64_t abs_total_ns = 0;
        std::uint64_t abs_max_ns = 0;
        double signed_total_ns = 0.0;
        std::int64_t signed_min_ns = 0;
        std::int64_t signed_max_ns = 0;

        void add(std::int64_t ns) noexcept
        {
            const std::uint64_t abs_ns = abs_i64_to_u64(ns);
            if (count == 0U)
            {
                signed_min_ns = ns;
                signed_max_ns = ns;
            }
            else
            {
                signed_min_ns = std::min(signed_min_ns, ns);
                signed_max_ns = std::max(signed_max_ns, ns);
            }

            ++count;
            abs_total_ns += abs_ns;
            abs_max_ns = std::max(abs_max_ns, abs_ns);
            signed_total_ns += static_cast<double>(ns);
        }
    };

    struct CaptureTiming
    {
        bool mapped = false;
        std::uint64_t map_ns = 0;
        std::uint64_t render_ns = 0;
        std::uint64_t stage_ns = 0;
        std::uint64_t total_ns = 0;
    };

    struct LivePipelineTelemetry
    {
        TimingSummary capture_total{};
        TimingSummary capture_map{};
        TimingSummary capture_render{};
        TimingSummary capture_stage{};
        TimingSummary alignment_batch{};
        SignedDeltaSummary alignment_output_cts_delta{};
        SignedDeltaSummary alignment_alpha_content_delta{};
        SignedDeltaSummary packet_fer_cts_delta{};
        SignedDeltaSummary packet_cts_delta{};
        SignedDeltaSummary packet_fer_delta{};
        std::uint64_t texture_stall_corrections = 0;
        std::uint64_t rendered_callbacks = 0;
        std::uint64_t captured_frames = 0;
        std::uint64_t queued_alpha_frames = 0;
        std::uint64_t raw_video_frames = 0;
        std::uint64_t packet_frames = 0;
        std::uint64_t aligned_frames = 0;
        std::uint64_t alignment_repeated_frames = 0;
        std::uint64_t alignment_missing_output_repeats = 0;
        std::uint64_t alignment_missing_alpha_repeats = 0;
        std::uint64_t alignment_texture_stall_repeats = 0;
        std::uint64_t alignment_black_repeats = 0;
        std::uint64_t alignment_alpha_dropped_frames = 0;
        std::uint64_t alignment_output_dropped_frames = 0;
        std::uint64_t first_packet_cts = 0;
        std::uint64_t last_packet_cts = 0;
        std::uint64_t last_packet_cts_for_delta = 0;
        std::uint64_t last_packet_fer_for_delta = 0;
        std::uint64_t first_raw_output_timestamp = 0;
        std::uint64_t last_raw_output_timestamp = 0;
        std::uint64_t first_alpha_timestamp = 0;
        std::uint64_t last_alpha_timestamp = 0;
        std::size_t max_pending_alpha_frames = 0;
        std::size_t max_pending_output_frames = 0;
        std::size_t max_pending_encoded_frames = 0;

        void reset() noexcept
        {
            *this = {};
        }
    };

    [[nodiscard]] inline double ns_to_ms(std::uint64_t ns) noexcept
    {
        return static_cast<double>(ns) / 1000000.0;
    }

    [[nodiscard]] inline double average_ms(const TimingSummary &summary) noexcept
    {
        return summary.count == 0U ? 0.0 : ns_to_ms(summary.total_ns / summary.count);
    }

    [[nodiscard]] inline double signed_ns_to_ms(std::int64_t ns) noexcept
    {
        return static_cast<double>(ns) / 1000000.0;
    }

    [[nodiscard]] inline std::string format_timing_summary(const TimingSummary &summary)
    {
        char buffer[128];
        (void)std::snprintf(buffer, sizeof(buffer), "count=%llu avg_ms=%.3f max_ms=%.3f",
                            static_cast<unsigned long long>(summary.count), average_ms(summary), ns_to_ms(summary.max_ns));
        return std::string{buffer};
    }

    [[nodiscard]] inline std::string format_signed_delta_summary(const SignedDeltaSummary &summary)
    {
        char buffer[192];
        (void)std::snprintf(
            buffer, sizeof(buffer), "count=%llu avg_abs_ms=%.3f max_abs_ms=%.3f avg_signed_ms=%.3f min_signed_ms=%.3f max_signed_ms=%.3f",
            static_cast<unsigned long long>(summary.count),
            summary.count == 0U ? 0.0 : ns_to_ms(summary.abs_total_ns / summary.count),
            ns_to_ms(summary.abs_max_ns),
            summary.count == 0U ? 0.0 : (summary.signed_total_ns / static_cast<double>(summary.count)) / 1000000.0,
            signed_ns_to_ms(summary.signed_min_ns), signed_ns_to_ms(summary.signed_max_ns));
        return std::string{buffer};
    }

    [[nodiscard]] inline std::string format_bytes(std::uint64_t bytes)
    {
        char buffer[64];
        (void)std::snprintf(buffer, sizeof(buffer), "%.2fMiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return std::string{buffer};
    }

    [[nodiscard]] inline const char *bool_text(bool value) noexcept
    {
        return value ? "true" : "false";
    }

    [[nodiscard]] inline std::int64_t signed_timestamp_delta_ns(std::uint64_t selected,
                                                                std::uint64_t reference) noexcept
    {
        constexpr auto max_i64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (selected >= reference)
        {
            const std::uint64_t delta = selected - reference;
            return delta > max_i64 ? std::numeric_limits<std::int64_t>::max()
                                   : static_cast<std::int64_t>(delta);
        }

        const std::uint64_t delta = reference - selected;
        return delta > max_i64 ? std::numeric_limits<std::int64_t>::min() + 1
                               : -static_cast<std::int64_t>(delta);
    }

    inline void remember_timestamp_span(std::uint64_t timestamp,
                                        std::uint64_t &first_timestamp,
                                        std::uint64_t &last_timestamp) noexcept
    {
        if (timestamp == 0U)
        {
            return;
        }
        if (first_timestamp == 0U)
        {
            first_timestamp = timestamp;
        }
        last_timestamp = timestamp;
    }

    [[nodiscard]] inline double timestamp_span_ms(std::uint64_t first_timestamp,
                                                  std::uint64_t last_timestamp) noexcept
    {
        if (first_timestamp == 0U || last_timestamp == 0U)
        {
            return 0.0;
        }
        return signed_ns_to_ms(signed_timestamp_delta_ns(last_timestamp, first_timestamp));
    }

} // namespace alpha_recorder::obs
