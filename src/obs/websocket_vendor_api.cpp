#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"
#include "gpu_texture_recording_output.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-websocket-api.h>

#include <util/config-file.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace alpha_recorder::obs
{
    namespace
    {
        constexpr const char *kVendorName = "alpha_recorder";
        constexpr const char *kRequestGetSettings = "GetSettings";
        constexpr const char *kRequestSetSettings = "SetSettings";

        obs_websocket_vendor g_vendor = nullptr;

        void write_error(obs_data_t *response, std::string_view message)
        {
            if (response != nullptr)
            {
                obs_data_set_bool(response, "ok", false);
                obs_data_set_string(response, "error", std::string{message}.c_str());
            }
        }

        bool finalization_format_runtime_available_for_api(FinalizationFormat format,
                                                           std::string *reason)
        {
            if (finalization_format_uses_gpu_texture_path(format))
            {
                return gpu_texture_hevc_encoder_runtime_available(format, reason);
            }

            return finalization_format_runtime_available(format, reason);
        }

        void write_settings(obs_data_t *response, const Settings &settings)
        {
            obs_data_set_bool(response, "ok", true);
            obs_data_set_bool(response, settings_enabled_key().data(), settings.enabled);
            obs_data_set_string(response, settings_finalization_format_key().data(),
                                finalization_format_config_value(settings.finalization_format).data());
            obs_data_set_string(response, "finalization_format_display_name",
                                finalization_format_display_name(settings.finalization_format).data());
            obs_data_set_string(response, settings_hevc_quality_profile_key().data(),
                                hevc_quality_profile_config_value(settings.hevc_encoder.quality_profile).data());
            obs_data_set_int(response, settings_hevc_quality_cq_key().data(), settings.hevc_encoder.quality_cq);
            obs_data_set_string(response, settings_hevc_preset_key().data(),
                                hevc_encoder_preset_config_value(settings.hevc_encoder.preset).data());
            obs_data_set_string(response, settings_hevc_nvenc_tune_key().data(),
                                hevc_nvenc_tune_config_value(settings.hevc_encoder.nvenc_tune).data());
            obs_data_set_int(response, settings_hevc_gop_size_key().data(), settings.hevc_encoder.gop_size);
            obs_data_set_int(response, settings_hevc_b_frames_key().data(), settings.hevc_encoder.b_frames);
            obs_data_set_bool(response, settings_hevc_adaptive_quantization_key().data(),
                              settings.hevc_encoder.adaptive_quantization);
            obs_data_set_string(response, settings_hevc_nvenc_split_encode_key().data(),
                                hevc_nvenc_split_encode_config_value(settings.hevc_encoder.nvenc_split_encode).data());
            obs_data_set_int(response, settings_hevc_nvenc_gpu_index_key().data(), settings.hevc_encoder.nvenc_gpu_index);
            obs_data_set_bool(response, settings_diagnostic_logging_key().data(), settings.diagnostic_logging);
            obs_data_t *formats = obs_data_create();
            for (const FinalizationFormatOption &option : finalization_format_options)
            {
                std::string reason;
                const bool available = finalization_format_runtime_available_for_api(option.value, &reason);
                obs_data_t *format = obs_data_create();
                obs_data_set_string(format, "display_name", std::string{option.display_name}.c_str());
                obs_data_set_bool(format, "available", available);
                if (!available)
                {
                    obs_data_set_string(format, "unavailable_reason", reason.c_str());
                }
                obs_data_set_obj(formats, std::string{option.config_value}.c_str(), format);
                obs_data_release(format);
            }
            obs_data_set_obj(response, "finalization_formats", formats);
            obs_data_release(formats);
        }

        void get_settings_request(obs_data_t *, obs_data_t *response, void *)
        {
            config_t *config = obs_frontend_get_user_config();
            if (config == nullptr)
            {
                write_error(response, "OBS user configuration is unavailable.");
                return;
            }

            write_settings(response, load_settings(config));
        }

        void set_settings_request(obs_data_t *request, obs_data_t *response, void *)
        {
            config_t *config = obs_frontend_get_user_config();
            if (config == nullptr)
            {
                write_error(response, "OBS user configuration is unavailable.");
                return;
            }

            Settings settings = load_settings(config);
            if (request != nullptr && obs_data_has_user_value(request, settings_enabled_key().data()))
            {
                settings.enabled = obs_data_get_bool(request, settings_enabled_key().data());
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_finalization_format_key().data()))
            {
                const char *format_text = obs_data_get_string(request, settings_finalization_format_key().data());
                FinalizationFormat parsed_format = settings.finalization_format;
                if (format_text == nullptr || !try_parse_finalization_format(format_text, parsed_format))
                {
                    write_error(response, "Unsupported finalization_format.");
                    return;
                }

                std::string unavailable_reason;
                if (!finalization_format_runtime_available_for_api(parsed_format, &unavailable_reason))
                {
                    write_error(response, std::string{"Unsupported finalization_format: "} + unavailable_reason);
                    return;
                }

                settings.finalization_format = normalize_finalization_format(parsed_format);
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_quality_profile_key().data()))
            {
                const char *profile_text = obs_data_get_string(request, settings_hevc_quality_profile_key().data());
                HevcQualityProfile parsed_profile = settings.hevc_encoder.quality_profile;
                if (profile_text == nullptr || !try_parse_hevc_quality_profile(profile_text, parsed_profile))
                {
                    write_error(response, "Unsupported hevc_quality_profile.");
                    return;
                }

                settings.hevc_encoder.quality_profile = parsed_profile;
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_quality_cq_key().data()))
            {
                const long long requested_cq = obs_data_get_int(request, settings_hevc_quality_cq_key().data());
                settings.hevc_encoder.quality_cq = clamp_hevc_quality_cq(static_cast<std::uint32_t>(requested_cq < 0 ? 0 : requested_cq));
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_preset_key().data()))
            {
                const char *preset_text = obs_data_get_string(request, settings_hevc_preset_key().data());
                HevcEncoderPreset parsed_preset = settings.hevc_encoder.preset;
                if (preset_text == nullptr || !try_parse_hevc_encoder_preset(preset_text, parsed_preset))
                {
                    write_error(response, "Unsupported hevc_preset.");
                    return;
                }

                settings.hevc_encoder.preset = parsed_preset;
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_nvenc_tune_key().data()))
            {
                const char *tune_text = obs_data_get_string(request, settings_hevc_nvenc_tune_key().data());
                HevcNvencTune parsed_tune = settings.hevc_encoder.nvenc_tune;
                if (tune_text == nullptr || !try_parse_hevc_nvenc_tune(tune_text, parsed_tune))
                {
                    write_error(response, "Unsupported hevc_nvenc_tune.");
                    return;
                }

                settings.hevc_encoder.nvenc_tune = parsed_tune;
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_gop_size_key().data()))
            {
                const long long requested_gop = obs_data_get_int(request, settings_hevc_gop_size_key().data());
                settings.hevc_encoder.gop_size =
                    clamp_hevc_gop_size(static_cast<std::uint32_t>(requested_gop < 0 ? 0 : requested_gop));
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_b_frames_key().data()))
            {
                const long long requested_b_frames = obs_data_get_int(request, settings_hevc_b_frames_key().data());
                settings.hevc_encoder.b_frames =
                    clamp_hevc_b_frames(static_cast<std::uint32_t>(requested_b_frames < 0 ? 0 : requested_b_frames));
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_adaptive_quantization_key().data()))
            {
                settings.hevc_encoder.adaptive_quantization =
                    obs_data_get_bool(request, settings_hevc_adaptive_quantization_key().data());
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_nvenc_split_encode_key().data()))
            {
                const char *split_encode_text = obs_data_get_string(request, settings_hevc_nvenc_split_encode_key().data());
                HevcNvencSplitEncodeMode parsed_split_encode = settings.hevc_encoder.nvenc_split_encode;
                if (split_encode_text == nullptr ||
                    !try_parse_hevc_nvenc_split_encode(split_encode_text, parsed_split_encode))
                {
                    write_error(response, "Unsupported hevc_nvenc_split_encode.");
                    return;
                }

                settings.hevc_encoder.nvenc_split_encode = parsed_split_encode;
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_hevc_nvenc_gpu_index_key().data()))
            {
                const long long requested_gpu_index = obs_data_get_int(request, settings_hevc_nvenc_gpu_index_key().data());
                std::int32_t normalized_gpu_index = -1;
                if (!try_normalize_hevc_nvenc_gpu_index(requested_gpu_index, normalized_gpu_index))
                {
                    write_error(response, "hevc_nvenc_gpu_index must be -1 or a non-negative 32-bit integer.");
                    return;
                }
                settings.hevc_encoder.nvenc_gpu_index = normalized_gpu_index;
            }

            if (request != nullptr && obs_data_has_user_value(request, settings_diagnostic_logging_key().data()))
            {
                settings.diagnostic_logging = obs_data_get_bool(request, settings_diagnostic_logging_key().data());
            }

            config_set_bool(config, settings_section().data(), settings_enabled_key().data(), settings.enabled);
            config_set_string(config, settings_section().data(), settings_finalization_format_key().data(),
                              finalization_format_config_value(settings.finalization_format).data());
            config_set_string(config, settings_section().data(), settings_hevc_quality_profile_key().data(),
                              hevc_quality_profile_config_value(settings.hevc_encoder.quality_profile).data());
            config_set_int(config, settings_section().data(), settings_hevc_quality_cq_key().data(), settings.hevc_encoder.quality_cq);
            config_set_string(config, settings_section().data(), settings_hevc_preset_key().data(),
                              hevc_encoder_preset_config_value(settings.hevc_encoder.preset).data());
            config_set_string(config, settings_section().data(), settings_hevc_nvenc_tune_key().data(),
                              hevc_nvenc_tune_config_value(settings.hevc_encoder.nvenc_tune).data());
            config_set_int(config, settings_section().data(), settings_hevc_gop_size_key().data(), settings.hevc_encoder.gop_size);
            config_set_int(config, settings_section().data(), settings_hevc_b_frames_key().data(), settings.hevc_encoder.b_frames);
            config_set_bool(config, settings_section().data(), settings_hevc_adaptive_quantization_key().data(),
                            settings.hevc_encoder.adaptive_quantization);
            config_set_string(config, settings_section().data(), settings_hevc_nvenc_split_encode_key().data(),
                              hevc_nvenc_split_encode_config_value(settings.hevc_encoder.nvenc_split_encode).data());
            config_set_int(config, settings_section().data(), settings_hevc_nvenc_gpu_index_key().data(),
                           settings.hevc_encoder.nvenc_gpu_index);
            config_set_bool(config, settings_section().data(), settings_diagnostic_logging_key().data(),
                            settings.diagnostic_logging);

            if (config_save(config) != CONFIG_SUCCESS)
            {
                write_error(response, "Failed to save OBS user configuration.");
                return;
            }

            const bool hooks_synced = register_runtime_hooks();
            write_settings(response, settings);
            obs_data_set_bool(response, "runtime_hooks_synced", hooks_synced);
        }
    } // namespace

    void register_websocket_vendor_api() noexcept
    {
        if (g_vendor != nullptr)
        {
            return;
        }

        g_vendor = obs_websocket_register_vendor(kVendorName);
        if (g_vendor == nullptr)
        {
            blog(LOG_WARNING, "Alpha Recorder could not register obs-websocket vendor API; obs-websocket may be unavailable.");
            return;
        }

        if (!obs_websocket_vendor_register_request(g_vendor, kRequestGetSettings, get_settings_request, nullptr) ||
            !obs_websocket_vendor_register_request(g_vendor, kRequestSetSettings, set_settings_request, nullptr))
        {
            blog(LOG_WARNING, "Alpha Recorder could not register all obs-websocket vendor requests.");
        }
    }

    void unregister_websocket_vendor_api() noexcept
    {
        if (g_vendor == nullptr)
        {
            return;
        }

        // obs-websocket exposes unregister through a cached proc-handler pointer.
        // During OBS shutdown that handler may already be tearing down, especially
        // on Windows, so avoid calling back into obs-websocket from module unload.
        g_vendor = nullptr;
    }

} // namespace alpha_recorder::obs
