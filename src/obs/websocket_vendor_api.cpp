#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-websocket-api.h>

#include <util/config-file.h>

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

        void write_settings(obs_data_t *response, const Settings &settings)
        {
            obs_data_set_bool(response, "ok", true);
            obs_data_set_bool(response, settings_enabled_key().data(), settings.enabled);
            obs_data_set_string(response, settings_finalization_format_key().data(),
                                finalization_format_config_value(settings.finalization_format).data());
            obs_data_set_string(response, "finalization_format_display_name",
                                finalization_format_display_name(settings.finalization_format).data());
            obs_data_t *formats = obs_data_create();
            for (const FinalizationFormatOption &option : finalization_format_options)
            {
                std::string reason;
                const bool available = finalization_format_runtime_available(option.value, &reason);
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
                if (!finalization_format_runtime_available(parsed_format, &unavailable_reason))
                {
                    write_error(response, std::string{"Unsupported finalization_format: "} + unavailable_reason);
                    return;
                }

                settings.finalization_format = normalize_finalization_format(parsed_format);
            }

            config_set_bool(config, settings_section().data(), settings_enabled_key().data(), settings.enabled);
            config_set_string(config, settings_section().data(), settings_finalization_format_key().data(),
                              finalization_format_config_value(settings.finalization_format).data());

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
