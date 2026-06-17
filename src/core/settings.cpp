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

        void persist_hevc_nvenc_split_encode(struct config_data *config, HevcNvencSplitEncodeMode mode) noexcept
        {
            if (config == nullptr)
            {
                return;
            }

            config_set_string(config, settings_section().data(), settings_hevc_nvenc_split_encode_key().data(),
                              hevc_nvenc_split_encode_config_value(mode).data());
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
                const std::int64_t stored_cq = config_get_int(config, settings_section().data(), settings_hevc_quality_cq_key().data());
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

            if (config_has_user_value(config, settings_section().data(), settings_hevc_nvenc_tune_key().data()))
            {
                const char *stored_tune = config_get_string(config, settings_section().data(), settings_hevc_nvenc_tune_key().data());
                HevcNvencTune parsed_tune = settings.nvenc_tune;
                if (stored_tune != nullptr && try_parse_hevc_nvenc_tune(std::string_view{stored_tune}, parsed_tune))
                {
                    settings.nvenc_tune = parsed_tune;
                }
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_gop_size_key().data()))
            {
                const std::int64_t stored_gop = config_get_int(config, settings_section().data(), settings_hevc_gop_size_key().data());
                settings.gop_size = clamp_hevc_gop_size(static_cast<std::uint32_t>(stored_gop < 0 ? 0 : stored_gop));
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_b_frames_key().data()))
            {
                const std::int64_t stored_b_frames = config_get_int(config, settings_section().data(), settings_hevc_b_frames_key().data());
                settings.b_frames = clamp_hevc_b_frames(static_cast<std::uint32_t>(stored_b_frames < 0 ? 0 : stored_b_frames));
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_lookahead_key().data()))
            {
                const std::int64_t stored_lookahead = config_get_int(config, settings_section().data(), settings_hevc_lookahead_key().data());
                settings.lookahead = clamp_hevc_lookahead(static_cast<std::uint32_t>(stored_lookahead < 0 ? 0 : stored_lookahead));
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_adaptive_quantization_key().data()))
            {
                settings.adaptive_quantization =
                    config_get_bool(config, settings_section().data(), settings_hevc_adaptive_quantization_key().data());
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_nvenc_split_encode_key().data()))
            {
                const char *stored_split_encode =
                    config_get_string(config, settings_section().data(), settings_hevc_nvenc_split_encode_key().data());
                HevcNvencSplitEncodeMode parsed_split_encode = settings.nvenc_split_encode;
                if (stored_split_encode != nullptr &&
                    try_parse_hevc_nvenc_split_encode(std::string_view{stored_split_encode}, parsed_split_encode))
                {
                    settings.nvenc_split_encode = parsed_split_encode;
                }
            }

            if (config_has_user_value(config, settings_section().data(), settings_hevc_nvenc_gpu_index_key().data()))
            {
                const std::int64_t stored_gpu_index =
                    config_get_int(config, settings_section().data(), settings_hevc_nvenc_gpu_index_key().data());
                settings.nvenc_gpu_index = normalize_hevc_nvenc_gpu_index_from_int64(stored_gpu_index);
            }
        }

        void load_diagnostic_settings(struct config_data *config, Settings &settings) noexcept
        {
            if (config_has_user_value(config, settings_section().data(), settings_diagnostic_logging_key().data()))
            {
                settings.diagnostic_logging =
                    config_get_bool(config, settings_section().data(), settings_diagnostic_logging_key().data());
            }
        }

        void sanitize_hevc_nvenc_runtime_settings(struct config_data *config,
                                                  HevcEncoderSettings &settings) noexcept
        {
            bool changed = false;
            if (settings.nvenc_split_encode != HevcNvencSplitEncodeMode::Auto &&
                !hevc_nvenc_split_encode_runtime_available(settings.nvenc_split_encode))
            {
                settings.nvenc_split_encode = HevcNvencSplitEncodeMode::Auto;
                persist_hevc_nvenc_split_encode(config, settings.nvenc_split_encode);
                changed = true;
            }

            if (changed && config != nullptr)
            {
                (void)config_save(config);
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
        load_diagnostic_settings(config, settings);

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

        if (settings.finalization_format == FinalizationFormat::MaskHevcNvenc)
        {
            sanitize_hevc_nvenc_runtime_settings(config, settings.hevc_encoder);
        }

        return settings;
    }

} // namespace alpha_recorder::obs
