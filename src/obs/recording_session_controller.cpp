#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"
#include "recording_session_controller_cadence.hpp"
#include "recording_session_controller_gate.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <graphics/vec4.h>
#include <util/bmem.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{

    using alpha_recorder::obs::AlphaMaskVideoWriter;

    constexpr std::string_view kAlphaExtractEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;

sampler_state def_sampler {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertInOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VertInOut VSDefault(VertInOut vert_in)
{
    VertInOut vert_out;
    vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv  = vert_in.uv;
    return vert_out;
}

float4 PSAlpha(VertInOut vert_in) : TARGET
{
    float alpha = image.Sample(def_sampler, vert_in.uv).a;
    return float4(alpha, alpha, alpha, 1.0);
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PSAlpha(vert_in);
    }
}
)";

    std::filesystem::path path_from_utf8(const char *text)
    {
        if (text == nullptr || *text == '\0')
        {
            return {};
        }

        return std::filesystem::u8path(text);
    }

    bool path_is_directory(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return false;
        }

        std::error_code error;
        return std::filesystem::is_directory(path, error) && !error;
    }

    std::filesystem::path recording_file_path_from_output(obs_output_t *recording_output)
    {
        if (recording_output == nullptr)
        {
            return {};
        }

        obs_data_t *settings = obs_output_get_settings(recording_output);
        if (settings == nullptr)
        {
            return {};
        }

        std::filesystem::path recording_path = path_from_utf8(obs_data_get_string(settings, "url"));
        if (recording_path.empty())
        {
            recording_path = path_from_utf8(obs_data_get_string(settings, "path"));
        }

        obs_data_release(settings);
        return path_is_directory(recording_path) ? std::filesystem::path{} : recording_path;
    }

    bool recording_output_uses_texture_encoder(obs_output_t *recording_output, enum video_format output_format)
    {
        if (recording_output == nullptr || (output_format != VIDEO_FORMAT_NV12 && output_format != VIDEO_FORMAT_P010))
        {
            return false;
        }

        obs_encoder_t *encoder = obs_output_get_video_encoder(recording_output);
        if (encoder == nullptr || (obs_encoder_get_caps(encoder) & OBS_ENCODER_CAP_PASS_TEXTURE) == 0U)
        {
            return false;
        }

        // The per-encoder texture query takes OBS's mix lock and can stall
        // texture-encoder stop/start when used from Alpha Recorder's live path.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        const bool texture_active = output_format == VIDEO_FORMAT_NV12 ? obs_nv12_tex_active() : obs_p010_tex_active();
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
        return texture_active;
    }

#ifdef _WIN32
    std::wstring utf8_to_wide(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }

        const int required_size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (required_size <= 0)
        {
            return {};
        }

        std::wstring wide(static_cast<std::size_t>(required_size), L'\0');
        (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), required_size);
        return wide;
    }
#endif

    void log_and_show_error(const std::string &message, bool show_popup)
    {
        blog(LOG_ERROR, "%s", message.c_str());

#ifdef _WIN32
        if (show_popup)
        {
            const std::wstring wide_message = utf8_to_wide(message);
            const std::wstring wide_title = L"Alpha Recorder";
            const HWND owner = static_cast<HWND>(obs_frontend_get_main_window_handle());
            MessageBoxW(owner, wide_message.empty() ? L"Alpha Recorder encountered an error." : wide_message.c_str(),
                        wide_title.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
        }
#else
        (void)show_popup;
#endif
    }

    void copy_alpha_plane(std::vector<std::uint8_t> &alpha,
                          const std::uint8_t *source,
                          std::uint32_t source_linesize,
                          std::uint32_t width,
                          std::uint32_t height)
    {
        const std::size_t alpha_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        alpha.resize(alpha_bytes);

        for (std::uint32_t row = 0; row < height; ++row)
        {
            const std::uint8_t *source_row = source + (static_cast<std::size_t>(row) * static_cast<std::size_t>(source_linesize));
            std::uint8_t *dest_row = alpha.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width));
            std::copy(source_row, source_row + width, dest_row);
        }
    }

    [[nodiscard]] std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start,
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

    struct CaptureTiming
    {
        bool mapped = false;
        std::uint64_t map_ns = 0;
        std::uint64_t render_ns = 0;
        std::uint64_t stage_ns = 0;
        std::uint64_t total_ns = 0;
    };

    [[nodiscard]] double ns_to_ms(std::uint64_t ns) noexcept
    {
        return static_cast<double>(ns) / 1000000.0;
    }

    [[nodiscard]] double average_ms(const TimingSummary &summary) noexcept
    {
        return summary.count == 0U ? 0.0 : ns_to_ms(summary.total_ns / summary.count);
    }

    [[nodiscard]] std::string format_timing_summary(const TimingSummary &summary)
    {
        char buffer[128];
        (void)std::snprintf(buffer, sizeof(buffer), "count=%llu avg_ms=%.3f max_ms=%.3f",
                            static_cast<unsigned long long>(summary.count), average_ms(summary), ns_to_ms(summary.max_ns));
        return std::string{buffer};
    }

    [[nodiscard]] std::string format_bytes(std::uint64_t bytes)
    {
        char buffer[64];
        (void)std::snprintf(buffer, sizeof(buffer), "%.2fMiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return std::string{buffer};
    }

    struct LivePipelineTelemetry
    {
        TimingSummary capture_total{};
        TimingSummary capture_map{};
        TimingSummary capture_render{};
        TimingSummary capture_stage{};
        TimingSummary alignment_batch{};
        std::uint64_t rendered_callbacks = 0;
        std::uint64_t captured_frames = 0;
        std::uint64_t queued_alpha_frames = 0;
        std::uint64_t raw_video_frames = 0;
        std::uint64_t packet_frames = 0;
        std::uint64_t aligned_frames = 0;
        std::size_t max_pending_alpha_frames = 0;
        std::size_t max_pending_output_frames = 0;
        std::size_t max_pending_encoded_frames = 0;

        void reset() noexcept
        {
            *this = {};
        }
    };

    class AlphaPlaneExtractor
    {
    public:
        bool ensure(std::uint32_t width, std::uint32_t height, std::string &error_message)
        {
            if (width == 0U || height == 0U)
            {
                error_message = "Alpha Recorder cannot capture a zero-sized OBS frame.";
                return false;
            }

            if (effect_ != nullptr && mask_texture_ != nullptr && stage_surfaces_ready() && width_ == width &&
                height_ == height)
            {
                return true;
            }

            destroy();

            char *effect_error = nullptr;
            effect_ = gs_effect_create(std::string{kAlphaExtractEffect}.c_str(), "alpha-recorder-alpha-extract.effect", &effect_error);
            if (effect_ == nullptr)
            {
                error_message = effect_error != nullptr ? std::string{effect_error}
                                                        : std::string{"Alpha Recorder could not create the alpha extraction shader."};
                if (effect_error != nullptr)
                {
                    bfree(effect_error);
                }
                return false;
            }

            image_param_ = gs_effect_get_param_by_name(effect_, "image");
            mask_texture_ = gs_texture_create(width, height, GS_R8, 1, nullptr, GS_RENDER_TARGET);
            for (gs_stagesurf_t *&stage_surface : stage_surfaces_)
            {
                stage_surface = gs_stagesurface_create(width, height, GS_R8);
            }

            if (image_param_ == nullptr || mask_texture_ == nullptr || !stage_surfaces_ready())
            {
                error_message = "Alpha Recorder could not allocate GPU resources for alpha extraction.";
                destroy();
                return false;
            }

            width_ = width;
            height_ = height;
            return true;
        }

        bool capture_latest(std::vector<std::uint8_t> &alpha,
                            std::uint64_t &timestamp,
                            std::string &error_message,
                            CaptureTiming *timing = nullptr)
        {
            const auto total_start = std::chrono::steady_clock::now();
            CaptureTiming local_timing{};
            alpha.clear();
            timestamp = 0U;

            gs_texture_t *program_texture = obs_get_main_texture();
            if (program_texture == nullptr)
            {
                if (timing != nullptr)
                {
                    local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
                    *timing = local_timing;
                }
                return true;
            }

            if (staged_surface_count_ == stage_surfaces_.size())
            {
                const auto map_start = std::chrono::steady_clock::now();
                map_staged_surface(next_map_surface_, alpha, error_message);
                local_timing.mapped = true;
                local_timing.map_ns = elapsed_ns(map_start, std::chrono::steady_clock::now());
                if (!error_message.empty())
                {
                    if (timing != nullptr)
                    {
                        local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
                        *timing = local_timing;
                    }
                    return false;
                }
                timestamp = staged_timestamps_[next_map_surface_];
                next_map_surface_ = (next_map_surface_ + 1U) % stage_surfaces_.size();
                --staged_surface_count_;
            }

            const auto render_start = std::chrono::steady_clock::now();
            if (!render_alpha_mask(program_texture))
            {
                if (timing != nullptr)
                {
                    local_timing.render_ns = elapsed_ns(render_start, std::chrono::steady_clock::now());
                    local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
                    *timing = local_timing;
                }
                return false;
            }
            local_timing.render_ns = elapsed_ns(render_start, std::chrono::steady_clock::now());

            const auto stage_start = std::chrono::steady_clock::now();
            gs_stage_texture(stage_surfaces_[next_stage_surface_], mask_texture_);
            staged_timestamps_[next_stage_surface_] = obs_get_video_frame_time();
            next_stage_surface_ = (next_stage_surface_ + 1U) % stage_surfaces_.size();
            ++staged_surface_count_;
            local_timing.stage_ns = elapsed_ns(stage_start, std::chrono::steady_clock::now());
            local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
            if (timing != nullptr)
            {
                *timing = local_timing;
            }

            return true;
        }

        void destroy() noexcept
        {
            for (gs_stagesurf_t *&stage_surface : stage_surfaces_)
            {
                if (stage_surface != nullptr)
                {
                    gs_stagesurface_destroy(stage_surface);
                    stage_surface = nullptr;
                }
            }

            if (mask_texture_ != nullptr)
            {
                gs_texture_destroy(mask_texture_);
                mask_texture_ = nullptr;
            }

            if (effect_ != nullptr)
            {
                gs_effect_destroy(effect_);
                effect_ = nullptr;
            }

            image_param_ = nullptr;
            width_ = 0U;
            height_ = 0U;
            staged_timestamps_ = {};
            next_stage_surface_ = 0U;
            next_map_surface_ = 0U;
            staged_surface_count_ = 0U;
        }

    private:
        [[nodiscard]] bool stage_surfaces_ready() const noexcept
        {
            return std::all_of(stage_surfaces_.begin(), stage_surfaces_.end(),
                               [](gs_stagesurf_t *surface) { return surface != nullptr; });
        }

        void map_staged_surface(std::size_t surface_index, std::vector<std::uint8_t> &alpha, std::string &error_message)
        {
            std::uint8_t *data = nullptr;
            std::uint32_t linesize = 0U;
            gs_stagesurf_t *const stage_surface = stage_surfaces_[surface_index];
            if (stage_surface == nullptr || !gs_stagesurface_map(stage_surface, &data, &linesize))
            {
                error_message = "Alpha Recorder could not map the staged alpha frame.";
                return;
            }

            if (data == nullptr || linesize < width_)
            {
                gs_stagesurface_unmap(stage_surface);
                error_message = "Alpha Recorder received an invalid staged alpha frame.";
                return;
            }

            copy_alpha_plane(alpha, data, linesize, width_, height_);
            gs_stagesurface_unmap(stage_surface);
        }

        bool render_alpha_mask(gs_texture_t *program_texture)
        {
            gs_texture_t *previous_render_target = gs_get_render_target();
            gs_zstencil_t *previous_zstencil_target = gs_get_zstencil_target();

            gs_set_render_target(mask_texture_, nullptr);
            vec4 clear_color;
            vec4_set(&clear_color, 0.0F, 0.0F, 0.0F, 1.0F);
            gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0F, 0);
            gs_ortho(0.0F, static_cast<float>(width_), 0.0F, static_cast<float>(height_), -100.0F, 100.0F);
            gs_set_viewport(0, 0, static_cast<int>(width_), static_cast<int>(height_));

            gs_enable_blending(false);
            gs_effect_set_texture(image_param_, program_texture);
            while (gs_effect_loop(effect_, "Draw"))
            {
                gs_draw_sprite(program_texture, 0, width_, height_);
            }
            gs_enable_blending(true);
            gs_set_render_target(previous_render_target, previous_zstencil_target);
            return true;
        }

        gs_effect_t *effect_ = nullptr;
        gs_eparam_t *image_param_ = nullptr;
        gs_texture_t *mask_texture_ = nullptr;
        static constexpr std::size_t kStageSurfaceCount = 4U;
        std::array<gs_stagesurf_t *, kStageSurfaceCount> stage_surfaces_{};
        std::array<std::uint64_t, kStageSurfaceCount> staged_timestamps_{};
        std::uint32_t width_ = 0U;
        std::uint32_t height_ = 0U;
        std::size_t next_stage_surface_ = 0U;
        std::size_t next_map_surface_ = 0U;
        std::size_t staged_surface_count_ = 0U;
    };

    struct EncodedAlphaFrame
    {
        std::int64_t pts = 0;
        std::uint64_t cts = 0U;
        bool texture_encoded = false;
    };

    class RecordingSessionController
    {
    public:
        ~RecordingSessionController()
        {
            stop_alignment_worker();
        }

        void initialize()
        {
            if (event_callback_registered_)
            {
                if (obs_frontend_recording_active())
                {
                    (void)start_session();
                }

                return;
            }

            obs_frontend_add_event_callback(&RecordingSessionController::on_frontend_event, this);
            event_callback_registered_ = true;

            if (obs_frontend_recording_active())
            {
                (void)start_session();
            }
        }

        void shutdown()
        {
            stop_session(false);

            if (event_callback_registered_)
            {
                obs_frontend_remove_event_callback(&RecordingSessionController::on_frontend_event, this);
                event_callback_registered_ = false;
            }
        }

        void on_frontend_event(enum obs_frontend_event event)
        {
            switch (event)
            {
            case OBS_FRONTEND_EVENT_RECORDING_STARTING:
                prepare_capture_session();
                break;

            case OBS_FRONTEND_EVENT_RECORDING_STARTED:
                start_session();
                break;

            case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
                set_recording_paused(true);
                break;

            case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
                set_recording_paused(false);
                break;

            case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
                break;

            case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
                stop_session(true);
                break;

            default:
                break;
            }
        }

    private:
        static void on_frontend_event(enum obs_frontend_event event, void *data)
        {
            static_cast<RecordingSessionController *>(data)->on_frontend_event(event);
        }

        static void on_main_rendered(void *data)
        {
            static_cast<RecordingSessionController *>(data)->on_main_rendered();
        }

        static void on_video_packet(obs_output_t *output, encoder_packet *packet, encoder_packet_time *packet_time, void *data)
        {
            static_cast<RecordingSessionController *>(data)->on_video_packet(output, packet, packet_time);
        }

        static void on_raw_video(void *data, video_data *frame)
        {
            static_cast<RecordingSessionController *>(data)->on_raw_video(frame);
        }

        static void on_file_changed(void *data, calldata_t *params)
        {
            static_cast<RecordingSessionController *>(data)->on_file_changed(params);
        }

        void set_recording_paused(bool paused)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!session_active_)
            {
                recording_paused_ = false;
                return;
            }

            recording_paused_ = paused;
        }

        bool prepare_capture_session()
        {
            const alpha_recorder::obs::Settings settings = alpha_recorder::obs::load_settings(obs_frontend_get_user_config());
            if (!settings.enabled)
            {
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            if (session_active_)
            {
                return true;
            }

            obs_output_t *recording_output = obs_frontend_get_recording_output();
            if (recording_output == nullptr)
            {
                log_and_show_error("Alpha Recorder could not access the active recording output.", true);
                return false;
            }

            obs_video_info video_info = {};
            if (!obs_get_video_info(&video_info))
            {
                log_and_show_error("Alpha Recorder could not read the OBS video configuration.", true);
                return false;
            }

            recording_output_ = obs_output_get_ref(recording_output);
            if (recording_output_ == nullptr)
            {
                log_and_show_error("Alpha Recorder could not retain a reference to the recording output.", true);
                return false;
            }

            finalization_format_ = settings.finalization_format;
            settings_ = settings;
            session_active_ = true;
            session_aborted_ = false;
            recording_paused_ = obs_frontend_recording_paused();
            video_info_ = video_info;
            recording_path_.clear();
            next_sequence_ = 0;
            last_alpha_frame_.alpha.reset();
            last_alpha_frame_.timestamp = 0U;
            last_captured_alpha_frame_.alpha.reset();
            last_captured_alpha_frame_.timestamp = 0U;
            clear_pending_alignment_locked();
            raw_video_cadence_.reset();
            live_telemetry_.reset();
            recording_texture_encoded_ =
                recording_output_uses_texture_encoder(recording_output_, video_info_.output_format);
            start_alignment_worker_locked();

            obs_add_main_rendered_callback(&RecordingSessionController::on_main_rendered, this);
            main_rendered_callback_connected_ = true;
            obs_add_raw_video_callback(nullptr, &RecordingSessionController::on_raw_video, this);
            raw_video_callback_connected_ = true;
            obs_output_add_packet_callback(recording_output_, &RecordingSessionController::on_video_packet, this);
            packet_callback_connected_ = true;
            return true;
        }

        bool start_session()
        {
            const alpha_recorder::obs::Settings settings = alpha_recorder::obs::load_settings(obs_frontend_get_user_config());
            if (!settings.enabled)
            {
                return true;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            if (writer_.is_open())
            {
                return true;
            }

            obs_output_t *recording_output = recording_output_;
            if (recording_output == nullptr)
            {
                recording_output = obs_frontend_get_recording_output();
                if (recording_output == nullptr)
                {
                    log_and_show_error("Alpha Recorder could not access the active recording output.", true);
                    return false;
                }
            }

            obs_video_info video_info = video_info_;
            if (video_info.output_width == 0U || video_info.output_height == 0U)
            {
                if (!obs_get_video_info(&video_info))
                {
                    log_and_show_error("Alpha Recorder could not read the OBS video configuration.", true);
                    return false;
                }
            }

            char *recording_path_text = obs_frontend_get_current_record_output_path();
            std::filesystem::path recording_path = recording_file_path_from_output(recording_output);
            if (recording_path.empty() && recording_path_text != nullptr && *recording_path_text != '\0')
            {
                recording_path = path_from_utf8(recording_path_text);
            }

            if (recording_path_text != nullptr)
            {
                bfree(recording_path_text);
            }

            if (recording_path.empty())
            {
                log_and_show_error("Alpha Recorder could not determine the recording file path.", true);
                return false;
            }

            if (path_is_directory(recording_path))
            {
                log_and_show_error(std::string{"Alpha Recorder could not determine the recording file name; OBS only reported the recording folder: "} +
                                       recording_path.generic_string(),
                                   true);
                return false;
            }

            if (recording_output_ == nullptr)
            {
                recording_output_ = obs_output_get_ref(recording_output);
                if (recording_output_ == nullptr)
                {
                    log_and_show_error("Alpha Recorder could not retain a reference to the recording output.", true);
                    return false;
                }
            }

            finalization_format_ = settings.finalization_format;
            settings_ = settings;

            if (!open_segment_locked(recording_path, video_info, true))
            {
                obs_output_release(recording_output_);
                recording_output_ = nullptr;
                session_active_ = false;
                clear_pending_alignment_locked();
                return false;
            }

            session_active_ = true;
            session_aborted_ = false;
            recording_paused_ = obs_frontend_recording_paused();
            video_info_ = video_info;
            recording_path_ = recording_path;
            recording_texture_encoded_ =
                recording_output_uses_texture_encoder(recording_output_, video_info_.output_format);
            start_alignment_worker_locked();
            notify_alignment_worker_locked();

            signal_handler_t *signal_handler = obs_output_get_signal_handler(recording_output_);
            if (signal_handler != nullptr && !file_changed_connected_)
            {
                signal_handler_connect(signal_handler, "file_changed", &RecordingSessionController::on_file_changed, this);
                file_changed_connected_ = true;
            }

            if (!main_rendered_callback_connected_)
            {
                obs_add_main_rendered_callback(&RecordingSessionController::on_main_rendered, this);
                main_rendered_callback_connected_ = true;
            }
            if (!raw_video_callback_connected_)
            {
                obs_add_raw_video_callback(nullptr, &RecordingSessionController::on_raw_video, this);
                raw_video_callback_connected_ = true;
            }
            if (!packet_callback_connected_)
            {
                obs_output_add_packet_callback(recording_output_, &RecordingSessionController::on_video_packet, this);
                packet_callback_connected_ = true;
            }
            std::string drain_error;
            reconcile_output_frame_count_locked(drain_error);
            if (!drain_error.empty())
            {
                log_and_show_error(drain_error, false);
            }
            return true;
        }

        bool open_segment_locked(const std::filesystem::path &recording_path, const obs_video_info &video_info, bool show_popup)
        {
            const std::filesystem::path mask_path = alpha_recorder::obs::recording_alpha_movie_path(recording_path, finalization_format_);

            alpha_recorder::obs::AlphaMaskVideoWriterConfig config{};
            config.output_path = mask_path;
            config.finalization_format = finalization_format_;
            config.width = video_info.output_width;
            config.height = video_info.output_height;
            config.fps_num = video_info.fps_num;
            config.fps_den = video_info.fps_den;
            config.hevc_encoder = settings_.hevc_encoder;

            std::string writer_error;
            if (!writer_.open(config, &writer_error))
            {
                log_and_show_error(writer_error.empty() ? std::string{"Alpha Recorder could not open the alpha mask movie for recording path: "} +
                                                              recording_path.generic_string()
                                                        : writer_error,
                                   show_popup);
                return false;
            }

            video_info_ = video_info;
            recording_path_ = recording_path;
            session_aborted_ = false;
            live_telemetry_.reset();
            return true;
        }

        bool finalize_current_segment_locked(std::string *error_message)
        {
            if (!writer_.is_open())
            {
                return true;
            }

            const std::filesystem::path writer_path = writer_.path();
            alpha_recorder::obs::AlphaMaskVideoWriterStats writer_stats{};
            if (!writer_.close(error_message, &writer_stats))
            {
                log_performance_summary_locked(writer_path, writer_stats);
                return false;
            }

            log_performance_summary_locked(writer_path, writer_stats);
            return true;
        }

        void clear_pending_alignment_locked() noexcept
        {
            pending_alpha_frames_.clear();
            pending_encoded_alpha_frames_.clear();
            pending_output_frames_.clear();
        }

        void log_performance_summary_locked(const std::filesystem::path &mask_path,
                                            const alpha_recorder::obs::AlphaMaskVideoWriterStats &writer_stats)
        {
            blog(LOG_INFO,
                 "Alpha Recorder performance telemetry: path=\"%s\" capture_total={%s callbacks=%llu captured=%llu} readback={%s} gpu_submit={render={%s} stage={%s}} alignment_worker={%s frames=%llu raw=%llu packets=%llu} queues={alpha_max=%zu output_max=%zu encoded_max=%zu writer_max_frames=%zu writer_max_bytes=%s} writer={enqueue={count=%llu avg_ms=%.3f max_ms=%.3f} encode={count=%llu avg_ms=%.3f max_ms=%.3f} finalize_ms=%.3f queued=%s}",
                 mask_path.generic_string().c_str(),
                 format_timing_summary(live_telemetry_.capture_total).c_str(),
                 static_cast<unsigned long long>(live_telemetry_.rendered_callbacks),
                 static_cast<unsigned long long>(live_telemetry_.captured_frames),
                 format_timing_summary(live_telemetry_.capture_map).c_str(),
                 format_timing_summary(live_telemetry_.capture_render).c_str(),
                 format_timing_summary(live_telemetry_.capture_stage).c_str(),
                 format_timing_summary(live_telemetry_.alignment_batch).c_str(),
                 static_cast<unsigned long long>(live_telemetry_.aligned_frames),
                 static_cast<unsigned long long>(live_telemetry_.raw_video_frames),
                 static_cast<unsigned long long>(live_telemetry_.packet_frames),
                 live_telemetry_.max_pending_alpha_frames,
                 live_telemetry_.max_pending_output_frames,
                 live_telemetry_.max_pending_encoded_frames,
                 writer_stats.max_queued_frames,
                 format_bytes(static_cast<std::uint64_t>(writer_stats.max_queued_bytes)).c_str(),
                 static_cast<unsigned long long>(writer_stats.enqueued_frames),
                 writer_stats.enqueued_frames == 0U ? 0.0 : ns_to_ms(writer_stats.enqueue_time_ns_total / writer_stats.enqueued_frames),
                 ns_to_ms(writer_stats.enqueue_time_ns_max),
                 static_cast<unsigned long long>(writer_stats.encoded_frames),
                 writer_stats.encoded_frames == 0U ? 0.0 : ns_to_ms(writer_stats.encode_time_ns_total / writer_stats.encoded_frames),
                 ns_to_ms(writer_stats.encode_time_ns_max),
                 ns_to_ms(writer_stats.finalize_time_ns),
                 format_bytes(writer_stats.queued_bytes_total).c_str());
        }

        [[nodiscard]] bool alignment_worker_has_work_locked() const noexcept
        {
            return writer_.is_open() && pending_encoded_alpha_frames_.size() > kMaxEncoderReorderFrames &&
                   !pending_output_frames_.empty();
        }

        void notify_alignment_worker_locked() noexcept
        {
            alignment_condition_.notify_one();
        }

        void start_alignment_worker_locked()
        {
            if (alignment_worker_.joinable())
            {
                return;
            }

            alignment_worker_stop_ = false;
            alignment_worker_ = std::thread([this]() { alignment_worker_loop(); });
        }

        void stop_alignment_worker()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                alignment_worker_stop_ = true;
                alignment_condition_.notify_all();
            }

            if (alignment_worker_.joinable())
            {
                alignment_worker_.join();
            }
        }

        void alignment_worker_loop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (true)
            {
                alignment_condition_.wait(lock, [this]() {
                    return alignment_worker_stop_ || alignment_worker_has_work_locked();
                });

                if (alignment_worker_stop_)
                {
                    return;
                }

                drain_encoded_alpha_locked(false, 1U);
                lock.unlock();
                std::this_thread::yield();
                lock.lock();
            }
        }

        bool remember_pending_alpha_frame_locked(alpha_recorder::obs::AlphaFrame frame, std::string &error_message)
        {
            constexpr std::size_t kMaxPendingFrames = 240U;
            pending_alpha_frames_.push_back(std::move(frame));
            ++live_telemetry_.queued_alpha_frames;
            live_telemetry_.max_pending_alpha_frames =
                std::max(live_telemetry_.max_pending_alpha_frames, pending_alpha_frames_.size());
            if (pending_alpha_frames_.size() > kMaxPendingFrames)
            {
                session_aborted_ = true;
                clear_pending_alignment_locked();
                error_message = "Alpha Recorder alpha frame queue overflowed before OBS output cadence could consume it.";
                return false;
            }

            notify_alignment_worker_locked();
            return true;
        }

        bool write_alpha_frame_locked(const alpha_recorder::obs::AlphaFrame &frame, std::string &error_message)
        {
            if (frame.empty() || !writer_.write_frame(frame.alpha, &error_message))
            {
                session_aborted_ = true;
                if (error_message.empty())
                {
                    error_message = "Alpha Recorder failed to write an alpha mask frame.";
                }
                return false;
            }

            last_alpha_frame_ = frame;
            ++next_sequence_;
            return true;
        }

        bool resolve_output_alpha_frame_locked(const EncodedAlphaFrame &encoded_frame,
                                               bool drain_all,
                                               alpha_recorder::obs::AlphaFrame &frame,
                                               std::string &error_message)
        {
            if (encoded_frame.cts == 0U)
            {
                session_aborted_ = true;
                error_message =
                    "Alpha Recorder could not align alpha output because OBS did not provide an encoded-frame composition timestamp.";
                return false;
            }

            const alpha_recorder::obs::TimestampFrameSelection output_selection =
                encoded_frame.texture_encoded
                    ? alpha_recorder::obs::select_frame_after_timestamp(pending_output_frames_, encoded_frame.cts, drain_all)
                    : alpha_recorder::obs::select_frame_by_timestamp(pending_output_frames_, encoded_frame.cts, drain_all);
            if (output_selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::WaitingForMoreFrames)
            {
                return false;
            }

            if (output_selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::NoPlausibleFrame)
            {
                session_aborted_ = true;
                error_message =
                    std::string{"Alpha Recorder could not find the raw-video cadence frame admitted by the RGB encoder; nearest cadence timestamp delta "} +
                    std::to_string(output_selection.timestamp_delta) + " ns for packet composition timestamp " +
                    std::to_string(encoded_frame.cts) +
                    (encoded_frame.texture_encoded ? " while waiting for the texture-encoder successor cadence frame."
                                                   : " while waiting for the exact software-encoder cadence frame.");
                return false;
            }

            const alpha_recorder::obs::OutputFrameCadence output_frame =
                pending_output_frames_[output_selection.selected_index];
            if (alpha_recorder::obs::duplicate_output_uses_previous_alpha(output_frame, last_alpha_frame_,
                                                                          frame))
            {
                pending_output_frames_.erase(
                    pending_output_frames_.begin(),
                    pending_output_frames_.begin() + static_cast<std::ptrdiff_t>(output_selection.selected_index + 1U));
                return true;
            }

            const alpha_recorder::obs::TimestampFrameSelection alpha_selection =
                alpha_recorder::obs::select_frame_by_timestamp(pending_alpha_frames_, output_frame.content_timestamp, drain_all);
            if (alpha_selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::WaitingForMoreFrames)
            {
                return false;
            }

            if (alpha_selection.status == alpha_recorder::obs::TimestampFrameSelectionStatus::NoPlausibleFrame)
            {
                session_aborted_ = true;
                error_message =
                    "Alpha Recorder could not find an alpha frame for the admitted RGB content timestamp; nearest alpha timestamp delta " +
                    std::to_string(alpha_selection.timestamp_delta) + " ns for content timestamp " +
                    std::to_string(output_frame.content_timestamp) + ".";
                return false;
            }

            frame = std::move(pending_alpha_frames_[alpha_selection.selected_index]);
            pending_alpha_frames_.erase(
                pending_alpha_frames_.begin(),
                pending_alpha_frames_.begin() + static_cast<std::ptrdiff_t>(alpha_selection.selected_index + 1U));
            pending_output_frames_.erase(
                pending_output_frames_.begin(),
                pending_output_frames_.begin() + static_cast<std::ptrdiff_t>(output_selection.selected_index + 1U));
            return true;
        }

        void drain_encoded_alpha_locked(bool drain_all, std::size_t max_frames = static_cast<std::size_t>(-1))
        {
            if (!writer_.is_open())
            {
                return;
            }

            const auto batch_start = std::chrono::steady_clock::now();
            alpha_recorder::obs::AlphaFrame alpha_frame{};
            std::string error_message;
            std::size_t drained_frames = 0U;
            while (!pending_encoded_alpha_frames_.empty() &&
                   (drain_all || pending_encoded_alpha_frames_.size() > kMaxEncoderReorderFrames) &&
                   drained_frames < max_frames)
            {
                if (pending_output_frames_.empty())
                {
                    return;
                }

                auto selected = std::min_element(
                    pending_encoded_alpha_frames_.begin(), pending_encoded_alpha_frames_.end(),
                    [](const EncodedAlphaFrame &left, const EncodedAlphaFrame &right) { return left.pts < right.pts; });
                if (!resolve_output_alpha_frame_locked(*selected, drain_all, alpha_frame, error_message))
                {
                    if (!error_message.empty())
                    {
                        clear_pending_alignment_locked();
                        log_and_show_error(error_message, false);
                    }
                    return;
                }

                pending_encoded_alpha_frames_.erase(selected);

                if (!write_alpha_frame_locked(alpha_frame, error_message))
                {
                    clear_pending_alignment_locked();
                    log_and_show_error(error_message, false);
                    return;
                }
                ++drained_frames;
                ++live_telemetry_.aligned_frames;
            }
            if (drained_frames > 0U)
            {
                live_telemetry_.alignment_batch.add(elapsed_ns(batch_start, std::chrono::steady_clock::now()));
            }
        }

        void queue_alpha_for_packet_locked(std::int64_t pts, std::uint64_t cts, bool texture_encoded)
        {
            EncodedAlphaFrame encoded_frame{};
            encoded_frame.pts = pts;
            encoded_frame.cts = cts;
            encoded_frame.texture_encoded = texture_encoded;
            pending_encoded_alpha_frames_.push_back(std::move(encoded_frame));
            ++live_telemetry_.packet_frames;
            live_telemetry_.max_pending_encoded_frames =
                std::max(live_telemetry_.max_pending_encoded_frames, pending_encoded_alpha_frames_.size());
            notify_alignment_worker_locked();
        }

        bool remember_output_frame_locked(alpha_recorder::obs::OutputFrameCadence frame, std::string &error_message)
        {
            constexpr std::size_t kMaxPendingOutputFrames = 240U;
            pending_output_frames_.push_back(frame);
            ++live_telemetry_.raw_video_frames;
            live_telemetry_.max_pending_output_frames =
                std::max(live_telemetry_.max_pending_output_frames, pending_output_frames_.size());
            if (pending_output_frames_.size() > kMaxPendingOutputFrames)
            {
                session_aborted_ = true;
                clear_pending_alignment_locked();
                error_message = "Alpha Recorder output cadence queue overflowed before encoded packet order could catch up.";
                return false;
            }

            notify_alignment_worker_locked();
            return true;
        }

        void reconcile_output_frame_count_locked(std::string &error_message)
        {
            if (!writer_.is_open())
            {
                return;
            }

            drain_encoded_alpha_locked(true);
            (void)error_message;
        }

        void capture_final_alpha_frame_locked()
        {
            if (video_info_.output_width == 0U || video_info_.output_height == 0U)
            {
                return;
            }

            std::string error_message;
            obs_enter_graphics();
            std::vector<std::uint8_t> alpha;
            std::uint64_t alpha_timestamp = 0U;
            const bool captured = alpha_extractor_.ensure(video_info_.output_width, video_info_.output_height, error_message) &&
                                  alpha_extractor_.capture_latest(alpha, alpha_timestamp, error_message);
            if (captured && !alpha.empty())
            {
                (void)remember_pending_alpha_frame_locked(
                    alpha_recorder::obs::AlphaFrame{alpha_timestamp, std::make_shared<std::vector<std::uint8_t>>(std::move(alpha))},
                    error_message);
            }
            obs_leave_graphics();

            if (!captured)
            {
                session_aborted_ = true;
                if (error_message.empty())
                {
                    error_message = "Alpha Recorder failed to capture the final alpha mask frame.";
                }
            }
            if (!error_message.empty())
            {
                log_and_show_error(error_message, false);
            }
        }

        void stop_session(bool show_popup)
        {
            obs_output_t *recording_output = nullptr;
            bool disconnect_main_rendered_callback = false;
            bool disconnect_raw_video_callback = false;
            bool disconnect_packet_callback = false;
            bool disconnect_file_changed = false;
            bool finalize_failed = false;
            std::string finalize_error;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!session_active_ && !writer_.is_open() && recording_output_ == nullptr &&
                    !main_rendered_callback_connected_ && !raw_video_callback_connected_ &&
                    !packet_callback_connected_ && !file_changed_connected_)
                {
                    recording_paused_ = false;
                    return;
                }

                session_active_ = false;
                recording_paused_ = false;
                recording_output = recording_output_;
                recording_output_ = nullptr;
                disconnect_main_rendered_callback = main_rendered_callback_connected_;
                disconnect_raw_video_callback = raw_video_callback_connected_;
                disconnect_packet_callback = packet_callback_connected_;
                disconnect_file_changed = file_changed_connected_;
                main_rendered_callback_connected_ = false;
                raw_video_callback_connected_ = false;
                packet_callback_connected_ = false;
                file_changed_connected_ = false;
            }

            if (disconnect_main_rendered_callback)
            {
                obs_remove_main_rendered_callback(&RecordingSessionController::on_main_rendered, this);
            }
            if (disconnect_raw_video_callback)
            {
                obs_remove_raw_video_callback(&RecordingSessionController::on_raw_video, this);
            }
            if (disconnect_packet_callback && recording_output != nullptr)
            {
                obs_output_remove_packet_callback(recording_output, &RecordingSessionController::on_video_packet, this);
            }

            if (recording_output != nullptr)
            {
                if (disconnect_file_changed)
                {
                    signal_handler_t *signal_handler = obs_output_get_signal_handler(recording_output);
                    if (signal_handler != nullptr)
                    {
                        signal_handler_disconnect(signal_handler, "file_changed", &RecordingSessionController::on_file_changed,
                                                  this);
                    }
                }

            }

            stop_alignment_worker();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                capture_final_alpha_frame_locked();
                reconcile_output_frame_count_locked(finalize_error);
                if (!finalize_error.empty())
                {
                    finalize_failed = true;
                }
                if (!finalize_current_segment_locked(&finalize_error))
                {
                    finalize_failed = true;
                }

                last_alpha_frame_.alpha.reset();
                last_alpha_frame_.timestamp = 0U;
                last_captured_alpha_frame_.alpha.reset();
                last_captured_alpha_frame_.timestamp = 0U;
                clear_pending_alignment_locked();
                raw_video_cadence_.reset();
                recording_texture_encoded_ = false;
                obs_enter_graphics();
                alpha_extractor_.destroy();
                obs_leave_graphics();
            }

            if (recording_output != nullptr)
            {
                obs_output_release(recording_output);
            }

            if (finalize_failed)
            {
                log_and_show_error(finalize_error.empty() ? "Alpha Recorder failed to finalize the current alpha mask movie."
                                                          : finalize_error,
                                   show_popup);
                return;
            }
        }

        void on_file_changed(calldata_t *params)
        {
            const char *next_file_text = calldata_string(params, "next_file");
            if (next_file_text == nullptr || *next_file_text == '\0')
            {
                return;
            }

            const std::filesystem::path next_recording_path = path_from_utf8(next_file_text);
            if (next_recording_path.empty())
            {
                return;
            }

            std::string error_message;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!session_active_ || session_aborted_)
                {
                    recording_path_ = next_recording_path;
                    return;
                }

                if (recording_path_ == next_recording_path)
                {
                    return;
                }

                if (writer_.is_open())
                {
                    reconcile_output_frame_count_locked(error_message);
                    if (!finalize_current_segment_locked(&error_message))
                    {
                        if (error_message.empty())
                        {
                            error_message = "Alpha Recorder failed to finalize the current split alpha mask movie.";
                        }
                    }
                }

                if (!session_aborted_)
                {
                    last_alpha_frame_.alpha.reset();
                    last_alpha_frame_.timestamp = 0U;
                    last_captured_alpha_frame_.alpha.reset();
                    last_captured_alpha_frame_.timestamp = 0U;
                    clear_pending_alignment_locked();
                    raw_video_cadence_.reset();

                    if (!open_segment_locked(next_recording_path, video_info_, false))
                    {
                        session_aborted_ = true;
                    }
                }

                recording_path_ = next_recording_path;
            }

            if (!error_message.empty())
            {
                log_and_show_error(error_message, false);
            }
        }

        void on_main_rendered()
        {
            std::vector<std::uint8_t> alpha;
            std::uint64_t alpha_timestamp = 0U;
            std::string error_message;
            std::uint32_t output_width = 0U;
            std::uint32_t output_height = 0U;
            CaptureTiming capture_timing{};

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!session_active_ || session_aborted_ || recording_paused_ || recording_output_ == nullptr ||
                    video_info_.output_width == 0U || video_info_.output_height == 0U)
                {
                    return;
                }

                output_width = video_info_.output_width;
                output_height = video_info_.output_height;
            }

            if (!alpha_extractor_.ensure(output_width, output_height, error_message) ||
                !alpha_extractor_.capture_latest(alpha, alpha_timestamp, error_message, &capture_timing))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++live_telemetry_.rendered_callbacks;
                live_telemetry_.capture_total.add(capture_timing.total_ns);
                if (capture_timing.mapped)
                {
                    live_telemetry_.capture_map.add(capture_timing.map_ns);
                }
                live_telemetry_.capture_render.add(capture_timing.render_ns);
                live_telemetry_.capture_stage.add(capture_timing.stage_ns);
                if (session_active_ && !session_aborted_)
                {
                    session_aborted_ = true;
                    if (error_message.empty())
                    {
                        error_message = "Alpha Recorder failed to capture an alpha mask frame.";
                    }
                }
            }
            else if (!alpha.empty())
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++live_telemetry_.rendered_callbacks;
                ++live_telemetry_.captured_frames;
                live_telemetry_.capture_total.add(capture_timing.total_ns);
                if (capture_timing.mapped)
                {
                    live_telemetry_.capture_map.add(capture_timing.map_ns);
                }
                live_telemetry_.capture_render.add(capture_timing.render_ns);
                live_telemetry_.capture_stage.add(capture_timing.stage_ns);
                if (session_active_ && !session_aborted_ && !recording_paused_ && recording_output_ != nullptr &&
                    video_info_.output_width == output_width && video_info_.output_height == output_height)
                {
                    last_captured_alpha_frame_ = alpha_recorder::obs::AlphaFrame{alpha_timestamp, std::make_shared<std::vector<std::uint8_t>>(std::move(alpha))};
                    (void)remember_pending_alpha_frame_locked(last_captured_alpha_frame_, error_message);
                }
            }
            else
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++live_telemetry_.rendered_callbacks;
                live_telemetry_.capture_total.add(capture_timing.total_ns);
                if (capture_timing.mapped)
                {
                    live_telemetry_.capture_map.add(capture_timing.map_ns);
                }
                live_telemetry_.capture_render.add(capture_timing.render_ns);
                live_telemetry_.capture_stage.add(capture_timing.stage_ns);
            }

            if (!error_message.empty())
            {
                log_and_show_error(error_message, false);
            }
        }

        void on_raw_video(video_data *frame)
        {
            if (frame == nullptr)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            if (!session_active_ || session_aborted_ || recording_paused_ || recording_output_ == nullptr ||
                video_info_.output_width == 0U || video_info_.output_height == 0U)
            {
                return;
            }

            std::string error_message;
            (void)remember_output_frame_locked(raw_video_cadence_.remember(frame->data[0], frame->timestamp), error_message);
            if (!error_message.empty())
            {
                log_and_show_error(error_message, false);
            }
        }

        void on_video_packet(obs_output_t *output, encoder_packet *packet, encoder_packet_time *packet_time)
        {
            std::string error_message;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (output != recording_output_ || packet == nullptr || packet->type != OBS_ENCODER_VIDEO ||
                    !session_active_ || session_aborted_ || recording_paused_ || recording_output_ == nullptr ||
                    video_info_.output_width == 0U || video_info_.output_height == 0U)
                {
                    return;
                }

                if (packet_time == nullptr || packet_time->cts == 0U)
                {
                    session_aborted_ = true;
                    clear_pending_alignment_locked();
                    error_message =
                        "Alpha Recorder could not align alpha output because OBS did not report encoded-frame composition timing.";
                }
                else
                {
                    queue_alpha_for_packet_locked(packet->pts, packet_time->cts, recording_texture_encoded_);
                }
            }

            if (!error_message.empty())
            {
                log_and_show_error(error_message, false);
            }
        }

        bool event_callback_registered_ = false;
        bool main_rendered_callback_connected_ = false;
        bool raw_video_callback_connected_ = false;
        bool packet_callback_connected_ = false;
        bool file_changed_connected_ = false;
        bool session_active_ = false;
        bool session_aborted_ = false;
        bool recording_paused_ = false;
        bool recording_texture_encoded_ = false;
        std::uint64_t next_sequence_ = 0;
        alpha_recorder::obs::AlphaFrame last_alpha_frame_{};
        alpha_recorder::obs::AlphaFrame last_captured_alpha_frame_{};
        std::deque<alpha_recorder::obs::AlphaFrame> pending_alpha_frames_{};
        std::deque<EncodedAlphaFrame> pending_encoded_alpha_frames_{};
        std::deque<alpha_recorder::obs::OutputFrameCadence> pending_output_frames_{};
        alpha_recorder::obs::RawVideoCadenceTracker raw_video_cadence_{};
        LivePipelineTelemetry live_telemetry_{};
        std::filesystem::path recording_path_{};
        obs_video_info video_info_{};
        AlphaPlaneExtractor alpha_extractor_{};
        AlphaMaskVideoWriter writer_{};
        obs_output_t *recording_output_ = nullptr;
        alpha_recorder::obs::FinalizationFormat finalization_format_ = alpha_recorder::obs::FinalizationFormat::MaskPngMov;
        alpha_recorder::obs::Settings settings_{};
        static constexpr std::size_t kMaxEncoderReorderFrames = 16U;
        std::condition_variable alignment_condition_{};
        std::thread alignment_worker_{};
        bool alignment_worker_stop_ = false;
        std::mutex mutex_{};
    };

    RecordingSessionController &controller()
    {
        static RecordingSessionController instance;
        return instance;
    }

} // namespace

namespace alpha_recorder::obs
{

    bool register_runtime_hooks() noexcept
    {
        const alpha_recorder::obs::Settings settings = alpha_recorder::obs::load_settings(obs_frontend_get_user_config());
        if (!settings.enabled)
        {
            controller().shutdown();
            return true;
        }

        controller().initialize();
        return true;
    }

    void unregister_runtime_hooks() noexcept
    {
        controller().shutdown();
    }

} // namespace alpha_recorder::obs
