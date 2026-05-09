#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

#include <util/config-file.h>

#include "alpha_recorder/export_worker.hpp"
#include "alpha_recorder/plugin.hpp"

int main()
{
    const alpha_recorder::obs::Settings defaults = alpha_recorder::obs::default_settings();
    if (!defaults.enabled || defaults.finalization_format != alpha_recorder::obs::FinalizationFormat::MaskPngMov ||
        defaults.hevc_encoder.quality_profile != alpha_recorder::obs::HevcQualityProfile::HighQuality ||
        defaults.hevc_encoder.quality_cq != 19U ||
        defaults.hevc_encoder.preset != alpha_recorder::obs::HevcEncoderPreset::NvencP3 ||
        defaults.hevc_encoder.nvenc_tune != alpha_recorder::obs::HevcNvencTune::HighQuality ||
        defaults.hevc_encoder.gop_size != 0U ||
        defaults.hevc_encoder.b_frames != 0U ||
        defaults.hevc_encoder.lookahead != 0U ||
        defaults.hevc_encoder.adaptive_quantization)
    {
        std::cerr << "default settings are incorrect\n";
        return 1;
    }

    if (alpha_recorder::obs::settings_section() != "AlphaRecorder" || alpha_recorder::obs::settings_enabled_key() != "enabled" ||
        alpha_recorder::obs::settings_finalization_format_key() != "finalization_format" ||
        alpha_recorder::obs::settings_hevc_quality_profile_key() != "hevc_quality_profile" ||
        alpha_recorder::obs::settings_hevc_quality_cq_key() != "hevc_quality_cq" ||
        alpha_recorder::obs::settings_hevc_preset_key() != "hevc_preset" ||
        alpha_recorder::obs::settings_hevc_nvenc_tune_key() != "hevc_nvenc_tune" ||
        alpha_recorder::obs::settings_hevc_gop_size_key() != "hevc_gop_size" ||
        alpha_recorder::obs::settings_hevc_b_frames_key() != "hevc_b_frames" ||
        alpha_recorder::obs::settings_hevc_lookahead_key() != "hevc_lookahead" ||
        alpha_recorder::obs::settings_hevc_adaptive_quantization_key() != "hevc_adaptive_quantization")
    {
        std::cerr << "settings keys do not match the expected config layout\n";
        return 2;
    }

    if (alpha_recorder::obs::finalization_format_options.size() != 3U)
    {
        std::cerr << "unexpected finalization format option count\n";
        return 3;
    }

    if (alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskPngMov) != alpha_recorder::obs::FinalizationFormat::MaskPngMov ||
        alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) != alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc ||
        alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskHevcAmf) != alpha_recorder::obs::FinalizationFormat::MaskHevcAmf)
    {
        std::cerr << "finalization format normalization is incorrect\n";
        return 4;
    }

    if (!alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskPngMov) ||
        !alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) ||
        !alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskHevcAmf))
    {
        std::cerr << "finalization support classification is incorrect\n";
        return 5;
    }

    const std::filesystem::path alpha_sidecar = std::filesystem::path{"C:/Recordings/MyRec.alpha.sidecar"};
    if (alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskPngMov) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mov"} ||
        alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
        alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskHevcAmf) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"})
    {
        std::cerr << "finalization output path helper returned an unexpected value\n";
        return 6;
    }

    const std::filesystem::path recording_path = std::filesystem::path{"C:/Recordings/MyRec.mkv"};
    if (alpha_recorder::obs::recording_alpha_movie_path(recording_path, alpha_recorder::obs::FinalizationFormat::MaskPngMov) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mov"} ||
        alpha_recorder::obs::recording_alpha_movie_path(recording_path, alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
        alpha_recorder::obs::recording_sidecar_path(recording_path) != std::filesystem::path{"C:/Recordings/MyRec.alpha.sidecar"} ||
        alpha_recorder::obs::recording_manifest_path(recording_path) != std::filesystem::path{"C:/Recordings/MyRec.alpha.manifest.json"})
    {
        std::cerr << "recording path helpers do not match the expected OBS naming convention\n";
        return 7;
    }

    if (alpha_recorder::obs::finalization_format_display_name(alpha_recorder::obs::FinalizationFormat::MaskPngMov) != "Lossless PNG MOV Mask")
    {
        std::cerr << "PNG display name mismatch\n";
        return 8;
    }

    if (alpha_recorder::obs::finalization_format_config_value(alpha_recorder::obs::FinalizationFormat::MaskPngMov) != "mask_png_mov" ||
        alpha_recorder::obs::finalization_format_config_value(alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) != "mask_hevc_nvenc" ||
        alpha_recorder::obs::finalization_format_config_value(alpha_recorder::obs::FinalizationFormat::MaskHevcAmf) != "mask_hevc_amf")
    {
        std::cerr << "finalization format config value mismatch\n";
        return 9;
    }

    if (alpha_recorder::obs::frame_pts_from_elapsed_ns(33366667ULL, 30000U, 1001U) != 1U)
    {
        std::cerr << "fractional frame-rate pts conversion did not use fps_den\n";
        return 10;
    }

    if (alpha_recorder::obs::frame_pts_from_elapsed_ns(1000000000ULL, 60U, 1U) != 60U)
    {
        std::cerr << "integer frame-rate pts conversion mismatch\n";
        return 11;
    }

    alpha_recorder::obs::FinalizationFormat parsed_format = alpha_recorder::obs::FinalizationFormat::MaskPngMov;
    if (!alpha_recorder::obs::try_parse_finalization_format("mask_hevc_amf", parsed_format) || parsed_format != alpha_recorder::obs::FinalizationFormat::MaskHevcAmf)
    {
        std::cerr << "failed to parse the hevc amf config value\n";
        return 12;
    }

    if (!alpha_recorder::obs::try_parse_finalization_format("mask_prores_422", parsed_format) || parsed_format != alpha_recorder::obs::FinalizationFormat::MaskPngMov ||
        !alpha_recorder::obs::try_parse_finalization_format("prores_4444", parsed_format) || parsed_format != alpha_recorder::obs::FinalizationFormat::MaskPngMov ||
        !alpha_recorder::obs::try_parse_finalization_format("lossless_hevc", parsed_format) || parsed_format != alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc)
    {
        std::cerr << "legacy finalization format migration did not parse as expected\n";
        return 12;
    }

    if (alpha_recorder::obs::try_parse_finalization_format("not-a-format", parsed_format))
    {
        std::cerr << "invalid finalization format value should be rejected\n";
        return 13;
    }

    alpha_recorder::obs::HevcQualityProfile parsed_profile = alpha_recorder::obs::HevcQualityProfile::HighQuality;
    alpha_recorder::obs::HevcEncoderPreset parsed_preset = alpha_recorder::obs::HevcEncoderPreset::NvencP3;
    alpha_recorder::obs::HevcNvencTune parsed_tune = alpha_recorder::obs::HevcNvencTune::HighQuality;
    if (!alpha_recorder::obs::try_parse_hevc_quality_profile("fast", parsed_profile) ||
        parsed_profile != alpha_recorder::obs::HevcQualityProfile::Fast ||
        alpha_recorder::obs::hevc_quality_profile_config_value(alpha_recorder::obs::HevcQualityProfile::Balanced) != "balanced" ||
        !alpha_recorder::obs::try_parse_hevc_encoder_preset("nvenc_p5", parsed_preset) ||
        parsed_preset != alpha_recorder::obs::HevcEncoderPreset::NvencP5 ||
        !alpha_recorder::obs::try_parse_hevc_encoder_preset("lossless", parsed_preset) ||
        parsed_preset != alpha_recorder::obs::HevcEncoderPreset::NvencLossless ||
        !alpha_recorder::obs::try_parse_hevc_encoder_preset("quality", parsed_preset) ||
        parsed_preset != alpha_recorder::obs::HevcEncoderPreset::NvencP5 ||
        alpha_recorder::obs::hevc_encoder_preset_config_value(alpha_recorder::obs::HevcEncoderPreset::AmfSpeed) != "amf_speed" ||
        !alpha_recorder::obs::try_parse_hevc_nvenc_tune("lossless", parsed_tune) ||
        parsed_tune != alpha_recorder::obs::HevcNvencTune::Lossless ||
        !alpha_recorder::obs::try_parse_hevc_nvenc_tune("ull", parsed_tune) ||
        parsed_tune != alpha_recorder::obs::HevcNvencTune::UltraLowLatency ||
        alpha_recorder::obs::hevc_nvenc_tune_config_value(alpha_recorder::obs::HevcNvencTune::LowLatency) != "ll" ||
        alpha_recorder::obs::clamp_hevc_quality_cq(99U) != 51U ||
        alpha_recorder::obs::clamp_hevc_gop_size(1200U) != 1000U ||
        alpha_recorder::obs::clamp_hevc_b_frames(8U) != 4U ||
        alpha_recorder::obs::clamp_hevc_lookahead(64U) != 32U)
    {
        std::cerr << "hevc encoder setting helpers are incorrect\n";
        return 14;
    }

    config_t *config = nullptr;
    if (config_open_string(&config, "[AlphaRecorder]\nenabled=true\nfinalization_format=mask_png_mov\nhevc_quality_profile=fast\nhevc_quality_cq=64\nhevc_preset=amf_quality\nhevc_nvenc_tune=ull\nhevc_gop_size=1200\nhevc_b_frames=8\nhevc_lookahead=64\nhevc_adaptive_quantization=true\n") != CONFIG_SUCCESS || config == nullptr)
    {
        std::cerr << "failed to open an in-memory config string\n";
        return 15;
    }

    const alpha_recorder::obs::Settings loaded_settings = alpha_recorder::obs::load_settings(config);
    config_close(config);
    if (!loaded_settings.enabled || loaded_settings.finalization_format != alpha_recorder::obs::FinalizationFormat::MaskPngMov ||
        loaded_settings.hevc_encoder.quality_profile != alpha_recorder::obs::HevcQualityProfile::Fast ||
        loaded_settings.hevc_encoder.quality_cq != 51U ||
        loaded_settings.hevc_encoder.preset != alpha_recorder::obs::HevcEncoderPreset::AmfQuality ||
        loaded_settings.hevc_encoder.nvenc_tune != alpha_recorder::obs::HevcNvencTune::UltraLowLatency ||
        loaded_settings.hevc_encoder.gop_size != 1000U ||
        loaded_settings.hevc_encoder.b_frames != 4U ||
        loaded_settings.hevc_encoder.lookahead != 32U ||
        !loaded_settings.hevc_encoder.adaptive_quantization)
    {
        std::cerr << "valid config values were not preserved by the loader\n";
        return 16;
    }

    if (config_open_string(&config, "[AlphaRecorder]\n") != CONFIG_SUCCESS || config == nullptr)
    {
        std::cerr << "failed to open an in-memory default config string\n";
        return 17;
    }

    const alpha_recorder::obs::Settings missing_key_settings = alpha_recorder::obs::load_settings(config);
    config_close(config);
    if (!missing_key_settings.enabled ||
        missing_key_settings.finalization_format != alpha_recorder::obs::preferred_runtime_finalization_format())
    {
        std::cerr << "missing settings did not use enabled and preferred runtime defaults\n";
        return 18;
    }

    const std::filesystem::path temp_root = std::filesystem::temp_directory_path() / "alpha_recorder_settings_test";
    std::error_code remove_error;
    std::filesystem::remove_all(temp_root, remove_error);
    std::filesystem::create_directories(temp_root);

    const std::filesystem::path config_path = temp_root / "settings.ini";
    config_t *file_config = nullptr;
    const std::string config_path_text = config_path.generic_string();
    if (config_open(&file_config, config_path_text.c_str(), CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS || file_config == nullptr)
    {
        std::cerr << "failed to open a file-backed config for persistence verification\n";
        return 40;
    }

    config_set_bool(file_config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_enabled_key().data(), true);
    config_set_string(file_config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data(), "lossless_hevc");
    if (config_save(file_config) != CONFIG_SUCCESS)
    {
        std::cerr << "failed to seed the file-backed config\n";
        config_close(file_config);
        return 41;
    }

    const alpha_recorder::obs::Settings rewritten_settings = alpha_recorder::obs::load_settings(file_config);
    const char *rewritten_format = config_get_string(file_config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data());
    const alpha_recorder::obs::FinalizationFormat expected_rewritten_format =
        alpha_recorder::obs::finalization_format_runtime_available(alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc)
            ? alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc
            : alpha_recorder::obs::preferred_runtime_finalization_format();
    const std::string expected_rewritten_text{alpha_recorder::obs::finalization_format_config_value(expected_rewritten_format)};
    if (!rewritten_settings.enabled || rewritten_settings.finalization_format != expected_rewritten_format || rewritten_format == nullptr || std::string{rewritten_format} != expected_rewritten_text)
    {
        std::cerr << "lossless hevc config values were not preserved in the persisted config\n";
        config_close(file_config);
        return 42;
    }

    config_close(file_config);

    std::ifstream config_stream(config_path, std::ios::binary);
    if (!config_stream)
    {
        std::cerr << "failed to reopen the persisted config for verification\n";
        return 43;
    }

    const std::string config_text((std::istreambuf_iterator<char>(config_stream)), std::istreambuf_iterator<char>());
    if (config_text.find("finalization_format=" + expected_rewritten_text) == std::string::npos)
    {
        std::cerr << "the hevc finalization format was not written back to disk\n";
        return 44;
    }

    std::cout << "settings mapping test passed\n";
    return 0;
}
