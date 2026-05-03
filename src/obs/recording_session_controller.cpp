#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/frame_matcher.hpp"
#include "alpha_recorder/frame_pair.hpp"
#include "alpha_recorder/plugin.hpp"
#include "recording_session_controller_gate.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <util/bmem.h>
#include <util/util_uint64.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{

    using alpha_recorder::obs::AlphaMaskVideoWriter;
    using alpha_recorder::FramePair;

    constexpr std::size_t kMaxPendingFrames = 120U;
    constexpr std::size_t kMaxPendingPackets = 120U;
    constexpr std::size_t kMaxPendingBytes = 32U * 1024U * 1024U;

    struct PendingFrame
    {
        FramePair pair{};
        std::uint64_t composition_timestamp_ns = 0;
    };

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

    bool output_supports_alpha(enum video_format format)
    {
        return format == VIDEO_FORMAT_BGRA || format == VIDEO_FORMAT_RGBA;
    }

    std::vector<std::uint8_t> extract_alpha_plane(const video_data *frame,
                                                  const obs_video_info &video_info)
    {
        if (frame == nullptr || frame->data[0] == nullptr)
        {
            return {};
        }

        const std::uint32_t width = video_info.output_width;
        const std::uint32_t height = video_info.output_height;
        if (width == 0U || height == 0U)
        {
            return {};
        }

        const std::size_t alpha_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (width != 0U && alpha_bytes / static_cast<std::size_t>(width) != static_cast<std::size_t>(height))
        {
            return {};
        }

        const std::size_t source_row_bytes = static_cast<std::size_t>(width) * 4U;
        if (source_row_bytes / 4U != width)
        {
            return {};
        }

        if (static_cast<std::size_t>(frame->linesize[0]) < source_row_bytes)
        {
            return {};
        }

        std::vector<std::uint8_t> alpha(alpha_bytes);
        for (std::uint32_t row = 0; row < height; ++row)
        {
            const std::uint8_t *source_row = frame->data[0] + (static_cast<std::size_t>(row) * static_cast<std::size_t>(frame->linesize[0]));
            std::uint8_t *dest_row = alpha.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width));
            for (std::uint32_t column = 0; column < width; ++column)
            {
                dest_row[column] = source_row[(static_cast<std::size_t>(column) * 4U) + 3U];
            }
        }

        return alpha;
    }

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
                set_recording_paused(true);
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

        static void on_packet(obs_output_t *output, struct encoder_packet *packet, struct encoder_packet_time *packet_time,
                              void *data)
        {
            (void)output;
            static_cast<RecordingSessionController *>(data)->on_packet(packet, packet_time);
        }

        static void on_raw_video(void *data, struct video_data *frame)
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
            if (paused)
            {
                pending_frames_.clear();
                matcher_.clear();
            }
        }

        bool prepare_packet_callback()
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

            recording_output_ = obs_output_get_ref(recording_output);
            if (recording_output_ == nullptr)
            {
                log_and_show_error("Alpha Recorder could not retain a reference to the recording output.", true);
                return false;
            }

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
            if (session_active_)
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

            obs_video_info video_info = {};
            if (!obs_get_video_info(&video_info))
            {
                log_and_show_error("Alpha Recorder could not read the OBS video configuration.", true);
                return false;
            }

            if (!output_supports_alpha(video_info.output_format))
            {
                log_and_show_error("Alpha Recorder requires OBS to use an alpha-preserving video format such as BGRA or RGBA.", true);
                return false;
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
                return false;
            }

            session_active_ = true;
            session_aborted_ = false;
            recording_paused_ = obs_frontend_recording_paused();
            video_info_ = video_info;
            recording_path_ = recording_path;
            next_sequence_ = 0;
            pending_frames_.clear();
            matcher_.clear();
            missing_packet_timing_logged_ = false;

            signal_handler_t *signal_handler = obs_output_get_signal_handler(recording_output_);
            if (signal_handler != nullptr)
            {
                signal_handler_connect(signal_handler, "file_changed", &RecordingSessionController::on_file_changed, this);
                file_changed_connected_ = true;
            }

            struct video_scale_info conversion = {};
            conversion.format = VIDEO_FORMAT_BGRA;
            conversion.width = video_info.output_width;
            conversion.height = video_info.output_height;
            conversion.colorspace = video_info.colorspace;
            conversion.range = video_info.range;
            obs_add_raw_video_callback(&conversion, &RecordingSessionController::on_raw_video, this);
            raw_callback_connected_ = true;
            return true;
        }

        void abort_overflow_locked(std::string_view message, std::string &error_message)
        {
            session_aborted_ = true;
            pending_frames_.clear();
            matcher_.clear();
            error_message.assign(message.data(), message.size());
            if (!writer_.close())
            {
                error_message += " Alpha Recorder also failed to finalize the current alpha mask movie.";
            }
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
            pending_frames_.clear();
            matcher_.clear();
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

        void stop_session(bool show_popup)
        {
            obs_output_t *recording_output = nullptr;
            bool disconnect_packet_callback = false;
            bool disconnect_raw_callback = false;
            bool disconnect_file_changed = false;
            bool finalize_failed = false;
            std::string finalize_error;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!session_active_ && !writer_.is_open() && recording_output_ == nullptr)
                {
                    recording_paused_ = false;
                    return;
                }

                session_active_ = false;
                recording_paused_ = false;
                recording_output = recording_output_;
                recording_output_ = nullptr;
                disconnect_packet_callback = packet_callback_connected_;
                disconnect_raw_callback = raw_callback_connected_;
                disconnect_file_changed = file_changed_connected_;
                packet_callback_connected_ = false;
                raw_callback_connected_ = false;
                file_changed_connected_ = false;

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

                    if (disconnect_packet_callback)
                    {
                        obs_output_remove_packet_callback(recording_output, &RecordingSessionController::on_packet, this);
                    }

                    if (disconnect_raw_callback)
                    {
                        obs_remove_raw_video_callback(&RecordingSessionController::on_raw_video, this);
                    }
                }

                if (!finalize_current_segment_locked(&finalize_error))
                {
                    finalize_failed = true;
                }

                pending_frames_.clear();
                matcher_.clear();
                missing_packet_timing_logged_ = false;
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

        void on_raw_video(struct video_data *frame)
        {
            if (frame == nullptr)
            {
                return;
            }

            std::string error_message;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!alpha_recorder::obs::recording_input_is_allowed(session_active_, session_aborted_, writer_.is_open(), recording_paused_))
                {
                    return;
                }

                std::vector<std::uint8_t> alpha = extract_alpha_plane(frame, video_info_);
                if (alpha.empty())
                {
                    return;
                }

                if (!writer_.write_frame(alpha.data(), video_info_.output_width, &error_message))
                {
                    session_aborted_ = true;
                    pending_frames_.clear();
                    matcher_.clear();
                    if (error_message.empty())
                    {
                        error_message = "Alpha Recorder failed to write an alpha mask frame.";
                    }
                }
                ++next_sequence_;
            }

            if (!error_message.empty())
            {
                log_and_show_error(error_message, false);
            }
        }

        void on_packet(struct encoder_packet *packet, struct encoder_packet_time *packet_time)
        {
            (void)packet;
            (void)packet_time;
        }

        bool event_callback_registered_ = false;
        bool packet_callback_connected_ = false;
        bool raw_callback_connected_ = false;
        bool file_changed_connected_ = false;
        bool session_active_ = false;
        bool session_aborted_ = false;
        bool recording_paused_ = false;
        bool missing_packet_timing_logged_ = false;
        std::uint64_t next_sequence_ = 0;
        std::deque<PendingFrame> pending_frames_{};
        alpha_recorder::AlphaFrameMatcher matcher_{kMaxPendingFrames, kMaxPendingPackets, kMaxPendingBytes};
        std::filesystem::path recording_path_{};
        obs_video_info video_info_{};
        AlphaMaskVideoWriter writer_{};
        obs_output_t *recording_output_ = nullptr;
        alpha_recorder::obs::FinalizationFormat finalization_format_ = alpha_recorder::obs::FinalizationFormat::MaskProRes422;
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
