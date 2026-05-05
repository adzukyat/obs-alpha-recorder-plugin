#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"
#include "recording_session_controller_cadence.hpp"
#include "recording_session_controller_gate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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

            if (effect_ != nullptr && mask_texture_ != nullptr && width_ == width && height_ == height)
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
            stage_surface_ = gs_stagesurface_create(width, height, GS_R8);

            if (image_param_ == nullptr || mask_texture_ == nullptr || stage_surface_ == nullptr)
            {
                error_message = "Alpha Recorder could not allocate GPU resources for alpha extraction.";
                destroy();
                return false;
            }

            width_ = width;
            height_ = height;
            return true;
        }

        bool capture_latest(std::vector<std::uint8_t> &alpha, std::uint64_t &timestamp, std::string &error_message)
        {
            alpha.clear();
            timestamp = 0U;

            gs_texture_t *program_texture = obs_get_main_texture();
            if (program_texture == nullptr)
            {
                return true;
            }

            if (has_staged_frame_)
            {
                map_staged_surface(alpha, error_message);
                if (!error_message.empty())
                {
                    return false;
                }
                timestamp = staged_timestamp_;
            }

            if (!render_alpha_mask(program_texture))
            {
                return false;
            }

            gs_stage_texture(stage_surface_, mask_texture_);
            staged_timestamp_ = obs_get_video_frame_time();
            has_staged_frame_ = true;

            return true;
        }

        void destroy() noexcept
        {
            if (stage_surface_ != nullptr)
            {
                gs_stagesurface_destroy(stage_surface_);
                stage_surface_ = nullptr;
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
            staged_timestamp_ = 0U;
            has_staged_frame_ = false;
        }

    private:
        void map_staged_surface(std::vector<std::uint8_t> &alpha, std::string &error_message)
        {
            std::uint8_t *data = nullptr;
            std::uint32_t linesize = 0U;
            if (!gs_stagesurface_map(stage_surface_, &data, &linesize))
            {
                error_message = "Alpha Recorder could not map the staged alpha frame.";
                return;
            }

            if (data == nullptr || linesize < width_)
            {
                gs_stagesurface_unmap(stage_surface_);
                error_message = "Alpha Recorder received an invalid staged alpha frame.";
                return;
            }

            copy_alpha_plane(alpha, data, linesize, width_, height_);
            gs_stagesurface_unmap(stage_surface_);
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
        gs_stagesurf_t *stage_surface_ = nullptr;
        std::uint32_t width_ = 0U;
        std::uint32_t height_ = 0U;
        std::uint64_t staged_timestamp_ = 0U;
        bool has_staged_frame_ = false;
    };

    struct EncodedAlphaFrame
    {
        std::int64_t pts = 0;
        std::uint64_t timestamp = 0U;
        alpha_recorder::obs::AlphaFrame frame{};
    };

    // Keep enough alpha frames queued to absorb packet callback reordering before binding by presentation order.
    constexpr std::size_t kVideoOutputCacheFrameLatency = 0U;

    class RecordingSessionController
    {
    public:
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
            pending_alpha_frames_.clear();
            pending_encoded_alpha_frames_.clear();
            pending_output_frames_.clear();
            raw_video_cadence_.reset();

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

            if (!open_segment_locked(recording_path, video_info, true))
            {
                obs_output_release(recording_output_);
                recording_output_ = nullptr;
                session_active_ = false;
                pending_alpha_frames_.clear();
                pending_encoded_alpha_frames_.clear();
                pending_output_frames_.clear();
                return false;
            }

            session_active_ = true;
            session_aborted_ = false;
            recording_paused_ = obs_frontend_recording_paused();
            video_info_ = video_info;
            recording_path_ = recording_path;

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
            return true;
        }

        bool finalize_current_segment_locked(std::string *error_message)
        {
            if (!writer_.is_open())
            {
                return true;
            }

            if (!writer_.close(error_message))
            {
                return false;
            }

            return true;
        }

        void remember_pending_alpha_frame_locked(alpha_recorder::obs::AlphaFrame frame)
        {
            constexpr std::size_t kMaxPendingFrames = 240U;
            pending_alpha_frames_.push_back(std::move(frame));
            while (pending_alpha_frames_.size() > kMaxPendingFrames)
            {
                pending_alpha_frames_.pop_front();
            }
        }

        bool write_alpha_frame_locked(const alpha_recorder::obs::AlphaFrame &frame, std::string &error_message)
        {
            if (frame.empty() || !writer_.write_frame(frame.alpha->data(), video_info_.output_width, &error_message))
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

        bool select_alpha_for_timestamp_locked(std::uint64_t timestamp, alpha_recorder::obs::AlphaFrame &frame, bool drain_all)
        {
            if (pending_alpha_frames_.empty())
            {
                return false;
            }

            std::size_t selected_index = 0U;
            if (timestamp != 0U)
            {
                std::uint64_t selected_delta = pending_alpha_frames_.front().timestamp > timestamp
                                                   ? pending_alpha_frames_.front().timestamp - timestamp
                                                   : timestamp - pending_alpha_frames_.front().timestamp;
                for (std::size_t index = 1U; index < pending_alpha_frames_.size(); ++index)
                {
                    const std::uint64_t candidate_timestamp = pending_alpha_frames_[index].timestamp;
                    const std::uint64_t candidate_delta = candidate_timestamp > timestamp
                                                              ? candidate_timestamp - timestamp
                                                              : timestamp - candidate_timestamp;
                    if (candidate_delta <= selected_delta)
                    {
                        selected_index = index;
                        selected_delta = candidate_delta;
                    }
                }
            }

            if (!drain_all && selected_index == pending_alpha_frames_.size() - 1U &&
                pending_alpha_frames_.size() <= kVideoOutputCacheFrameLatency)
            {
                return false;
            }

            frame = std::move(pending_alpha_frames_[selected_index]);
            pending_alpha_frames_.erase(pending_alpha_frames_.begin(),
                                        pending_alpha_frames_.begin() + static_cast<std::ptrdiff_t>(selected_index + 1U));
            return true;
        }

        bool resolve_encoded_alpha_frame_locked(EncodedAlphaFrame &encoded_frame,
                                                const alpha_recorder::obs::OutputFrameCadence &output_frame,
                                                bool allow_duplicate)
        {
            if (!encoded_frame.frame.empty())
            {
                return true;
            }

            if (alpha_recorder::obs::duplicate_output_uses_previous_alpha(output_frame, last_alpha_frame_,
                                                                          encoded_frame.frame))
            {
                return true;
            }

            const std::uint64_t alpha_timestamp = encoded_frame.timestamp != 0U ? encoded_frame.timestamp : output_frame.timestamp;
            if (select_alpha_for_timestamp_locked(alpha_timestamp, encoded_frame.frame, allow_duplicate))
            {
                return true;
            }

            if (allow_duplicate && !last_alpha_frame_.empty())
            {
                encoded_frame.frame = last_alpha_frame_;
                return true;
            }

            return false;
        }

        void drain_encoded_alpha_locked(bool drain_all)
        {
            if (!writer_.is_open())
            {
                return;
            }

            constexpr std::size_t kMaxEncoderReorderFrames = 16U;
            alpha_recorder::obs::AlphaFrame alpha_frame{};
            std::string error_message;
            while (!pending_encoded_alpha_frames_.empty() &&
                   (drain_all || pending_encoded_alpha_frames_.size() > kMaxEncoderReorderFrames))
            {
                if (pending_output_frames_.empty())
                {
                    return;
                }

                auto selected = std::min_element(
                    pending_encoded_alpha_frames_.begin(), pending_encoded_alpha_frames_.end(),
                    [](const EncodedAlphaFrame &left, const EncodedAlphaFrame &right) { return left.pts < right.pts; });
                const alpha_recorder::obs::OutputFrameCadence output_frame = pending_output_frames_.front();

                if (!resolve_encoded_alpha_frame_locked(*selected, output_frame, drain_all))
                {
                    return;
                }

                alpha_frame = std::move(selected->frame);
                pending_encoded_alpha_frames_.erase(selected);
                pending_output_frames_.pop_front();

                if (!write_alpha_frame_locked(alpha_frame, error_message))
                {
                    pending_alpha_frames_.clear();
                    pending_encoded_alpha_frames_.clear();
                    pending_output_frames_.clear();
                    log_and_show_error(error_message, false);
                    return;
                }
            }
        }

        void queue_alpha_for_packet_locked(std::int64_t pts, std::uint64_t timestamp)
        {
            EncodedAlphaFrame encoded_frame{};
            encoded_frame.pts = pts;
            encoded_frame.timestamp = timestamp;
            pending_encoded_alpha_frames_.push_back(std::move(encoded_frame));
            drain_encoded_alpha_locked(false);
        }

        void remember_output_frame_locked(alpha_recorder::obs::OutputFrameCadence frame)
        {
            constexpr std::size_t kMaxPendingOutputFrames = 240U;
            pending_output_frames_.push_back(frame);
            while (pending_output_frames_.size() > kMaxPendingOutputFrames)
            {
                pending_output_frames_.pop_front();
            }
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
                remember_pending_alpha_frame_locked(
                    alpha_recorder::obs::AlphaFrame{alpha_timestamp, std::make_shared<std::vector<std::uint8_t>>(std::move(alpha))});
            }
            obs_leave_graphics();

            if (!captured)
            {
                session_aborted_ = true;
                log_and_show_error(error_message.empty() ? "Alpha Recorder failed to capture the final alpha mask frame." : error_message,
                                   false);
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
                pending_alpha_frames_.clear();
                pending_encoded_alpha_frames_.clear();
                pending_output_frames_.clear();
                raw_video_cadence_.reset();
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
                    if (!finalize_current_segment_locked(&error_message))
                    {
                        if (error_message.empty())
                        {
                            error_message = "Alpha Recorder failed to finalize the current split alpha mask movie.";
                        }
                    }
                }

                last_alpha_frame_.alpha.reset();
                last_alpha_frame_.timestamp = 0U;
                last_captured_alpha_frame_.alpha.reset();
                last_captured_alpha_frame_.timestamp = 0U;
                pending_alpha_frames_.clear();
                pending_encoded_alpha_frames_.clear();
                pending_output_frames_.clear();
                raw_video_cadence_.reset();

                if (!open_segment_locked(next_recording_path, video_info_, false))
                {
                    session_aborted_ = true;
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

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!session_active_ || session_aborted_ || recording_paused_ || recording_output_ == nullptr ||
                    video_info_.output_width == 0U || video_info_.output_height == 0U)
                {
                    return;
                }

                if (!alpha_extractor_.ensure(video_info_.output_width, video_info_.output_height, error_message) ||
                    !alpha_extractor_.capture_latest(alpha, alpha_timestamp, error_message))
                {
                    session_aborted_ = true;
                    if (error_message.empty())
                    {
                        error_message = "Alpha Recorder failed to capture an alpha mask frame.";
                    }
                }
                else if (!alpha.empty())
                {
                    last_captured_alpha_frame_ = alpha_recorder::obs::AlphaFrame{alpha_timestamp, std::make_shared<std::vector<std::uint8_t>>(std::move(alpha))};
                    remember_pending_alpha_frame_locked(last_captured_alpha_frame_);
                }
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

            remember_output_frame_locked(raw_video_cadence_.remember(frame->data[0], frame->timestamp));
            drain_encoded_alpha_locked(false);
        }

        void on_video_packet(obs_output_t *output, encoder_packet *packet, encoder_packet_time *packet_time)
        {
            (void)packet_time;
            std::lock_guard<std::mutex> lock(mutex_);
            if (output != recording_output_ || packet == nullptr || packet->type != OBS_ENCODER_VIDEO ||
                !session_active_ || session_aborted_ || recording_paused_ || recording_output_ == nullptr ||
                video_info_.output_width == 0U || video_info_.output_height == 0U)
            {
                return;
            }

            queue_alpha_for_packet_locked(packet->pts, packet_time != nullptr ? packet_time->cts : 0U);
        }

        bool event_callback_registered_ = false;
        bool main_rendered_callback_connected_ = false;
        bool raw_video_callback_connected_ = false;
        bool packet_callback_connected_ = false;
        bool file_changed_connected_ = false;
        bool session_active_ = false;
        bool session_aborted_ = false;
        bool recording_paused_ = false;
        std::uint64_t next_sequence_ = 0;
        alpha_recorder::obs::AlphaFrame last_alpha_frame_{};
        alpha_recorder::obs::AlphaFrame last_captured_alpha_frame_{};
        std::deque<alpha_recorder::obs::AlphaFrame> pending_alpha_frames_{};
        std::deque<EncodedAlphaFrame> pending_encoded_alpha_frames_{};
        std::deque<alpha_recorder::obs::OutputFrameCadence> pending_output_frames_{};
        alpha_recorder::obs::RawVideoCadenceTracker raw_video_cadence_{};
        std::filesystem::path recording_path_{};
        obs_video_info video_info_{};
        AlphaPlaneExtractor alpha_extractor_{};
        AlphaMaskVideoWriter writer_{};
        obs_output_t *recording_output_ = nullptr;
        alpha_recorder::obs::FinalizationFormat finalization_format_ = alpha_recorder::obs::FinalizationFormat::MaskPngMov;
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
