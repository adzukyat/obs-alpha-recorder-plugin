#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

struct config_data;

namespace alpha_recorder::obs
{

    enum class FinalizationFormat
    {
        MaskPngMov,
        MaskHevcNvenc,
        MaskHevcAmf,
    };

    enum class HevcQualityProfile
    {
        Lossless,
        HighQuality,
        Balanced,
        Fast,
    };

    enum class HevcEncoderPreset
    {
        Fast,
        Balanced,
        Quality,
    };

    struct HevcEncoderSettings
    {
        HevcQualityProfile quality_profile = HevcQualityProfile::HighQuality;
        std::uint32_t quality_cq = 19;
        HevcEncoderPreset preset = HevcEncoderPreset::Balanced;
    };

    struct Settings
    {
        bool enabled = false;
        FinalizationFormat finalization_format = FinalizationFormat::MaskPngMov;
        HevcEncoderSettings hevc_encoder{};
    };

    struct FinalizationFormatOption
    {
        FinalizationFormat value;
        std::string_view config_value;
        std::string_view display_name;
    };

    [[nodiscard]] inline constexpr std::string_view settings_section() noexcept
    {
        return "AlphaRecorder";
    }

    [[nodiscard]] inline constexpr std::string_view settings_enabled_key() noexcept
    {
        return "enabled";
    }

    [[nodiscard]] inline constexpr std::string_view settings_finalization_format_key() noexcept
    {
        return "finalization_format";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_quality_profile_key() noexcept
    {
        return "hevc_quality_profile";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_quality_cq_key() noexcept
    {
        return "hevc_quality_cq";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_preset_key() noexcept
    {
        return "hevc_preset";
    }

    [[nodiscard]] inline constexpr FinalizationFormat finalization_format_default() noexcept
    {
        return FinalizationFormat::MaskPngMov;
    }

    [[nodiscard]] inline std::filesystem::path recording_sidecar_path(const std::filesystem::path &recording_path)
    {
        std::filesystem::path sidecar_path = recording_path;
        sidecar_path.replace_extension(".alpha.sidecar");
        return sidecar_path;
    }

    [[nodiscard]] inline std::filesystem::path recording_manifest_path(const std::filesystem::path &recording_path)
    {
        std::filesystem::path manifest_path = recording_path;
        manifest_path.replace_extension(".alpha.manifest.json");
        return manifest_path;
    }

    [[nodiscard]] inline constexpr Settings default_settings() noexcept
    {
        return Settings{true, finalization_format_default(), HevcEncoderSettings{}};
    }

    [[nodiscard]] inline std::uint64_t frame_pts_from_elapsed_ns(std::uint64_t elapsed_ns,
                                                                 std::uint32_t fps_num,
                                                                 std::uint32_t fps_den) noexcept
    {
        if (fps_num == 0U || fps_den == 0U)
        {
            return 0U;
        }

        const std::uint64_t denominator = 1000000000ULL * static_cast<std::uint64_t>(fps_den);
        const std::uint64_t quotient = elapsed_ns / denominator;
        const std::uint64_t remainder = elapsed_ns % denominator;

        return (quotient * static_cast<std::uint64_t>(fps_num)) +
               ((remainder * static_cast<std::uint64_t>(fps_num)) / denominator);
    }

    inline constexpr std::array<FinalizationFormatOption, 3> finalization_format_options{{
        {FinalizationFormat::MaskPngMov, "mask_png_mov", "Lossless PNG MOV Mask"},
        {FinalizationFormat::MaskHevcNvenc, "mask_hevc_nvenc", "HEVC NVENC Mask"},
        {FinalizationFormat::MaskHevcAmf, "mask_hevc_amf", "HEVC AMF Mask"},
    }};

    [[nodiscard]] inline constexpr std::string_view hevc_quality_profile_config_value(HevcQualityProfile profile) noexcept
    {
        switch (profile)
        {
        case HevcQualityProfile::Lossless:
            return "lossless";
        case HevcQualityProfile::HighQuality:
            return "high_quality";
        case HevcQualityProfile::Balanced:
            return "balanced";
        case HevcQualityProfile::Fast:
            return "fast";
        }

        return "high_quality";
    }

    [[nodiscard]] inline constexpr std::string_view hevc_quality_profile_display_name(HevcQualityProfile profile) noexcept
    {
        switch (profile)
        {
        case HevcQualityProfile::Lossless:
            return "Lossless";
        case HevcQualityProfile::HighQuality:
            return "High Quality";
        case HevcQualityProfile::Balanced:
            return "Balanced";
        case HevcQualityProfile::Fast:
            return "Fast";
        }

        return "High Quality";
    }

    [[nodiscard]] inline bool try_parse_hevc_quality_profile(std::string_view value, HevcQualityProfile &profile) noexcept
    {
        if (value == "lossless")
        {
            profile = HevcQualityProfile::Lossless;
            return true;
        }
        if (value == "high_quality")
        {
            profile = HevcQualityProfile::HighQuality;
            return true;
        }
        if (value == "balanced")
        {
            profile = HevcQualityProfile::Balanced;
            return true;
        }
        if (value == "fast")
        {
            profile = HevcQualityProfile::Fast;
            return true;
        }

        return false;
    }

    [[nodiscard]] inline constexpr std::string_view hevc_encoder_preset_config_value(HevcEncoderPreset preset) noexcept
    {
        switch (preset)
        {
        case HevcEncoderPreset::Fast:
            return "fast";
        case HevcEncoderPreset::Balanced:
            return "balanced";
        case HevcEncoderPreset::Quality:
            return "quality";
        }

        return "balanced";
    }

    [[nodiscard]] inline constexpr std::string_view hevc_encoder_preset_display_name(HevcEncoderPreset preset) noexcept
    {
        switch (preset)
        {
        case HevcEncoderPreset::Fast:
            return "Fast";
        case HevcEncoderPreset::Balanced:
            return "Balanced";
        case HevcEncoderPreset::Quality:
            return "Quality";
        }

        return "Balanced";
    }

    [[nodiscard]] inline bool try_parse_hevc_encoder_preset(std::string_view value, HevcEncoderPreset &preset) noexcept
    {
        if (value == "fast")
        {
            preset = HevcEncoderPreset::Fast;
            return true;
        }
        if (value == "balanced")
        {
            preset = HevcEncoderPreset::Balanced;
            return true;
        }
        if (value == "quality")
        {
            preset = HevcEncoderPreset::Quality;
            return true;
        }

        return false;
    }

    [[nodiscard]] inline constexpr std::uint32_t clamp_hevc_quality_cq(std::uint32_t cq) noexcept
    {
        return cq > 51U ? 51U : cq;
    }

    [[nodiscard]] inline constexpr std::string_view finalization_format_export_unsupported_reason(FinalizationFormat format) noexcept
    {
        switch (format)
        {
        case FinalizationFormat::MaskPngMov:
        case FinalizationFormat::MaskHevcNvenc:
        case FinalizationFormat::MaskHevcAmf:
            return {};
        }

        return "unsupported finalization format";
    }

    [[nodiscard]] inline constexpr bool finalization_format_is_supported(FinalizationFormat format) noexcept
    {
        return finalization_format_export_unsupported_reason(format).empty();
    }

    [[nodiscard]] inline constexpr FinalizationFormat normalize_finalization_format(FinalizationFormat format) noexcept
    {
        return finalization_format_is_supported(format) ? format : finalization_format_default();
    }

    [[nodiscard]] inline constexpr std::string_view finalization_format_output_extension(FinalizationFormat format) noexcept
    {
        switch (format)
        {
        case FinalizationFormat::MaskPngMov:
            return ".mov";

        case FinalizationFormat::MaskHevcNvenc:
        case FinalizationFormat::MaskHevcAmf:
            return ".mp4";
        }

        return ".mov";
    }

    [[nodiscard]] inline std::filesystem::path finalization_output_path(const std::filesystem::path &sidecar_path,
                                                                        FinalizationFormat format)
    {
        std::filesystem::path output_path = sidecar_path;
        output_path.replace_extension(finalization_format_output_extension(format));
        return output_path;
    }

    [[nodiscard]] inline std::filesystem::path recording_alpha_movie_path(const std::filesystem::path &recording_path,
                                                                          FinalizationFormat format)
    {
        std::filesystem::path movie_path = recording_path;
        movie_path.replace_extension(std::string{".alpha"} + std::string{finalization_format_output_extension(format)});
        return movie_path;
    }

    [[nodiscard]] inline std::string_view finalization_format_config_value(FinalizationFormat format) noexcept
    {
        for (const FinalizationFormatOption &option : finalization_format_options)
        {
            if (option.value == format)
            {
                return option.config_value;
            }
        }

        return finalization_format_options.front().config_value;
    }

    [[nodiscard]] inline std::string_view finalization_format_display_name(FinalizationFormat format) noexcept
    {
        for (const FinalizationFormatOption &option : finalization_format_options)
        {
            if (option.value == format)
            {
                return option.display_name;
            }
        }

        return finalization_format_options.front().display_name;
    }

    [[nodiscard]] inline bool try_parse_finalization_format(std::string_view value, FinalizationFormat &format) noexcept
    {
        for (const FinalizationFormatOption &option : finalization_format_options)
        {
            if (option.config_value == value)
            {
                format = option.value;
                return true;
            }
        }

        if (value == "mask_prores_422" || value == "prores_4444")
        {
            format = FinalizationFormat::MaskPngMov;
            return true;
        }

        if (value == "lossless_hevc")
        {
            format = FinalizationFormat::MaskHevcNvenc;
            return true;
        }

        return false;
    }

    [[nodiscard]] std::string_view module_name() noexcept;
    [[nodiscard]] std::string_view module_description() noexcept;
    [[nodiscard]] Settings load_settings(struct config_data *config) noexcept;
    bool register_runtime_hooks() noexcept;
    void unregister_runtime_hooks() noexcept;
    void register_websocket_vendor_api() noexcept;
    void unregister_websocket_vendor_api() noexcept;
    bool register_output_module() noexcept;
    bool register_e2e_sources() noexcept;
    bool initialize_module() noexcept;

} // namespace alpha_recorder::obs
