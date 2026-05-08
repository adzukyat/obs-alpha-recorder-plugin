#include "alpha_recorder/plugin.hpp"
#include "alpha_recorder/export_worker.hpp"

#include <cstdint>
#include <string_view>

#include <util/config-file.h>

namespace alpha_recorder::obs
{

    namespace
    {

        void persist_normalized_finalization_format(struct config_data *config, FinalizationFormat format) noexcept
        {
            if (config == nullptr)
            {
                return;
            }

            config_set_string(config, settings_section().data(), settings_finalization_format_key().data(),
                              finalization_format_config_value(format).data());
            (void)config_save(config);
        }

        void load_hevc_encoder_settings(struct config_data *config, HevcEncoderSettings &settings) noexcept
        {
            if (config_has_user_value(config, settings_section().data(), settings_hevc_quality_profile_key().data()))
            {
                const char *stored_profile = config_get_string(config, settings_section().data(), settings_hevc_quality_profile_key().data());
                HevcQualityProfile parsed_profile = settings.quality_profile;
                if (stored_profile != nullptr && try_parse_hevc_quality_profile(std::string_view{stored_profile}, parsed_profile))
                {
                    settings.quality_profile = parsed_profile;
                }
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_quality_cq_key().data()))
            {
                const int stored_cq = config_get_int(config, settings_section().data(), settings_hevc_quality_cq_key().data());
                settings.quality_cq = clamp_hevc_quality_cq(static_cast<std::uint32_t>(stored_cq < 0 ? 0 : stored_cq));
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_preset_key().data()))
            {
                const char *stored_preset = config_get_string(config, settings_section().data(), settings_hevc_preset_key().data());
                HevcEncoderPreset parsed_preset = settings.preset;
                if (stored_preset != nullptr && try_parse_hevc_encoder_preset(std::string_view{stored_preset}, parsed_preset))
                {
                    settings.preset = parsed_preset;
                }
            }
        }

    } // namespace

    Settings load_settings(struct config_data *config) noexcept
    {
        Settings settings = default_settings();
        if (config == nullptr)
        {
            return settings;
        }

        if (config_has_user_value(config, settings_section().data(), settings_enabled_key().data()))
        {
            settings.enabled = config_get_bool(config, settings_section().data(), settings_enabled_key().data());
        }

        load_hevc_encoder_settings(config, settings.hevc_encoder);

        settings.finalization_format = preferred_runtime_finalization_format();

        const char *stored_format = config_get_string(config, settings_section().data(), settings_finalization_format_key().data());
        if (stored_format != nullptr)
        {
            FinalizationFormat parsed_format = settings.finalization_format;
            const bool has_user_value = config_has_user_value(config, settings_section().data(), settings_finalization_format_key().data());
            if (try_parse_finalization_format(std::string_view{stored_format}, parsed_format))
            {
                const FinalizationFormat normalized_format = normalize_finalization_format(parsed_format);
                if (finalization_format_runtime_available(normalized_format))
                {
                    settings.finalization_format = normalized_format;
                }
            }

            if (has_user_value && finalization_format_config_value(settings.finalization_format) != std::string_view{stored_format})
            {
                persist_normalized_finalization_format(config, settings.finalization_format);
            }
        }

        return settings;
    }

} // namespace alpha_recorder::obs
