#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>

struct config_data;

namespace alpha_recorder::obs
{

    enum class FinalizationFormat
    {
        MaskPngMov,
        MaskHevcNvenc,
        MaskHevcAmf,
        MaskHevcQsv,
        MaskHevcVaapi,
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
        NvencLossless,
        NvencP1,
        NvencP2,
        NvencP3,
        NvencP4,
        NvencP5,
        NvencP6,
        NvencP7,
        AmfSpeed,
        AmfBalanced,
        AmfQuality,
    };

    enum class HevcNvencTune
    {
        Lossless,
        HighQuality,
        LowLatency,
        UltraLowLatency,
    };

    enum class HevcNvencSplitEncodeMode
    {
        Auto,
        Disabled,
        Forced,
        TwoWay,
        ThreeWay,
    };

    struct HevcEncoderSettings
    {
        HevcQualityProfile quality_profile = HevcQualityProfile::HighQuality;
        std::uint32_t quality_cq = 19;
        HevcEncoderPreset preset = HevcEncoderPreset::NvencP3;
        HevcNvencTune nvenc_tune = HevcNvencTune::HighQuality;
        std::uint32_t gop_size = 0;
        std::uint32_t b_frames = 0;
        std::uint32_t lookahead = 0;
        bool adaptive_quantization = false;
        HevcNvencSplitEncodeMode nvenc_split_encode = HevcNvencSplitEncodeMode::Auto;
        std::int32_t nvenc_gpu_index = -1;
    };

    struct Settings
    {
        bool enabled = false;
        FinalizationFormat finalization_format = FinalizationFormat::MaskPngMov;
        HevcEncoderSettings hevc_encoder{};
        bool diagnostic_logging = false;
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

    [[nodiscard]] inline constexpr std::string_view settings_hevc_nvenc_tune_key() noexcept
    {
        return "hevc_nvenc_tune";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_gop_size_key() noexcept
    {
        return "hevc_gop_size";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_b_frames_key() noexcept
    {
        return "hevc_b_frames";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_lookahead_key() noexcept
    {
        return "hevc_lookahead";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_adaptive_quantization_key() noexcept
    {
        return "hevc_adaptive_quantization";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_nvenc_split_encode_key() noexcept
    {
        return "hevc_nvenc_split_encode";
    }

    [[nodiscard]] inline constexpr std::string_view settings_hevc_nvenc_gpu_index_key() noexcept
    {
        return "hevc_nvenc_gpu_index";
    }

    [[nodiscard]] inline constexpr std::string_view settings_diagnostic_logging_key() noexcept
    {
        return "diagnostic_logging";
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
        return Settings{true, finalization_format_default(), HevcEncoderSettings{}, false};
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

    inline constexpr std::array<FinalizationFormatOption, 5> finalization_format_options{{
        {FinalizationFormat::MaskPngMov, "mask_png_mov", "Lossless PNG MOV Mask"},
        {FinalizationFormat::MaskHevcNvenc, "mask_hevc_nvenc", "HEVC NVENC Mask"},
        {FinalizationFormat::MaskHevcAmf, "mask_hevc_amf", "HEVC AMF Mask"},
        {FinalizationFormat::MaskHevcQsv, "mask_hevc_qsv", "HEVC QSV Mask"},
        {FinalizationFormat::MaskHevcVaapi, "mask_hevc_vaapi", "HEVC VAAPI Mask"},
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
        case HevcEncoderPreset::NvencLossless:
            return "nvenc_lossless";
        case HevcEncoderPreset::NvencP1:
            return "nvenc_p1";
        case HevcEncoderPreset::NvencP2:
            return "nvenc_p2";
        case HevcEncoderPreset::NvencP3:
            return "nvenc_p3";
        case HevcEncoderPreset::NvencP4:
            return "nvenc_p4";
        case HevcEncoderPreset::NvencP5:
            return "nvenc_p5";
        case HevcEncoderPreset::NvencP6:
            return "nvenc_p6";
        case HevcEncoderPreset::NvencP7:
            return "nvenc_p7";
        case HevcEncoderPreset::AmfSpeed:
            return "amf_speed";
        case HevcEncoderPreset::AmfBalanced:
            return "amf_balanced";
        case HevcEncoderPreset::AmfQuality:
            return "amf_quality";
        }

        return "nvenc_p3";
    }

    [[nodiscard]] inline constexpr std::string_view hevc_encoder_preset_display_name(HevcEncoderPreset preset) noexcept
    {
        switch (preset)
        {
        case HevcEncoderPreset::NvencLossless:
            return "Lossless";
        case HevcEncoderPreset::NvencP1:
            return "P1";
        case HevcEncoderPreset::NvencP2:
            return "P2";
        case HevcEncoderPreset::NvencP3:
            return "P3";
        case HevcEncoderPreset::NvencP4:
            return "P4";
        case HevcEncoderPreset::NvencP5:
            return "P5";
        case HevcEncoderPreset::NvencP6:
            return "P6";
        case HevcEncoderPreset::NvencP7:
            return "P7";
        case HevcEncoderPreset::AmfSpeed:
            return "Speed";
        case HevcEncoderPreset::AmfBalanced:
            return "Balanced";
        case HevcEncoderPreset::AmfQuality:
            return "Quality";
        }

        return "P3";
    }

    [[nodiscard]] inline bool try_parse_hevc_encoder_preset(std::string_view value, HevcEncoderPreset &preset) noexcept
    {
        if (value == "nvenc_lossless" || value == "lossless")
        {
            preset = HevcEncoderPreset::NvencLossless;
            return true;
        }
        if (value == "nvenc_p1" || value == "p1")
        {
            preset = HevcEncoderPreset::NvencP1;
            return true;
        }
        if (value == "nvenc_p2" || value == "p2" || value == "fast")
        {
            preset = HevcEncoderPreset::NvencP2;
            return true;
        }
        if (value == "nvenc_p3" || value == "p3" || value == "balanced")
        {
            preset = HevcEncoderPreset::NvencP3;
            return true;
        }
        if (value == "nvenc_p4" || value == "p4")
        {
            preset = HevcEncoderPreset::NvencP4;
            return true;
        }
        if (value == "nvenc_p5" || value == "p5" || value == "quality")
        {
            preset = HevcEncoderPreset::NvencP5;
            return true;
        }
        if (value == "nvenc_p6" || value == "p6")
        {
            preset = HevcEncoderPreset::NvencP6;
            return true;
        }
        if (value == "nvenc_p7" || value == "p7")
        {
            preset = HevcEncoderPreset::NvencP7;
            return true;
        }
        if (value == "amf_speed")
        {
            preset = HevcEncoderPreset::AmfSpeed;
            return true;
        }
        if (value == "amf_balanced")
        {
            preset = HevcEncoderPreset::AmfBalanced;
            return true;
        }
        if (value == "amf_quality")
        {
            preset = HevcEncoderPreset::AmfQuality;
            return true;
        }

        return false;
    }

    [[nodiscard]] inline constexpr std::string_view hevc_nvenc_tune_config_value(HevcNvencTune tune) noexcept
    {
        switch (tune)
        {
        case HevcNvencTune::Lossless:
            return "lossless";
        case HevcNvencTune::HighQuality:
            return "hq";
        case HevcNvencTune::LowLatency:
            return "ll";
        case HevcNvencTune::UltraLowLatency:
            return "ull";
        }

        return "hq";
    }

    [[nodiscard]] inline constexpr std::string_view hevc_nvenc_tune_display_name(HevcNvencTune tune) noexcept
    {
        switch (tune)
        {
        case HevcNvencTune::Lossless:
            return "Lossless";
        case HevcNvencTune::HighQuality:
            return "High Quality";
        case HevcNvencTune::LowLatency:
            return "Low Latency";
        case HevcNvencTune::UltraLowLatency:
            return "Ultra Low Latency";
        }

        return "High Quality";
    }

    [[nodiscard]] inline bool try_parse_hevc_nvenc_tune(std::string_view value, HevcNvencTune &tune) noexcept
    {
        if (value == "lossless")
        {
            tune = HevcNvencTune::Lossless;
            return true;
        }
        if (value == "hq" || value == "high_quality")
        {
            tune = HevcNvencTune::HighQuality;
            return true;
        }
        if (value == "ll" || value == "low_latency")
        {
            tune = HevcNvencTune::LowLatency;
            return true;
        }
        if (value == "ull" || value == "ultra_low_latency")
        {
            tune = HevcNvencTune::UltraLowLatency;
            return true;
        }

        return false;
    }

    [[nodiscard]] inline constexpr std::string_view hevc_nvenc_split_encode_config_value(HevcNvencSplitEncodeMode mode) noexcept
    {
        switch (mode)
        {
        case HevcNvencSplitEncodeMode::Auto:
            return "auto";
        case HevcNvencSplitEncodeMode::Disabled:
            return "disabled";
        case HevcNvencSplitEncodeMode::Forced:
            return "forced";
        case HevcNvencSplitEncodeMode::TwoWay:
            return "2";
        case HevcNvencSplitEncodeMode::ThreeWay:
            return "3";
        }

        return "auto";
    }

    [[nodiscard]] inline constexpr std::string_view hevc_nvenc_split_encode_display_name(HevcNvencSplitEncodeMode mode) noexcept
    {
        switch (mode)
        {
        case HevcNvencSplitEncodeMode::Auto:
            return "Auto";
        case HevcNvencSplitEncodeMode::Disabled:
            return "Disabled";
        case HevcNvencSplitEncodeMode::Forced:
            return "Forced";
        case HevcNvencSplitEncodeMode::TwoWay:
            return "2 strips";
        case HevcNvencSplitEncodeMode::ThreeWay:
            return "3 strips";
        }

        return "Auto";
    }

    [[nodiscard]] inline bool try_parse_hevc_nvenc_split_encode(std::string_view value,
                                                                HevcNvencSplitEncodeMode &mode) noexcept
    {
        if (value == "auto" || value == "0")
        {
            mode = HevcNvencSplitEncodeMode::Auto;
            return true;
        }
        if (value == "disabled" || value == "disable" || value == "15")
        {
            mode = HevcNvencSplitEncodeMode::Disabled;
            return true;
        }
        if (value == "forced" || value == "force" || value == "1")
        {
            mode = HevcNvencSplitEncodeMode::Forced;
            return true;
        }
        if (value == "2" || value == "two" || value == "two_way" || value == "2_strips")
        {
            mode = HevcNvencSplitEncodeMode::TwoWay;
            return true;
        }
        if (value == "3" || value == "three" || value == "three_way" || value == "3_strips")
        {
            mode = HevcNvencSplitEncodeMode::ThreeWay;
            return true;
        }

        return false;
    }

    [[nodiscard]] inline constexpr std::uint32_t clamp_hevc_quality_cq(std::uint32_t cq) noexcept
    {
        return cq > 51U ? 51U : cq;
    }

    [[nodiscard]] inline constexpr std::uint32_t clamp_hevc_gop_size(std::uint32_t gop_size) noexcept
    {
        return gop_size > 1000U ? 1000U : gop_size;
    }

    [[nodiscard]] inline constexpr std::uint32_t clamp_hevc_b_frames(std::uint32_t b_frames) noexcept
    {
        return b_frames > 4U ? 4U : b_frames;
    }

    [[nodiscard]] inline constexpr std::uint32_t clamp_hevc_lookahead(std::uint32_t lookahead) noexcept
    {
        return lookahead > 32U ? 32U : lookahead;
    }

    [[nodiscard]] inline constexpr bool try_normalize_hevc_nvenc_gpu_index(std::int64_t gpu_index,
                                                                            std::int32_t &normalized) noexcept
    {
        if (gpu_index < -1 || gpu_index > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()))
        {
            return false;
        }

        normalized = static_cast<std::int32_t>(gpu_index);
        return true;
    }

    [[nodiscard]] inline constexpr std::int32_t normalize_hevc_nvenc_gpu_index_from_int64(std::int64_t gpu_index) noexcept
    {
        std::int32_t normalized = -1;
        (void)try_normalize_hevc_nvenc_gpu_index(gpu_index, normalized);
        return normalized;
    }

    [[nodiscard]] inline constexpr std::int32_t normalize_hevc_nvenc_gpu_index(std::int32_t gpu_index) noexcept
    {
        return normalize_hevc_nvenc_gpu_index_from_int64(gpu_index);
    }

    [[nodiscard]] inline constexpr std::string_view finalization_format_export_unsupported_reason(FinalizationFormat format) noexcept
    {
        switch (format)
        {
        case FinalizationFormat::MaskPngMov:
        case FinalizationFormat::MaskHevcNvenc:
        case FinalizationFormat::MaskHevcAmf:
        case FinalizationFormat::MaskHevcQsv:
        case FinalizationFormat::MaskHevcVaapi:
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
        case FinalizationFormat::MaskHevcQsv:
        case FinalizationFormat::MaskHevcVaapi:
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

        return false;
    }

    [[nodiscard]] std::string_view module_name() noexcept;
    [[nodiscard]] std::string_view module_description() noexcept;
    [[nodiscard]] Settings load_settings(struct config_data *config) noexcept;
    bool register_runtime_hooks() noexcept;
    void unregister_runtime_hooks() noexcept;
    void register_websocket_vendor_api() noexcept;
    void unregister_websocket_vendor_api() noexcept;
    void register_settings_ui() noexcept;
    void unregister_settings_ui() noexcept;
    bool register_output_module() noexcept;
    void unregister_output_module() noexcept;
    bool register_e2e_sources() noexcept;
    bool initialize_module() noexcept;

} // namespace alpha_recorder::obs
