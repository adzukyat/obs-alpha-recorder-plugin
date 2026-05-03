#include "alpha_recorder/plugin.hpp"
#include "alpha_recorder/export_worker.hpp"

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
