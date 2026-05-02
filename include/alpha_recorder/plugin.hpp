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
        ProRes4444,
        LosslessHevc,
    };

    struct Settings
    {
        bool enabled = false;
        FinalizationFormat finalization_format = FinalizationFormat::ProRes4444;
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

    [[nodiscard]] inline constexpr FinalizationFormat finalization_format_default() noexcept
    {
        return FinalizationFormat::ProRes4444;
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
        return Settings{false, finalization_format_default()};
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

    inline constexpr std::array<FinalizationFormatOption, 2> finalization_format_options{{
        {FinalizationFormat::ProRes4444, "prores_4444", "Apple ProRes 4444"},
        {FinalizationFormat::LosslessHevc, "lossless_hevc", "Lossless HEVC"},
    }};

    [[nodiscard]] inline constexpr std::string_view finalization_format_export_unsupported_reason(FinalizationFormat format) noexcept
    {
        switch (format)
        {
        case FinalizationFormat::ProRes4444:
        case FinalizationFormat::LosslessHevc:
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
        case FinalizationFormat::ProRes4444:
            return ".mov";

        case FinalizationFormat::LosslessHevc:
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
    bool initialize_module() noexcept;

} // namespace alpha_recorder::obs
