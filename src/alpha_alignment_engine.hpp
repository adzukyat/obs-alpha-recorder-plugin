#pragma once

#include "recording_session_controller_cadence.hpp"
#include "recording_telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alpha_recorder::obs
{

    [[nodiscard]] std::size_t alignment_alpha_queue_frame_limit(std::uint32_t fps_num,
                                                                std::uint32_t fps_den) noexcept;
    [[nodiscard]] std::size_t alignment_output_queue_frame_limit(std::uint32_t fps_num,
                                                                 std::uint32_t fps_den) noexcept;
    [[nodiscard]] std::uint64_t plausible_alignment_delta_ns(std::uint32_t fps_num,
                                                             std::uint32_t fps_den) noexcept;

    struct EncodedAlphaFrame
    {
        std::int64_t pts = 0;
        std::uint64_t cts = 0U;
        std::uint64_t fer = 0U;
        std::uint64_t ferc = 0U;
        bool texture_encoded = false;
    };

    struct AlignmentTraceSelection
    {
        std::size_t output_index = 0U;
        std::size_t alpha_index = 0U;
        std::uint64_t output_delta = 0U;
        std::uint64_t alpha_delta = 0U;
        bool alpha_index_valid = false;
        bool repeated = false;
    };

    struct AlignmentTraceEvent
    {
        const char *reason = "";
        EncodedAlphaFrame encoded_frame{};
        OutputFrameCadence output_frame{};
        AlphaFrame alpha_frame{};
        AlignmentTraceSelection selection{};
        std::size_t alpha_queue_size = 0U;
        std::size_t output_queue_size = 0U;
        std::size_t encoded_queue_size = 0U;
        std::uint32_t consecutive_output_duplicate_frames = 0U;
    };

    struct AlphaAlignmentEngineConfig
    {
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::uint32_t fps_num = 0U;
        std::uint32_t fps_den = 1U;
        std::size_t alpha_queue_frame_limit = 240U;
        std::size_t output_queue_frame_limit = 240U;
        std::size_t encoded_reorder_frames = 16U;
    };

    struct AlphaAlignmentDrainResult
    {
        bool failed = false;
        bool drained_any = false;
        std::size_t drained_frames = 0U;
        std::string error_message{};
    };

    class AlphaAlignmentEngine
    {
    public:
        using WriteFrameCallback = std::function<bool(const AlphaFrame &frame,
                                                      bool &queued,
                                                      std::string &error_message)>;
        using TraceCallback = std::function<void(const AlignmentTraceEvent &event)>;

        void configure(const AlphaAlignmentEngineConfig &config) noexcept;
        [[nodiscard]] const AlphaAlignmentEngineConfig &config() const noexcept;

        void clear_pending() noexcept;
        void reset_frame_history() noexcept;
        void reset_all() noexcept;

        [[nodiscard]] bool has_work() const noexcept;
        [[nodiscard]] std::size_t pending_alpha_size() const noexcept;
        [[nodiscard]] std::size_t pending_output_size() const noexcept;

        void mark_pending_encoded_texture_encoded() noexcept;

        void remember_alpha_frame(AlphaFrame frame, LivePipelineTelemetry &telemetry);
        void remember_output_frame(OutputFrameCadence frame, LivePipelineTelemetry &telemetry);
        void queue_packet(std::int64_t pts,
                          std::uint64_t cts,
                          std::uint64_t fer,
                          std::uint64_t ferc,
                          bool texture_encoded,
                          LivePipelineTelemetry &telemetry);

        AlphaAlignmentDrainResult drain(bool drain_all,
                                        std::size_t max_frames,
                                        LivePipelineTelemetry &telemetry,
                                        const WriteFrameCallback &write_frame,
                                        const TraceCallback &trace);

    private:
        enum class RepeatReason
        {
            MissingOutput,
            MissingAlpha,
            TextureStall,
        };

        void trim_pending_alpha_frames(LivePipelineTelemetry &telemetry) noexcept;
        void trim_pending_output_frames(LivePipelineTelemetry &telemetry) noexcept;
        void discard_alpha_frames_through_timestamp(std::uint64_t timestamp,
                                                    LivePipelineTelemetry &telemetry) noexcept;

        [[nodiscard]] bool make_repeat_frame(AlphaFrame &frame,
                                             LivePipelineTelemetry &telemetry) noexcept;
        bool repeat_frame(RepeatReason reason,
                          AlphaFrame &frame,
                          LivePipelineTelemetry &telemetry,
                          std::string &error_message);
        bool resolve_output_alpha_frame(const EncodedAlphaFrame &encoded_frame,
                                        bool drain_all,
                                        AlphaFrame &frame,
                                        LivePipelineTelemetry &telemetry,
                                        const TraceCallback &trace,
                                        std::string &error_message);
        bool write_resolved_frame(const AlphaFrame &frame,
                                  LivePipelineTelemetry &telemetry,
                                  const WriteFrameCallback &write_frame,
                                  std::string &error_message);
        void trace_selection(const TraceCallback &trace,
                             const char *reason,
                             const EncodedAlphaFrame &encoded_frame,
                             const OutputFrameCadence &output_frame,
                             const AlphaFrame &alpha_frame,
                             const AlignmentTraceSelection &selection) const;

        AlphaAlignmentEngineConfig config_{};
        AlphaFrame last_alpha_frame_{};
        AlphaFrame last_captured_alpha_frame_{};
        std::shared_ptr<std::vector<std::uint8_t>> fallback_black_alpha_{};
        std::deque<AlphaFrame> pending_alpha_frames_{};
        std::deque<EncodedAlphaFrame> pending_encoded_alpha_frames_{};
        std::deque<OutputFrameCadence> pending_output_frames_{};
        std::uint32_t consecutive_output_duplicate_frames_ = 0U;
        std::uint64_t last_aligned_packet_cts_ = 0U;
    };

} // namespace alpha_recorder::obs
