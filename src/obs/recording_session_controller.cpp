#include "alpha_recorder/plugin.hpp"
#include "alpha_alignment_engine.hpp"
#include "alpha_plane_extractor.hpp"
#include "alpha_output_sink.hpp"
#include "diagnostic_log.hpp"
#include "gpu_texture_recording_output.hpp"
#include "recording_session_controller_cadence.hpp"
#include "recording_session_controller_gate.hpp"
#include "recording_telemetry.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <obs-frontend-api.h>
#include <obs-module.h>

extern "C"
{
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <util/bmem.h>
#include <util/config-file.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{

    using alpha_recorder::obs::AlphaOutputSinkStats;
    using alpha_recorder::obs::AlphaAlignmentEngine;
    using alpha_recorder::obs::AlphaAlignmentEngineConfig;
    using alpha_recorder::obs::AlphaAlignmentDrainResult;
    using alpha_recorder::obs::AlignmentTraceEvent;
    using alpha_recorder::obs::AlphaPlaneExtractor;
    using alpha_recorder::obs::CaptureTiming;
    using alpha_recorder::obs::LivePipelineTelemetry;
    using alpha_recorder::obs::bool_text;
    using alpha_recorder::obs::format_bytes;
    using alpha_recorder::obs::format_signed_delta_summary;
    using alpha_recorder::obs::format_timing_summary;
    using alpha_recorder::obs::ns_to_ms;
    using alpha_recorder::obs::alignment_alpha_queue_frame_limit;
    using alpha_recorder::obs::alignment_output_queue_frame_limit;
    using alpha_recorder::obs::plausible_alignment_delta_ns;
    using alpha_recorder::obs::timestamp_span_ms;

    constexpr std::uint32_t kGpuTextureDeactivateTimeoutMs = 10000U;
    constexpr std::uint32_t kGpuTextureTailCoverageTimeoutMs = 1000U;

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

    struct VideoPacketCountProbe
    {
        bool ok = false;
        std::uint64_t packet_count = 0U;
        std::string error{};
    };

    VideoPacketCountProbe probe_video_packet_count(const std::filesystem::path &path)
    {
        VideoPacketCountProbe result{};
        if (path.empty())
        {
            result.error = "recording path is empty";
            return result;
        }

        std::error_code exists_error;
        if (!std::filesystem::exists(path, exists_error) || exists_error)
        {
            result.error = exists_error ? exists_error.message() : "recording file does not exist yet";
            return result;
        }

        AVFormatContext *format = nullptr;
        const std::string path_text = path_to_utf8(path);
        int ret = avformat_open_input(&format, path_text.c_str(), nullptr, nullptr);
        if (ret < 0)
        {
            result.error = std::string{"avformat_open_input failed: "} + av_error_message(ret);
            return result;
        }

        ret = avformat_find_stream_info(format, nullptr);
        if (ret < 0)
        {
            result.error = std::string{"avformat_find_stream_info failed: "} + av_error_message(ret);
            avformat_close_input(&format);
            return result;
        }

        int video_stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream_index < 0)
        {
            for (unsigned index = 0; index < format->nb_streams; ++index)
            {
                AVStream *stream = format->streams[index];
                if (stream != nullptr && stream->codecpar != nullptr &&
                    stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                {
                    video_stream_index = static_cast<int>(index);
                    break;
                }
            }
        }
        if (video_stream_index < 0)
        {
            result.error = "recording file has no video stream";
            avformat_close_input(&format);
            return result;
        }

        AVPacket *packet = av_packet_alloc();
        if (packet == nullptr)
        {
            result.error = "av_packet_alloc failed";
            avformat_close_input(&format);
            return result;
        }

        while ((ret = av_read_frame(format, packet)) >= 0)
        {
            if (packet->stream_index == video_stream_index)
            {
                ++result.packet_count;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        avformat_close_input(&format);

        if (ret != AVERROR_EOF)
        {
            result.error = std::string{"av_read_frame failed: "} + av_error_message(ret);
            result.packet_count = 0U;
            return result;
        }

        result.ok = true;
        return result;
    }

    bool text_equals(const char *text, const char *expected) noexcept
    {
        if (text == nullptr || expected == nullptr)
        {
            return false;
        }

        return std::string_view{text} == expected;
    }

    std::filesystem::path active_profile_recording_directory(config_t *profile_config)
    {
        if (profile_config == nullptr)
        {
            return {};
        }

        std::filesystem::path directory{};
        const char *mode = config_get_string(profile_config, "Output", "Mode");
        if (text_equals(mode, "Advanced"))
        {
            const char *recording_type = config_get_string(profile_config, "AdvOut", "RecType");
            if (text_equals(recording_type, "FFmpeg"))
            {
                if (config_get_bool(profile_config, "AdvOut", "FFOutputToFile"))
                {
                    directory = path_from_utf8(config_get_string(profile_config, "AdvOut", "FFFilePath"));
                }
            }
            else
            {
                directory = path_from_utf8(config_get_string(profile_config, "AdvOut", "RecFilePath"));
            }
        }
        else
        {
            directory = path_from_utf8(config_get_string(profile_config, "SimpleOutput", "FilePath"));
        }

        return directory;
    }

    std::string active_profile_recording_format(config_t *profile_config)
    {
        if (profile_config == nullptr)
        {
            return {};
        }

        const char *mode = config_get_string(profile_config, "Output", "Mode");
        if (text_equals(mode, "Advanced"))
        {
            const char *recording_type = config_get_string(profile_config, "AdvOut", "RecType");
            if (text_equals(recording_type, "FFmpeg"))
            {
                const char *format = config_get_string(profile_config, "AdvOut", "FFExtension");
                return format == nullptr ? std::string{} : std::string{format};
            }
            const char *format = config_get_string(profile_config, "AdvOut", "RecFormat2");
            return format == nullptr ? std::string{} : std::string{format};
        }

        const char *format = config_get_string(profile_config, "SimpleOutput", "RecFormat2");
        return format == nullptr ? std::string{} : std::string{format};
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

    std::filesystem::path current_recording_file_path(obs_output_t *recording_output)
    {
        std::filesystem::path recording_path = recording_file_path_from_output(recording_output);

        char *recording_path_text = obs_frontend_get_current_record_output_path();
        if (recording_path.empty() && recording_path_text != nullptr && *recording_path_text != '\0')
        {
            recording_path = path_from_utf8(recording_path_text);
        }
        if (recording_path_text != nullptr)
        {
            bfree(recording_path_text);
        }

        return path_is_directory(recording_path) ? std::filesystem::path{} : recording_path;
    }

    std::filesystem::path recording_output_directory(obs_output_t *recording_output)
    {
        std::filesystem::path directory{};
        if (recording_output != nullptr)
        {
            obs_data_t *settings = obs_output_get_settings(recording_output);
            if (settings != nullptr)
            {
                std::filesystem::path configured_path = path_from_utf8(obs_data_get_string(settings, "url"));
                if (configured_path.empty())
                {
                    configured_path = path_from_utf8(obs_data_get_string(settings, "path"));
                }
                obs_data_release(settings);

                if (!configured_path.empty())
                {
                    directory = path_is_directory(configured_path) ? configured_path : configured_path.parent_path();
                }
            }
        }

        if (directory.empty())
        {
            config_t *profile_config = obs_frontend_get_profile_config();
            if (profile_config != nullptr)
            {
                directory = active_profile_recording_directory(profile_config);
            }
        }

        if (directory.empty())
        {
            const std::filesystem::path current_path = current_recording_file_path(recording_output);
            directory = current_path.parent_path();
        }
        if (directory.empty())
        {
            std::error_code error;
            directory = std::filesystem::temp_directory_path(error);
            if (error)
            {
                directory.clear();
            }
        }

        return directory;
    }

    alpha_recorder::obs::AlphaMovieContainer gpu_texture_output_container(obs_output_t *recording_output,
                                                                          alpha_recorder::obs::FinalizationFormat format)
    {
        std::filesystem::path recording_path = current_recording_file_path(recording_output);
        if (!recording_path.empty())
        {
            return alpha_recorder::obs::alpha_movie_container_for_recording_path(recording_path, format);
        }

        config_t *profile_config = obs_frontend_get_profile_config();
        return alpha_recorder::obs::alpha_movie_container_for_recording_format(
            active_profile_recording_format(profile_config), format);
    }

    std::filesystem::path temporary_gpu_texture_output_path(obs_output_t *recording_output,
                                                            alpha_recorder::obs::FinalizationFormat format)
    {
        const std::filesystem::path directory = recording_output_directory(recording_output);
        if (directory.empty())
        {
            return {};
        }

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        const auto container = gpu_texture_output_container(recording_output, format);
        return directory / (std::string{".alpha-recorder-"} + std::to_string(ticks) + ".alpha.tmp" +
                            std::string{alpha_recorder::obs::alpha_movie_container_extension(container)});
    }

    bool move_completed_gpu_texture_output(const std::filesystem::path &temporary_path,
                                           const std::filesystem::path &final_path,
                                           std::string *error_message)
    {
        if (temporary_path.empty() || final_path.empty() || temporary_path == final_path)
        {
            return true;
        }

        std::error_code error;
        const std::filesystem::path parent = final_path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                if (error_message != nullptr)
                {
                    *error_message =
                        std::string{"Alpha Recorder could not create the final GPU texture alpha output directory: "} +
                        error.message();
                }
                return false;
            }
        }

#ifdef _WIN32
        if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error = std::error_code{static_cast<int>(GetLastError()), std::system_category()};
        }
#else
        std::filesystem::rename(temporary_path, final_path, error);
#endif
        if (error)
        {
            if (error_message != nullptr)
            {
                *error_message =
                    std::string{"Alpha Recorder could not rename the temporary GPU texture alpha movie to the final output path: "} +
                    error.message();
            }
            return false;
        }

        return true;
    }

    void remove_failed_gpu_texture_output(const std::filesystem::path &writer_path,
                                          const std::filesystem::path &final_path,
                                          const std::string &reason) noexcept
    {
        if (writer_path.empty())
        {
            return;
        }

        std::error_code error;
        const bool removed = std::filesystem::remove(writer_path, error);
        blog(error ? LOG_WARNING : LOG_INFO,
             "Alpha Recorder removed failed GPU texture alpha movie: path=\"%s\" final=\"%s\" removed=%s reason=\"%s\" remove_error=\"%s\"",
             writer_path.generic_u8string().c_str(),
             final_path.generic_u8string().c_str(),
             removed ? "true" : "false",
             reason.c_str(),
             error ? error.message().c_str() : "");
    }

    std::string nonfatal_sync_log_reason(std::string reason)
    {
        constexpr std::string_view prefix = "Alpha Recorder ";
        if (reason.rfind(prefix, 0U) == 0U)
        {
            reason.erase(0U, prefix.size());
        }
        return reason;
    }

    bool settings_use_gpu_texture_path(const alpha_recorder::obs::Settings &settings) noexcept
    {
        return alpha_recorder::obs::finalization_format_uses_gpu_texture_path(settings.finalization_format);
    }

    obs_data_t *make_gpu_texture_output_settings(const std::filesystem::path &mask_path,
                                                 const obs_video_info &video_info,
                                                 const alpha_recorder::obs::Settings &settings,
                                                 bool main_texture_encoded)
    {
        obs_data_t *data = obs_data_create();
        const std::string mask_path_utf8 = path_to_utf8(mask_path);
        obs_data_set_string(data, "path", mask_path_utf8.c_str());
        const char *encoder_id =
            alpha_recorder::obs::gpu_texture_hevc_encoder_id_for_format(settings.finalization_format);
        obs_data_set_string(data, "encoder_id", encoder_id != nullptr ? encoder_id : "");
        obs_data_set_int(data, "width", static_cast<long long>(video_info.output_width));
        obs_data_set_int(data, "height", static_cast<long long>(video_info.output_height));
        obs_data_set_int(data, "fps_num", static_cast<long long>(video_info.fps_num));
        obs_data_set_int(data, "fps_den", static_cast<long long>(video_info.fps_den));
        obs_data_set_string(data, alpha_recorder::obs::settings_hevc_quality_profile_key().data(),
                            alpha_recorder::obs::hevc_quality_profile_config_value(
                                settings.hevc_encoder.quality_profile)
                                .data());
        obs_data_set_int(data, alpha_recorder::obs::settings_hevc_quality_cq_key().data(),
                         settings.hevc_encoder.quality_cq);
        obs_data_set_string(data, alpha_recorder::obs::settings_hevc_preset_key().data(),
                            alpha_recorder::obs::hevc_encoder_preset_config_value(
                                settings.hevc_encoder.preset)
                                .data());
        obs_data_set_string(data, alpha_recorder::obs::settings_hevc_nvenc_tune_key().data(),
                            alpha_recorder::obs::hevc_nvenc_tune_config_value(
                                settings.hevc_encoder.nvenc_tune)
                                .data());
        obs_data_set_int(data, alpha_recorder::obs::settings_hevc_gop_size_key().data(),
                         settings.hevc_encoder.gop_size);
        obs_data_set_bool(data, alpha_recorder::obs::settings_hevc_adaptive_quantization_key().data(),
                          settings.hevc_encoder.adaptive_quantization);
        obs_data_set_string(data, alpha_recorder::obs::settings_hevc_nvenc_split_encode_key().data(),
                            alpha_recorder::obs::hevc_nvenc_split_encode_config_value(
                                settings.hevc_encoder.nvenc_split_encode)
                                .data());
        obs_data_set_int(data, alpha_recorder::obs::settings_hevc_nvenc_gpu_index_key().data(),
                         settings.hevc_encoder.nvenc_gpu_index);
        obs_data_set_bool(data, "main_texture_encoded", main_texture_encoded);
        return data;
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
            gpu_texture_path_ = settings_use_gpu_texture_path(settings_);
            session_active_ = true;
            session_aborted_ = false;
            recording_paused_ = obs_frontend_recording_paused();
            video_info_ = video_info;
            recording_path_.clear();
            next_sequence_ = 0;
            reset_gpu_texture_timing_locked();
            alignment_engine_.reset_all();
            raw_video_cadence_.reset();
            live_telemetry_.reset();
            recording_texture_encoded_ =
                recording_output_uses_texture_encoder(recording_output_, video_info_.output_format);

            if (!gpu_texture_path_)
            {
                start_alignment_worker_locked();
                obs_add_main_rendered_callback(&RecordingSessionController::on_main_rendered, this);
                main_rendered_callback_connected_ = true;
                obs_add_raw_video_callback(nullptr, &RecordingSessionController::on_raw_video, this);
                raw_video_callback_connected_ = true;
            }
            else
            {
                const std::filesystem::path temporary_mask_path =
                    temporary_gpu_texture_output_path(recording_output_, settings.finalization_format);
                if (!temporary_mask_path.empty())
                {
                    if (!open_gpu_texture_segment_locked({}, temporary_mask_path, video_info, false))
                    {
                        return false;
                    }
                    gpu_texture_output_is_temporary_ = true;
                }
                signal_handler_t *signal_handler = obs_output_get_signal_handler(recording_output_);
                if (signal_handler != nullptr && !file_changed_connected_)
                {
                    signal_handler_connect(signal_handler, "file_changed", &RecordingSessionController::on_file_changed, this);
                    file_changed_connected_ = true;
                }
            }
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
            const std::uint64_t recording_started_video_time = obs_get_video_frame_time();

            std::lock_guard<std::mutex> lock(mutex_);
            const bool use_gpu_texture_path = settings_use_gpu_texture_path(settings);
            if (!use_gpu_texture_path && alpha_sink_.is_open())
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

            std::filesystem::path recording_path = current_recording_file_path(recording_output);

            if (recording_path.empty())
            {
                log_and_show_error("Alpha Recorder could not determine the recording file path.", true);
                return false;
            }

            if (path_is_directory(recording_path))
            {
                log_and_show_error(std::string{"Alpha Recorder could not determine the recording file name; OBS only reported the recording folder: "} +
                                       recording_path.generic_u8string(),
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
            gpu_texture_path_ = use_gpu_texture_path;

            if (!open_segment_locked(recording_path, video_info, true))
            {
                obs_output_release(recording_output_);
                recording_output_ = nullptr;
                session_active_ = false;
                alignment_engine_.clear_pending();
                return false;
            }

            session_active_ = true;
            session_aborted_ = false;
            recording_paused_ = obs_frontend_recording_paused();
            video_info_ = video_info;
            recording_path_ = recording_path;
            if (gpu_texture_path_)
            {
                gpu_recording_started_video_time_ = recording_started_video_time;
            }
            const bool detected_texture_encoded =
                recording_output_uses_texture_encoder(recording_output_, video_info_.output_format);
            recording_texture_encoded_ = recording_texture_encoded_ || detected_texture_encoded;
            if (gpu_texture_path_ && gpu_texture_output_ != nullptr)
            {
                alpha_recorder::obs::gpu_texture_recording_output_set_main_texture_encoded(
                    gpu_texture_output_, recording_texture_encoded_);
            }
            if (!gpu_texture_path_)
            {
                if (recording_texture_encoded_)
                {
                    alignment_engine_.mark_pending_encoded_texture_encoded();
                }
                start_alignment_worker_locked();
                notify_alignment_worker_locked();
            }

            signal_handler_t *signal_handler = obs_output_get_signal_handler(recording_output_);
            if (signal_handler != nullptr && !file_changed_connected_)
            {
                signal_handler_connect(signal_handler, "file_changed", &RecordingSessionController::on_file_changed, this);
                file_changed_connected_ = true;
            }

            if (!gpu_texture_path_ && !main_rendered_callback_connected_)
            {
                obs_add_main_rendered_callback(&RecordingSessionController::on_main_rendered, this);
                main_rendered_callback_connected_ = true;
            }
            if (!gpu_texture_path_ && !raw_video_callback_connected_)
            {
                obs_add_raw_video_callback(nullptr, &RecordingSessionController::on_raw_video, this);
                raw_video_callback_connected_ = true;
            }
            if (!packet_callback_connected_)
            {
                obs_output_add_packet_callback(recording_output_, &RecordingSessionController::on_video_packet, this);
                packet_callback_connected_ = true;
            }
            // Encoder startup packets can still arrive out of PTS order here; let
            // the alignment worker wait for its reorder window before consuming them.
            return true;
        }

        bool open_segment_locked(const std::filesystem::path &recording_path, const obs_video_info &video_info, bool show_popup)
        {
            const std::filesystem::path mask_path = alpha_recorder::obs::recording_alpha_movie_path(recording_path, finalization_format_);

            if (gpu_texture_path_)
            {
                return open_gpu_texture_segment_locked(recording_path, mask_path, video_info, show_popup);
            }

            alpha_recorder::obs::AlphaOutputSinkConfig config{};
            config.output_path = mask_path;
            config.finalization_format = finalization_format_;
            config.width = video_info.output_width;
            config.height = video_info.output_height;
            config.fps_num = video_info.fps_num;
            config.fps_den = video_info.fps_den;
            config.hevc_encoder = settings_.hevc_encoder;
            max_pending_alpha_frames_ =
                alignment_alpha_queue_frame_limit(config.fps_num, config.fps_den);
            max_pending_output_frames_ =
                alignment_output_queue_frame_limit(config.fps_num, config.fps_den);
            alignment_engine_.configure(AlphaAlignmentEngineConfig{config.width,
                                                                   config.height,
                                                                   config.fps_num,
                                                                   config.fps_den,
                                                                   max_pending_alpha_frames_,
                                                                   max_pending_output_frames_,
                                                                   kMaxEncoderReorderFrames});

            std::string writer_error;
            if (!alpha_sink_.open(config, &writer_error))
            {
                const std::string message = writer_error.empty()
                                                ? std::string{"Alpha Recorder could not open the alpha mask movie for recording path: "} +
                                                      recording_path.generic_u8string()
                                                : writer_error;
                if (settings_.diagnostic_logging)
                {
                    alpha_recorder::obs::append_diagnostic_log_line(std::string{"Alpha Recorder segment open failed: path=\""} +
                                                                    mask_path.generic_u8string() + "\" error=\"" + message + "\"");
                }
                log_and_show_error(message,
                                   show_popup);
                return false;
            }

            video_info_ = video_info;
            recording_path_ = recording_path;
            session_aborted_ = false;
            live_telemetry_.reset();
            log_segment_start_locked(mask_path, config);
            return true;
        }

        bool open_gpu_texture_segment_locked(const std::filesystem::path &recording_path,
                                             const std::filesystem::path &mask_path,
                                             const obs_video_info &video_info,
                                             bool show_popup)
        {
            if (gpu_texture_output_ != nullptr &&
                (obs_output_active(gpu_texture_output_) || gpu_texture_output_is_temporary_))
            {
                if (gpu_texture_output_path_ == mask_path ||
                    (gpu_texture_output_is_temporary_ &&
                     (gpu_texture_final_output_path_.empty() || gpu_texture_final_output_path_ == mask_path)))
                {
                    if (gpu_texture_output_is_temporary_ && gpu_texture_output_path_ != mask_path)
                    {
                        gpu_texture_final_output_path_ = mask_path;
                        blog(LOG_INFO,
                             "Alpha Recorder GPU texture bound temporary output to final path: temp=\"%s\" final=\"%s\"",
                             gpu_texture_output_path_.generic_u8string().c_str(),
                             gpu_texture_final_output_path_.generic_u8string().c_str());
                    }
                    video_info_ = video_info;
                    recording_path_ = recording_path;
                    return true;
                }
            }

            if (gpu_texture_output_ != nullptr)
            {
                std::string finalize_error;
                (void)finalize_gpu_texture_segment_locked(&finalize_error);
            }

            std::string unavailable_reason;
            if (!alpha_recorder::obs::gpu_texture_hevc_encoder_runtime_available(
                    settings_.finalization_format, &unavailable_reason))
            {
                const std::string message =
                    unavailable_reason.empty()
                        ? "Alpha Recorder could not find the selected GPU texture alpha encoder."
                        : unavailable_reason;
                log_and_show_error(message, show_popup);
                return false;
            }

            obs_data_t *output_settings = make_gpu_texture_output_settings(
                mask_path, video_info, settings_, recording_texture_encoded_);
            gpu_texture_output_ = obs_output_create(alpha_recorder::obs::gpu_texture_recording_output_id(),
                                                    "Alpha Recorder GPU Texture Recording",
                                                    output_settings,
                                                    nullptr);
            obs_data_release(output_settings);
            if (gpu_texture_output_ == nullptr)
            {
                const std::string message = "Alpha Recorder could not create the GPU texture alpha output.";
                log_and_show_error(message, show_popup);
                return false;
            }

            reset_gpu_texture_timing_locked();
            gpu_texture_output_path_ = mask_path;
            gpu_texture_final_output_path_.clear();
            if (!obs_output_start(gpu_texture_output_))
            {
                const char *last_error = obs_output_get_last_error(gpu_texture_output_);
                const std::string message = last_error != nullptr && *last_error != '\0'
                                                ? last_error
                                                : "Alpha Recorder could not start the GPU texture alpha output.";
                obs_output_release(gpu_texture_output_);
                gpu_texture_output_ = nullptr;
                gpu_texture_output_path_.clear();
                gpu_texture_final_output_path_.clear();
                gpu_texture_output_is_temporary_ = false;
                log_and_show_error(message, show_popup);
                return false;
            }
            gpu_texture_output_start_video_time_ = obs_get_video_frame_time();

            alpha_recorder::obs::AlphaOutputSinkConfig log_config{};
            log_config.output_path = mask_path;
            log_config.finalization_format = finalization_format_;
            log_config.width = video_info.output_width;
            log_config.height = video_info.output_height;
            log_config.fps_num = video_info.fps_num;
            log_config.fps_den = video_info.fps_den;
            log_config.hevc_encoder = settings_.hevc_encoder;
            max_pending_alpha_frames_ = 0U;
            max_pending_output_frames_ = 0U;
            video_info_ = video_info;
            recording_path_ = recording_path;
            session_aborted_ = false;
            live_telemetry_.reset();
            log_segment_start_locked(mask_path, log_config);
            return true;
        }

        bool finalize_current_segment_locked(std::string *error_message)
        {
            if (gpu_texture_path_)
            {
                return finalize_gpu_texture_segment_locked(error_message);
            }

            if (!alpha_sink_.is_open())
            {
                return true;
            }

            const std::filesystem::path writer_path = alpha_sink_.path();
            AlphaOutputSinkStats writer_stats{};
            if (!alpha_sink_.close(error_message, &writer_stats))
            {
                log_performance_summary_locked(writer_path, writer_stats);
                return false;
            }

            log_performance_summary_locked(writer_path, writer_stats);
            return true;
        }

        void clamp_gpu_texture_visible_range_to_recording_file_locked(
            alpha_recorder::obs::AlphaVisiblePacketRange &visible_range)
        {
            if (visible_range.duration <= 0 || recording_path_.empty())
            {
                return;
            }

            const std::uint64_t callback_packet_count =
                static_cast<std::uint64_t>(gpu_main_packets_.size());
            if (callback_packet_count == 0U)
            {
                return;
            }

            std::int64_t alpha_pts_step = 0;
            if (visible_range.duration % static_cast<std::int64_t>(callback_packet_count) == 0)
            {
                alpha_pts_step =
                    visible_range.duration / static_cast<std::int64_t>(callback_packet_count);
            }
            else if (video_info_.fps_den > 0U && visible_range.duration % video_info_.fps_den == 0)
            {
                alpha_pts_step = static_cast<std::int64_t>(video_info_.fps_den);
            }

            if (alpha_pts_step <= 0)
            {
                blog(LOG_WARNING,
                     "[alpha_recorder] could not infer alpha PTS step for main-file duration clamp: path=\"%s\" visible_duration=%lld main_callback_packets=%llu fps=%u/%u",
                     recording_path_.generic_u8string().c_str(),
                     static_cast<long long>(visible_range.duration),
                     static_cast<unsigned long long>(callback_packet_count),
                     video_info_.fps_num,
                     video_info_.fps_den);
                return;
            }

            const std::uint64_t solved_visible_frames =
                static_cast<std::uint64_t>(visible_range.duration / alpha_pts_step);
            if (solved_visible_frames == 0U)
            {
                return;
            }

            const VideoPacketCountProbe probe = probe_video_packet_count(recording_path_);
            if (!probe.ok)
            {
                blog(LOG_WARNING,
                     "[alpha_recorder] could not probe main recording video packet count for GPU alpha duration clamp: path=\"%s\" reason=\"%s\"",
                     recording_path_.generic_u8string().c_str(),
                     probe.error.c_str());
                return;
            }

            if (probe.packet_count >= solved_visible_frames)
            {
                if (probe.packet_count > solved_visible_frames)
                {
                    blog(LOG_INFO,
                         "[alpha_recorder] main recording has more video packets than certified GPU alpha range; keeping alpha duration unchanged: path=\"%s\" main_video_packets=%llu solved_visible_frames=%llu duration=%lld",
                         recording_path_.generic_u8string().c_str(),
                         static_cast<unsigned long long>(probe.packet_count),
                         static_cast<unsigned long long>(solved_visible_frames),
                         static_cast<long long>(visible_range.duration));
                }
                return;
            }

            if (probe.packet_count >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / alpha_pts_step))
            {
                blog(LOG_WARNING,
                     "[alpha_recorder] main recording packet count is too large for GPU alpha duration clamp: path=\"%s\" main_video_packets=%llu alpha_pts_step=%lld",
                     recording_path_.generic_u8string().c_str(),
                     static_cast<unsigned long long>(probe.packet_count),
                     static_cast<long long>(alpha_pts_step));
                return;
            }

            const std::int64_t previous_duration = visible_range.duration;
            visible_range.duration = static_cast<std::int64_t>(probe.packet_count) * alpha_pts_step;
            blog(LOG_INFO,
                 "[alpha_recorder] clamped GPU texture alpha visible duration to written main recording frames: path=\"%s\" main_video_packets=%llu main_callback_packets=%llu solved_visible_frames=%llu media_time=%lld duration=%lld previous_duration=%lld alpha_pts_step=%lld",
                 recording_path_.generic_u8string().c_str(),
                 static_cast<unsigned long long>(probe.packet_count),
                 static_cast<unsigned long long>(callback_packet_count),
                 static_cast<unsigned long long>(solved_visible_frames),
                 static_cast<long long>(visible_range.media_time),
                 static_cast<long long>(visible_range.duration),
                 static_cast<long long>(previous_duration),
                 static_cast<long long>(alpha_pts_step));
            if (settings_.diagnostic_logging)
            {
                alpha_recorder::obs::append_diagnostic_log_line(
                    "Alpha Recorder clamped GPU texture alpha visible duration to main recording: path=\"" +
                    recording_path_.generic_u8string() + "\" main_video_packets=\"" +
                    std::to_string(probe.packet_count) + "\" main_callback_packets=\"" +
                    std::to_string(callback_packet_count) + "\" solved_visible_frames=\"" +
                    std::to_string(solved_visible_frames) + "\" media_time=\"" +
                    std::to_string(visible_range.media_time) + "\" duration=\"" +
                    std::to_string(visible_range.duration) + "\" previous_duration=\"" +
                    std::to_string(previous_duration) + "\" alpha_pts_step=\"" +
                    std::to_string(alpha_pts_step) + "\"");
            }
        }

        bool finalize_gpu_texture_segment_locked(std::string *error_message)
        {
            if (gpu_texture_output_ == nullptr)
            {
                return true;
            }

            const std::filesystem::path actual_writer_path = gpu_texture_output_path_;
            const std::filesystem::path final_writer_path =
                !gpu_texture_final_output_path_.empty() ? gpu_texture_final_output_path_ : actual_writer_path;
            const bool rename_temporary_output =
                gpu_texture_output_is_temporary_ && !gpu_texture_final_output_path_.empty() &&
                actual_writer_path != final_writer_path;
            alpha_recorder::obs::AlphaVisiblePacketRange visible_range{};
            std::string range_error;
            bool finalize_failed = false;
            bool data_capture_end_requested = false;
            std::string finalize_failure_reason;
            const bool strict_sync_proof = settings_.fail_close_on_sync_proof_failure;

            // Encoder drain cannot create replay frames that have not reached the
            // auxiliary video mix yet. Keep the alpha output live briefly so its
            // queued main-packet generations can catch the main recording tail.
            const auto tail_coverage_deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kGpuTextureTailCoverageTimeoutMs);
            do
            {
                range_error.clear();
                if (alpha_recorder::obs::gpu_texture_recording_output_compute_visible_range(
                        gpu_texture_output_,
                        gpu_main_packets_,
                        recording_texture_encoded_,
                        visible_range,
                        &range_error))
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            } while (std::chrono::steady_clock::now() < tail_coverage_deadline);

            alpha_recorder::obs::gpu_texture_recording_output_request_stop(gpu_texture_output_);
            if (!alpha_recorder::obs::gpu_texture_recording_output_wait_stop_boundary(
                    gpu_texture_output_, kGpuTextureDeactivateTimeoutMs, error_message))
            {
                finalize_failed = true;
            }
            if (!finalize_failed)
            {
                alpha_recorder::obs::gpu_texture_recording_output_end_data_capture(gpu_texture_output_);
                data_capture_end_requested = true;
                if (!alpha_recorder::obs::gpu_texture_recording_output_wait_deactivated(
                        gpu_texture_output_, kGpuTextureDeactivateTimeoutMs, error_message))
                {
                    finalize_failed = true;
                }
            }

            bool visible_range_certified = false;
            if (!finalize_failed)
            {
                range_error.clear();
                if (alpha_recorder::obs::gpu_texture_recording_output_compute_visible_range(
                        gpu_texture_output_,
                        gpu_main_packets_,
                        recording_texture_encoded_,
                        visible_range,
                        &range_error))
                {
                    visible_range_certified = true;
                }
            }

            if (!finalize_failed && !visible_range_certified)
            {
                const std::string sync_failure_reason =
                    range_error.empty()
                        ? "Alpha Recorder could not compute the GPU texture alpha visible range."
                        : range_error;
                const bool has_partial_visible_range = visible_range.duration > 0;
                if (strict_sync_proof)
                {
                    finalize_failed = true;
                    if (error_message != nullptr)
                    {
                        *error_message = sync_failure_reason;
                    }
                }
                else
                {
                    const std::string log_reason = nonfatal_sync_log_reason(sync_failure_reason);
                    blog(LOG_WARNING,
                         "[alpha_recorder] sync proof not certified; publishing GPU texture alpha movie %s: path=\"%s\" final=\"%s\" media_time=%lld duration=%lld reason=\"%s\"",
                         has_partial_visible_range ? "with a best-effort edit range" : "without an edit range",
                         actual_writer_path.generic_u8string().c_str(),
                         final_writer_path.generic_u8string().c_str(),
                         static_cast<long long>(visible_range.media_time),
                         static_cast<long long>(visible_range.duration),
                         log_reason.c_str());
                    if (settings_.diagnostic_logging)
                    {
                        alpha_recorder::obs::append_diagnostic_log_line(
                            std::string{
                                "Alpha Recorder sync proof was not certified; publishing "} +
                            (has_partial_visible_range ? "with a best-effort edit range" : "without an edit range") +
                            ": path=\"" + actual_writer_path.generic_u8string() + "\" final=\"" +
                            final_writer_path.generic_u8string() + "\" media_time=\"" +
                            std::to_string(visible_range.media_time) + "\" duration=\"" +
                            std::to_string(visible_range.duration) + "\" reason=\"" +
                            sync_failure_reason + "\"");
                    }
                }
            }
            if (!finalize_failed && visible_range.duration > 0)
            {
                clamp_gpu_texture_visible_range_to_recording_file_locked(visible_range);
            }
            if (!finalize_failed && visible_range.duration > 0)
            {
                std::string visible_range_error;
                if (!alpha_recorder::obs::gpu_texture_recording_output_set_visible_range(
                        gpu_texture_output_, visible_range, &visible_range_error))
                {
                    if (strict_sync_proof)
                    {
                        finalize_failed = true;
                        if (error_message != nullptr)
                        {
                            *error_message = visible_range_error.empty()
                                                 ? "Alpha Recorder could not apply the GPU texture alpha visible range."
                                                 : visible_range_error;
                        }
                    }
                    else
                    {
                        alpha_recorder::obs::AlphaVisiblePacketRange no_visible_range{};
                        std::string clear_range_error;
                        (void)alpha_recorder::obs::gpu_texture_recording_output_set_visible_range(
                            gpu_texture_output_, no_visible_range, &clear_range_error);
                        const std::string sync_failure_reason =
                            visible_range_error.empty()
                                ? "Alpha Recorder could not apply the GPU texture alpha visible range."
                                : visible_range_error;
                        visible_range_certified = false;
                        const std::string log_reason = nonfatal_sync_log_reason(sync_failure_reason);
                        blog(LOG_WARNING,
                             "[alpha_recorder] sync proof edit range not applied; publishing GPU texture alpha movie without an edit range: path=\"%s\" final=\"%s\" reason=\"%s\"",
                             actual_writer_path.generic_u8string().c_str(),
                             final_writer_path.generic_u8string().c_str(),
                             log_reason.c_str());
                        if (settings_.diagnostic_logging)
                        {
                            alpha_recorder::obs::append_diagnostic_log_line(
                                std::string{
                                    "Alpha Recorder sync proof edit range was not applied; publishing without an edit range: path=\""} +
                                actual_writer_path.generic_u8string() + "\" final=\"" +
                                final_writer_path.generic_u8string() + "\" reason=\"" +
                                sync_failure_reason + "\"");
                        }
                    }
                }
            }
            if (!finalize_failed && !alpha_recorder::obs::gpu_texture_recording_output_finalize_mux(
                                      gpu_texture_output_, error_message))
            {
                finalize_failed = true;
            }
            if (finalize_failed)
            {
                finalize_failure_reason =
                    error_message != nullptr && !error_message->empty()
                        ? *error_message
                        : "Alpha Recorder failed to certify or finalize the GPU texture alpha movie.";
                if (!data_capture_end_requested)
                {
                    std::string deactivate_error;
                    alpha_recorder::obs::gpu_texture_recording_output_request_stop(gpu_texture_output_);
                    alpha_recorder::obs::gpu_texture_recording_output_end_data_capture(gpu_texture_output_);
                    (void)alpha_recorder::obs::gpu_texture_recording_output_wait_deactivated(
                        gpu_texture_output_, kGpuTextureDeactivateTimeoutMs, &deactivate_error);
                }
                alpha_recorder::obs::gpu_texture_recording_output_abort_mux(gpu_texture_output_);
            }

            const alpha_recorder::obs::GpuTextureRecordingOutputStats gpu_stats =
                alpha_recorder::obs::gpu_texture_recording_output_stats(gpu_texture_output_);
            const char *last_error = obs_output_get_last_error(gpu_texture_output_);
            const std::string last_error_text = last_error != nullptr ? last_error : "";
            obs_output_release(gpu_texture_output_);
            gpu_texture_output_ = nullptr;
            if (!finalize_failed && !gpu_stats.finalized)
            {
                finalize_failed = true;
                finalize_failure_reason = !last_error_text.empty()
                                              ? last_error_text
                                              : "Alpha Recorder alpha movie muxer did not reach its finalized state.";
                if (error_message != nullptr && error_message->empty())
                {
                    *error_message = finalize_failure_reason;
                }
            }
            if (!finalize_failed && gpu_stats.finalized && rename_temporary_output &&
                !move_completed_gpu_texture_output(actual_writer_path, final_writer_path, error_message))
            {
                finalize_failed = true;
                finalize_failure_reason =
                    error_message != nullptr && !error_message->empty()
                        ? *error_message
                        : "Alpha Recorder failed to publish the GPU texture alpha movie.";
            }
            if (finalize_failed)
            {
                remove_failed_gpu_texture_output(actual_writer_path,
                                                 final_writer_path,
                                                 finalize_failure_reason);
            }
            gpu_texture_output_path_.clear();
            gpu_texture_final_output_path_.clear();
            gpu_texture_output_is_temporary_ = false;

            log_gpu_texture_performance_summary_locked(final_writer_path, gpu_stats, visible_range);
            if (finalize_failed || !gpu_stats.finalized)
            {
                if (error_message != nullptr)
                {
                    if (error_message->empty())
                    {
                        *error_message = !last_error_text.empty()
                                             ? last_error_text
                                             : "Alpha Recorder failed to finalize the GPU texture alpha movie.";
                    }
                }
                return false;
            }

            return true;
        }

        void clear_pending_alignment_locked() noexcept
        {
            alignment_engine_.clear_pending();
        }

        void log_performance_summary_locked(const std::filesystem::path &mask_path,
                                            const AlphaOutputSinkStats &writer_stats)
        {
            const std::string message = performance_summary_text_locked(mask_path, writer_stats);
            blog(LOG_INFO, "%s", message.c_str());
            if (settings_.diagnostic_logging)
            {
                alpha_recorder::obs::append_diagnostic_log_line(message);
            }
        }

        void log_gpu_texture_performance_summary_locked(
            const std::filesystem::path &mask_path,
            const alpha_recorder::obs::GpuTextureRecordingOutputStats &gpu_stats,
            const alpha_recorder::obs::AlphaVisiblePacketRange &visible_range)
        {
            char buffer[2048];
            (void)std::snprintf(
                buffer, sizeof(buffer),
                "Alpha Recorder GPU texture telemetry: path=\"%s\" packets=%llu keyframes=%llu packet_bytes=%s muxed_packets=%llu max_buffered_packets=%llu max_buffered_bytes=%s finalized=%s first_pts=%lld last_pts=%lld visible_range={media_time=%lld duration=%lld} main_packets={count=%llu first_cts=%llu last_cts=%llu}",
                mask_path.generic_u8string().c_str(),
                static_cast<unsigned long long>(gpu_stats.packet_count),
                static_cast<unsigned long long>(gpu_stats.keyframe_count),
                format_bytes(gpu_stats.packet_bytes).c_str(),
                static_cast<unsigned long long>(gpu_stats.muxed_packet_count),
                static_cast<unsigned long long>(gpu_stats.max_buffered_packet_count),
                format_bytes(gpu_stats.max_buffered_packet_bytes).c_str(),
                gpu_stats.finalized ? "true" : "false",
                static_cast<long long>(gpu_stats.first_pts),
                static_cast<long long>(gpu_stats.last_pts),
                static_cast<long long>(visible_range.media_time),
                static_cast<long long>(visible_range.duration),
                static_cast<unsigned long long>(gpu_main_packet_count_),
                static_cast<unsigned long long>(gpu_main_first_packet_cts_),
                static_cast<unsigned long long>(gpu_main_last_packet_cts_));
            blog(LOG_INFO, "%s", buffer);
            if (settings_.diagnostic_logging)
            {
                alpha_recorder::obs::append_diagnostic_log_line(buffer);
            }
        }

        void log_segment_start_locked(const std::filesystem::path &mask_path,
                                      const alpha_recorder::obs::AlphaOutputSinkConfig &config)
        {
            if (!settings_.diagnostic_logging)
            {
                return;
            }

            char buffer[1536];
            const std::size_t queue_frame_limit =
                alpha_recorder::obs::alpha_mask_writer_queue_frame_limit(config.fps_num, config.fps_den);
            const std::size_t queue_byte_limit =
                alpha_recorder::obs::alpha_mask_writer_queue_byte_limit(config.width, config.height, config.fps_num,
                                                                        config.fps_den);
            (void)std::snprintf(
                buffer, sizeof(buffer),
                "Alpha Recorder segment start: path=\"%s\" format=%s video={width=%u height=%u fps=%u/%u} alignment_queue={alpha_limit_frames=%zu output_limit_frames=%zu encoded_reorder_frames=%zu plausible_delta_ns=%llu} writer_queue={limit_frames=%zu limit_bytes=%s} hevc={profile=%s cq=%u preset=%s nvenc_tune=%s gop=%u aq=%s nvenc_split=%s nvenc_gpu=%d}",
                mask_path.generic_u8string().c_str(),
                std::string{alpha_recorder::obs::finalization_format_config_value(config.finalization_format)}.c_str(),
                config.width, config.height, config.fps_num, config.fps_den,
                max_pending_alpha_frames_, max_pending_output_frames_, kMaxEncoderReorderFrames,
                static_cast<unsigned long long>(plausible_alignment_delta_ns(config.fps_num, config.fps_den)),
                queue_frame_limit,
                format_bytes(static_cast<std::uint64_t>(queue_byte_limit)).c_str(),
                std::string{alpha_recorder::obs::hevc_quality_profile_config_value(config.hevc_encoder.quality_profile)}.c_str(),
                config.hevc_encoder.quality_cq,
                std::string{alpha_recorder::obs::hevc_encoder_preset_config_value(config.hevc_encoder.preset)}.c_str(),
                std::string{alpha_recorder::obs::hevc_nvenc_tune_config_value(config.hevc_encoder.nvenc_tune)}.c_str(),
                config.hevc_encoder.gop_size,
                bool_text(config.hevc_encoder.adaptive_quantization),
                std::string{alpha_recorder::obs::hevc_nvenc_split_encode_config_value(config.hevc_encoder.nvenc_split_encode)}.c_str(),
                static_cast<int>(config.hevc_encoder.nvenc_gpu_index));
            alpha_recorder::obs::append_diagnostic_log_line(buffer);
        }

        std::string performance_summary_text_locked(const std::filesystem::path &mask_path,
                                                    const AlphaOutputSinkStats &writer_stats)
        {
            const std::string capture_total = format_timing_summary(live_telemetry_.capture_total);
            const std::string capture_map = format_timing_summary(live_telemetry_.capture_map);
            const std::string capture_render = format_timing_summary(live_telemetry_.capture_render);
            const std::string capture_stage = format_timing_summary(live_telemetry_.capture_stage);
            const std::string alignment_batch = format_timing_summary(live_telemetry_.alignment_batch);
            const std::string output_cts_delta =
                format_signed_delta_summary(live_telemetry_.alignment_output_cts_delta);
            const std::string alpha_content_delta =
                format_signed_delta_summary(live_telemetry_.alignment_alpha_content_delta);
            const std::string packet_fer_cts_delta =
                format_signed_delta_summary(live_telemetry_.packet_fer_cts_delta);
            const std::string packet_cts_delta =
                format_signed_delta_summary(live_telemetry_.packet_cts_delta);
            const std::string packet_fer_delta =
                format_signed_delta_summary(live_telemetry_.packet_fer_delta);
            const std::string writer_max_bytes = format_bytes(static_cast<std::uint64_t>(writer_stats.max_queued_bytes));
            const std::string writer_limit_bytes = format_bytes(static_cast<std::uint64_t>(writer_stats.queue_byte_limit));
            const std::string writer_queued_bytes = format_bytes(writer_stats.queued_bytes_total);
            const auto writer_frame_avg_ms = [encoded_frames = writer_stats.encoded_frames](std::uint64_t ns_total) {
                return encoded_frames == 0U ? 0.0 : ns_to_ms(ns_total / encoded_frames);
            };
            const auto writer_packet_avg_ms = [packets = writer_stats.emitted_packets](std::uint64_t ns_total) {
                return packets == 0U ? 0.0 : ns_to_ms(ns_total / packets);
            };

            char buffer[12288];
            (void)std::snprintf(
                buffer, sizeof(buffer),
                "Alpha Recorder performance telemetry: path=\"%s\" capture_total={%s callbacks=%llu captured=%llu} readback={%s} gpu_submit={render={%s} stage={%s}} alignment_worker={%s frames=%llu raw=%llu packets=%llu} timestamp_spans={packet_cts_ms=%.3f raw_output_ms=%.3f alpha_capture_ms=%.3f first_packet_cts=%llu last_packet_cts=%llu first_raw=%llu last_raw=%llu first_alpha=%llu last_alpha=%llu} alignment_delta={output_minus_packet_cts={%s} alpha_minus_output_content={%s}} packet_timing={fer_minus_cts={%s} cts_delta={%s} fer_delta={%s} texture_stall_corrections=%llu} queues={alpha_max=%zu alpha_limit=%zu output_max=%zu output_limit=%zu encoded_max=%zu writer_max_frames=%zu writer_max_bytes=%s writer_limit_frames=%zu writer_limit_bytes=%s writer_overflow_repeats=%llu} alignment_recovery={repeated=%llu missing_output=%llu missing_alpha=%llu texture_stall=%llu black=%llu alpha_dropped=%llu output_dropped=%llu} writer={enqueue={count=%llu avg_ms=%.3f max_ms=%.3f} encode={count=%llu avg_ms=%.3f max_ms=%.3f} finalize_ms=%.3f queued=%s} encode_breakdown={make_writable_avg_ms=%.3f make_writable_max_ms=%.3f copy_avg_ms=%.3f copy_max_ms=%.3f send_avg_ms=%.3f send_max_ms=%.3f receive_avg_ms=%.3f receive_max_ms=%.3f packet_write_avg_ms=%.3f packet_write_max_ms=%.3f emitted_packets=%llu} nvenc_options={split_available=%s split_value=%lld gpu_available=%s gpu_value=%lld}",
                mask_path.generic_u8string().c_str(), capture_total.c_str(),
                static_cast<unsigned long long>(live_telemetry_.rendered_callbacks),
                static_cast<unsigned long long>(live_telemetry_.captured_frames), capture_map.c_str(),
                capture_render.c_str(), capture_stage.c_str(), alignment_batch.c_str(),
                static_cast<unsigned long long>(live_telemetry_.aligned_frames),
                static_cast<unsigned long long>(live_telemetry_.raw_video_frames),
                static_cast<unsigned long long>(live_telemetry_.packet_frames),
                timestamp_span_ms(live_telemetry_.first_packet_cts, live_telemetry_.last_packet_cts),
                timestamp_span_ms(live_telemetry_.first_raw_output_timestamp,
                                  live_telemetry_.last_raw_output_timestamp),
                timestamp_span_ms(live_telemetry_.first_alpha_timestamp, live_telemetry_.last_alpha_timestamp),
                static_cast<unsigned long long>(live_telemetry_.first_packet_cts),
                static_cast<unsigned long long>(live_telemetry_.last_packet_cts),
                static_cast<unsigned long long>(live_telemetry_.first_raw_output_timestamp),
                static_cast<unsigned long long>(live_telemetry_.last_raw_output_timestamp),
                static_cast<unsigned long long>(live_telemetry_.first_alpha_timestamp),
                static_cast<unsigned long long>(live_telemetry_.last_alpha_timestamp),
                output_cts_delta.c_str(), alpha_content_delta.c_str(),
                packet_fer_cts_delta.c_str(), packet_cts_delta.c_str(), packet_fer_delta.c_str(),
                static_cast<unsigned long long>(live_telemetry_.texture_stall_corrections),
                live_telemetry_.max_pending_alpha_frames, max_pending_alpha_frames_,
                live_telemetry_.max_pending_output_frames, max_pending_output_frames_,
                live_telemetry_.max_pending_encoded_frames, writer_stats.max_queued_frames,
                writer_max_bytes.c_str(), writer_stats.queue_frame_limit, writer_limit_bytes.c_str(),
                static_cast<unsigned long long>(writer_stats.overflow_repeated_frames),
                static_cast<unsigned long long>(live_telemetry_.alignment_repeated_frames),
                static_cast<unsigned long long>(live_telemetry_.alignment_missing_output_repeats),
                static_cast<unsigned long long>(live_telemetry_.alignment_missing_alpha_repeats),
                static_cast<unsigned long long>(live_telemetry_.alignment_texture_stall_repeats),
                static_cast<unsigned long long>(live_telemetry_.alignment_black_repeats),
                static_cast<unsigned long long>(live_telemetry_.alignment_alpha_dropped_frames),
                static_cast<unsigned long long>(live_telemetry_.alignment_output_dropped_frames),
                static_cast<unsigned long long>(writer_stats.enqueued_frames),
                writer_stats.enqueued_frames == 0U ? 0.0 : ns_to_ms(writer_stats.enqueue_time_ns_total / writer_stats.enqueued_frames),
                ns_to_ms(writer_stats.enqueue_time_ns_max),
                static_cast<unsigned long long>(writer_stats.encoded_frames),
                writer_stats.encoded_frames == 0U ? 0.0 : ns_to_ms(writer_stats.encode_time_ns_total / writer_stats.encoded_frames),
                ns_to_ms(writer_stats.encode_time_ns_max), ns_to_ms(writer_stats.finalize_time_ns),
                writer_queued_bytes.c_str(),
                writer_frame_avg_ms(writer_stats.encode_make_writable_time_ns_total),
                ns_to_ms(writer_stats.encode_make_writable_time_ns_max),
                writer_frame_avg_ms(writer_stats.encode_copy_time_ns_total),
                ns_to_ms(writer_stats.encode_copy_time_ns_max),
                writer_frame_avg_ms(writer_stats.encode_send_time_ns_total),
                ns_to_ms(writer_stats.encode_send_time_ns_max),
                writer_frame_avg_ms(writer_stats.encode_receive_time_ns_total),
                ns_to_ms(writer_stats.encode_receive_time_ns_max),
                writer_packet_avg_ms(writer_stats.encode_packet_write_time_ns_total),
                ns_to_ms(writer_stats.encode_packet_write_time_ns_max),
                static_cast<unsigned long long>(writer_stats.emitted_packets),
                bool_text(writer_stats.nvenc_split_encode_option_available),
                static_cast<long long>(writer_stats.nvenc_split_encode_option_value),
                bool_text(writer_stats.nvenc_gpu_option_available),
                static_cast<long long>(writer_stats.nvenc_gpu_option_value));
            return std::string{buffer};
        }

        [[nodiscard]] bool alignment_worker_has_work_locked() const noexcept
        {
            return !gpu_texture_path_ && alpha_sink_.is_open() && alignment_engine_.has_work();
        }

        void append_alignment_trace_locked(const AlignmentTraceEvent &event) const
        {
            if (!settings_.diagnostic_logging || live_telemetry_.aligned_frames >= kMaxDiagnosticAlignmentTraceFrames)
            {
                return;
            }

            char buffer[1536];
            (void)std::snprintf(
                buffer, sizeof(buffer),
                "Alpha Recorder alignment trace: seq=%llu reason=%s packet={pts=%lld cts=%llu fer=%llu ferc=%llu texture=%s} output={index=%zu timestamp=%llu content=%llu duplicate=%s delta_ns=%llu} alpha={index=%zu index_valid=%s timestamp=%llu delta_ns=%llu repeated=%s} queues={alpha=%zu output=%zu encoded=%zu duplicate_run=%u}",
                static_cast<unsigned long long>(live_telemetry_.aligned_frames),
                event.reason,
                static_cast<long long>(event.encoded_frame.pts),
                static_cast<unsigned long long>(event.encoded_frame.cts),
                static_cast<unsigned long long>(event.encoded_frame.fer),
                static_cast<unsigned long long>(event.encoded_frame.ferc),
                bool_text(event.encoded_frame.texture_encoded),
                event.selection.output_index,
                static_cast<unsigned long long>(event.output_frame.timestamp),
                static_cast<unsigned long long>(event.output_frame.content_timestamp),
                bool_text(event.output_frame.duplicate_previous),
                static_cast<unsigned long long>(event.selection.output_delta),
                event.selection.alpha_index,
                bool_text(event.selection.alpha_index_valid),
                static_cast<unsigned long long>(event.alpha_frame.timestamp),
                static_cast<unsigned long long>(event.selection.alpha_delta),
                bool_text(event.selection.repeated),
                event.alpha_queue_size,
                event.output_queue_size,
                event.encoded_queue_size,
                event.consecutive_output_duplicate_frames);
            alpha_recorder::obs::append_diagnostic_log_line(buffer);
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
            (void)error_message;
            alignment_engine_.remember_alpha_frame(std::move(frame), live_telemetry_);
            notify_alignment_worker_locked();
            return true;
        }

        void reset_gpu_texture_timing_locked() noexcept
        {
            gpu_main_packet_count_ = 0U;
            gpu_main_first_packet_cts_ = 0U;
            gpu_main_last_packet_cts_ = 0U;
            gpu_main_first_packet_pts_ = 0;
            gpu_main_last_packet_pts_ = 0;
            gpu_main_packets_.clear();
            gpu_texture_output_start_video_time_ = 0U;
            gpu_recording_started_video_time_ = 0U;
            gpu_texture_delayed_replay_started_ = false;
        }

        void remember_gpu_texture_main_packet_locked(const encoder_packet &packet,
                                                     const encoder_packet_time &packet_time) noexcept
        {
            if (gpu_main_packet_count_ == 0U)
            {
                gpu_main_first_packet_cts_ = packet_time.cts;
                gpu_main_first_packet_pts_ = packet.pts;
            }
            gpu_main_last_packet_cts_ = packet_time.cts;
            gpu_main_last_packet_pts_ = packet.pts;
            ++gpu_main_packet_count_;
            gpu_main_packets_.push_back(alpha_recorder::obs::GpuTexturePacketRecord{packet.pts,
                                                                                    packet.dts,
                                                                                    packet.timebase_num,
                                                                                    packet.timebase_den,
                                                                                    packet.keyframe,
                                                                                    packet.sys_dts_usec,
                                                                                    packet_time.cts,
                                                                                    true,
                                                                                    0U,
                                                                                    0U,
                                                                                    false,
                                                                                    false});
        }

        bool write_alpha_frame_locked(const alpha_recorder::obs::AlphaFrame &frame,
                                      bool &queued,
                                      std::string &error_message)
        {
            alpha_recorder::obs::AlphaOutputFrameDisposition disposition =
                alpha_recorder::obs::AlphaOutputFrameDisposition::Queued;
            if (frame.empty() || !alpha_sink_.write_frame(frame.alpha, &error_message, &disposition))
            {
                session_aborted_ = true;
                if (error_message.empty())
                {
                    error_message = "Alpha Recorder failed to write an alpha mask frame.";
                }
                return false;
            }

            queued = disposition == alpha_recorder::obs::AlphaOutputFrameDisposition::Queued;
            ++next_sequence_;
            return true;
        }

        void drain_encoded_alpha_locked(bool drain_all, std::size_t max_frames = static_cast<std::size_t>(-1))
        {
            if (!alpha_sink_.is_open())
            {
                return;
            }

            const AlphaAlignmentDrainResult result = alignment_engine_.drain(
                drain_all,
                max_frames,
                live_telemetry_,
                [this](const alpha_recorder::obs::AlphaFrame &frame, bool &queued, std::string &error_message) {
                    return write_alpha_frame_locked(frame, queued, error_message);
                },
                [this](const AlignmentTraceEvent &event) {
                    append_alignment_trace_locked(event);
                });
            if (result.failed)
            {
                session_aborted_ = true;
                alignment_engine_.clear_pending();
                log_and_show_error(result.error_message.empty()
                                       ? "Alpha Recorder failed to align an alpha mask frame."
                                       : result.error_message,
                                   false);
            }
        }

        void queue_alpha_for_packet_locked(std::int64_t pts,
                                           std::uint64_t cts,
                                           std::uint64_t fer,
                                           std::uint64_t ferc,
                                           bool texture_encoded)
        {
            alignment_engine_.queue_packet(pts, cts, fer, ferc, texture_encoded, live_telemetry_);
            notify_alignment_worker_locked();
        }

        bool remember_output_frame_locked(alpha_recorder::obs::OutputFrameCadence frame, std::string &error_message)
        {
            (void)error_message;
            alignment_engine_.remember_output_frame(frame, live_telemetry_);
            notify_alignment_worker_locked();
            return true;
        }

        void reconcile_output_frame_count_locked(std::string &error_message)
        {
            if (!alpha_sink_.is_open())
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
                if (!session_active_ && !alpha_sink_.is_open() && gpu_texture_output_ == nullptr && recording_output_ == nullptr &&
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
                if (!gpu_texture_path_)
                {
                    capture_final_alpha_frame_locked();
                }
                reconcile_output_frame_count_locked(finalize_error);
                if (!finalize_error.empty())
                {
                    finalize_failed = true;
                }
                if (!finalize_current_segment_locked(&finalize_error))
                {
                    finalize_failed = true;
                }

                alignment_engine_.reset_all();
                raw_video_cadence_.reset();
                reset_gpu_texture_timing_locked();
                recording_texture_encoded_ = false;
                if (!gpu_texture_path_)
                {
                    obs_enter_graphics();
                    alpha_extractor_.destroy();
                    obs_leave_graphics();
                }
                gpu_texture_path_ = false;
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

                if (gpu_texture_path_ && gpu_texture_output_ != nullptr &&
                    !(gpu_texture_output_is_temporary_ && gpu_texture_final_output_path_.empty()))
                {
                    if (!finalize_current_segment_locked(&error_message))
                    {
                        if (error_message.empty())
                        {
                            error_message = "Alpha Recorder failed to finalize the current split GPU texture alpha movie.";
                        }
                    }
                }
                else if (alpha_sink_.is_open())
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
                    alignment_engine_.reset_all();
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
                    gpu_texture_path_ || video_info_.output_width == 0U || video_info_.output_height == 0U)
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
                    !gpu_texture_path_ && video_info_.output_width == output_width && video_info_.output_height == output_height)
                {
                    (void)remember_pending_alpha_frame_locked(
                        alpha_recorder::obs::AlphaFrame{alpha_timestamp,
                                                        std::make_shared<std::vector<std::uint8_t>>(std::move(alpha))},
                        error_message);
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
                gpu_texture_path_ || video_info_.output_width == 0U || video_info_.output_height == 0U)
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
            obs_output_t *delayed_replay_output = nullptr;
            std::int64_t delayed_replay_main_pts = 0;
            std::uint64_t delayed_replay_main_cts = 0U;
            bool delayed_replay_main_texture_encoded = false;
            obs_output_t *cts_replay_output = nullptr;
            std::int64_t cts_replay_main_pts = 0;
            std::uint64_t cts_replay_main_cts = 0U;
            bool cts_replay_main_texture_encoded = false;
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
                else if (gpu_texture_path_)
                {
                    remember_gpu_texture_main_packet_locked(*packet, *packet_time);
                    if (!gpu_texture_delayed_replay_started_ && gpu_texture_output_ != nullptr &&
                        gpu_main_first_packet_cts_ != 0U)
                    {
                        delayed_replay_output = obs_output_get_ref(gpu_texture_output_);
                        delayed_replay_main_pts = packet->pts;
                        delayed_replay_main_cts = packet_time->cts;
                        delayed_replay_main_texture_encoded = recording_texture_encoded_;
                    }
                    else if (gpu_texture_delayed_replay_started_ && gpu_texture_output_ != nullptr)
                    {
                        cts_replay_output = obs_output_get_ref(gpu_texture_output_);
                        cts_replay_main_pts = packet->pts;
                        cts_replay_main_cts = packet_time->cts;
                        cts_replay_main_texture_encoded = recording_texture_encoded_;
                    }
                }
                else
                {
                    queue_alpha_for_packet_locked(packet->pts, packet_time->cts, packet_time->fer,
                                                  packet_time->ferc, recording_texture_encoded_);
                }
            }

            if (!error_message.empty())
            {
                log_and_show_error(error_message, false);
            }
            if (delayed_replay_output != nullptr)
            {
                std::string replay_error;
                const bool replay_started =
                    alpha_recorder::obs::gpu_texture_recording_output_begin_delayed_replay(
                        delayed_replay_output,
                        delayed_replay_main_pts,
                        delayed_replay_main_cts,
                        delayed_replay_main_texture_encoded,
                        &replay_error);
                obs_output_release(delayed_replay_output);
                if (replay_started)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    gpu_texture_delayed_replay_started_ = true;
                }
                else if (!replay_error.empty() && settings_.diagnostic_logging)
                {
                    alpha_recorder::obs::append_diagnostic_log_line(
                        "Alpha Recorder GPU delayed replay not started yet: reason=\"" +
                        replay_error + "\" main_cts=\"" + std::to_string(delayed_replay_main_cts) + "\"");
                }
            }
            if (cts_replay_output != nullptr)
            {
                std::string replay_error;
                (void)alpha_recorder::obs::gpu_texture_recording_output_queue_main_packet_replay(
                    cts_replay_output,
                    cts_replay_main_pts,
                    cts_replay_main_cts,
                    cts_replay_main_texture_encoded,
                    &replay_error);
                obs_output_release(cts_replay_output);
                if (!replay_error.empty() && settings_.diagnostic_logging)
                {
                    alpha_recorder::obs::append_diagnostic_log_line(
                        "Alpha Recorder GPU CTS replay packet was not queued: reason=\"" +
                        replay_error + "\" main_cts=\"" + std::to_string(cts_replay_main_cts) + "\"");
                }
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
        bool gpu_texture_path_ = false;
        std::uint64_t next_sequence_ = 0;
        std::uint64_t gpu_main_packet_count_ = 0U;
        std::uint64_t gpu_main_first_packet_cts_ = 0U;
        std::uint64_t gpu_main_last_packet_cts_ = 0U;
        std::int64_t gpu_main_first_packet_pts_ = 0;
        std::int64_t gpu_main_last_packet_pts_ = 0;
        std::vector<alpha_recorder::obs::GpuTexturePacketRecord> gpu_main_packets_{};
        std::uint64_t gpu_texture_output_start_video_time_ = 0U;
        std::uint64_t gpu_recording_started_video_time_ = 0U;
        bool gpu_texture_delayed_replay_started_ = false;
        AlphaAlignmentEngine alignment_engine_{};
        alpha_recorder::obs::RawVideoCadenceTracker raw_video_cadence_{};
        LivePipelineTelemetry live_telemetry_{};
        std::filesystem::path recording_path_{};
        std::filesystem::path gpu_texture_output_path_{};
        std::filesystem::path gpu_texture_final_output_path_{};
        obs_video_info video_info_{};
        AlphaPlaneExtractor alpha_extractor_{};
        alpha_recorder::obs::CpuAlphaOutputSink alpha_sink_{};
        obs_output_t *gpu_texture_output_ = nullptr;
        obs_output_t *recording_output_ = nullptr;
        alpha_recorder::obs::FinalizationFormat finalization_format_ = alpha_recorder::obs::FinalizationFormat::MaskPngMov;
        alpha_recorder::obs::Settings settings_{};
        static constexpr std::size_t kMaxEncoderReorderFrames = 16U;
        static constexpr std::uint64_t kMaxDiagnosticAlignmentTraceFrames = 720U;
        std::size_t max_pending_alpha_frames_ = 240U;
        std::size_t max_pending_output_frames_ = 240U;
        std::condition_variable alignment_condition_{};
        std::thread alignment_worker_{};
        bool alignment_worker_stop_ = false;
        bool gpu_texture_output_is_temporary_ = false;
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
