#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
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
        defaults.hevc_encoder.adaptive_quantization ||
        defaults.hevc_encoder.nvenc_split_encode != alpha_recorder::obs::HevcNvencSplitEncodeMode::Auto ||
        defaults.hevc_encoder.nvenc_gpu_index != -1 ||
        defaults.diagnostic_logging)
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
        alpha_recorder::obs::settings_hevc_adaptive_quantization_key() != "hevc_adaptive_quantization" ||
        alpha_recorder::obs::settings_hevc_nvenc_split_encode_key() != "hevc_nvenc_split_encode" ||
        alpha_recorder::obs::settings_hevc_nvenc_gpu_index_key() != "hevc_nvenc_gpu_index" ||
        alpha_recorder::obs::settings_diagnostic_logging_key() != "diagnostic_logging")
    {
        std::cerr << "settings keys do not match the expected config layout\n";
        return 2;
    }

    if (alpha_recorder::obs::finalization_format_options.size() != 5U)
    {
        std::cerr << "unexpected finalization format option count\n";
        return 3;
    }

    if (alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskPngMov) != alpha_recorder::obs::FinalizationFormat::MaskPngMov ||
        alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) != alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc ||
        alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskHevcAmf) != alpha_recorder::obs::FinalizationFormat::MaskHevcAmf ||
        alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskHevcQsv) != alpha_recorder::obs::FinalizationFormat::MaskHevcQsv ||
        alpha_recorder::obs::normalize_finalization_format(alpha_recorder::obs::FinalizationFormat::MaskHevcVaapi) != alpha_recorder::obs::FinalizationFormat::MaskHevcVaapi)
    {
        std::cerr << "finalization format normalization is incorrect\n";
        return 4;
    }

    if (!alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskPngMov) ||
        !alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) ||
        !alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskHevcAmf) ||
        !alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskHevcQsv) ||
        !alpha_recorder::obs::finalization_format_is_supported(alpha_recorder::obs::FinalizationFormat::MaskHevcVaapi))
    {
        std::cerr << "finalization support classification is incorrect\n";
        return 5;
    }

    const std::filesystem::path alpha_sidecar = std::filesystem::path{"C:/Recordings/MyRec.alpha.sidecar"};
    if (alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskPngMov) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mov"} ||
        alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
        alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskHevcAmf) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
        alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskHevcQsv) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
        alpha_recorder::obs::finalization_output_path(alpha_sidecar, alpha_recorder::obs::FinalizationFormat::MaskHevcVaapi) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"})
    {
        std::cerr << "finalization output path helper returned an unexpected value\n";
        return 6;
    }

    const std::filesystem::path recording_path = std::filesystem::path{"C:/Recordings/MyRec.mkv"};
    if (alpha_recorder::obs::recording_alpha_movie_path(recording_path, alpha_recorder::obs::FinalizationFormat::MaskPngMov) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mov"} ||
        alpha_recorder::obs::recording_alpha_movie_path(recording_path, alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
        alpha_recorder::obs::recording_alpha_movie_path(recording_path, alpha_recorder::obs::FinalizationFormat::MaskHevcQsv) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
        alpha_recorder::obs::recording_alpha_movie_path(recording_path, alpha_recorder::obs::FinalizationFormat::MaskHevcVaapi) != std::filesystem::path{"C:/Recordings/MyRec.alpha.mp4"} ||
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
        alpha_recorder::obs::finalization_format_config_value(alpha_recorder::obs::FinalizationFormat::MaskHevcAmf) != "mask_hevc_amf" ||
        alpha_recorder::obs::finalization_format_config_value(alpha_recorder::obs::FinalizationFormat::MaskHevcQsv) != "mask_hevc_qsv" ||
        alpha_recorder::obs::finalization_format_config_value(alpha_recorder::obs::FinalizationFormat::MaskHevcVaapi) != "mask_hevc_vaapi")
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
    if (!alpha_recorder::obs::try_parse_finalization_format("mask_hevc_qsv", parsed_format) || parsed_format != alpha_recorder::obs::FinalizationFormat::MaskHevcQsv ||
        !alpha_recorder::obs::try_parse_finalization_format("mask_hevc_vaapi", parsed_format) || parsed_format != alpha_recorder::obs::FinalizationFormat::MaskHevcVaapi)
    {
        std::cerr << "failed to parse qsv or vaapi config values\n";
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
    alpha_recorder::obs::HevcNvencSplitEncodeMode parsed_split_encode = alpha_recorder::obs::HevcNvencSplitEncodeMode::Auto;
    std::int32_t normalized_gpu_index = -1;
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
        !alpha_recorder::obs::try_parse_hevc_nvenc_split_encode("forced", parsed_split_encode) ||
        parsed_split_encode != alpha_recorder::obs::HevcNvencSplitEncodeMode::Forced ||
        !alpha_recorder::obs::try_parse_hevc_nvenc_split_encode("2", parsed_split_encode) ||
        parsed_split_encode != alpha_recorder::obs::HevcNvencSplitEncodeMode::TwoWay ||
        alpha_recorder::obs::hevc_nvenc_split_encode_config_value(alpha_recorder::obs::HevcNvencSplitEncodeMode::Disabled) != "disabled" ||
        alpha_recorder::obs::clamp_hevc_quality_cq(99U) != 51U ||
        alpha_recorder::obs::clamp_hevc_gop_size(1200U) != 1000U ||
        alpha_recorder::obs::clamp_hevc_b_frames(8U) != 4U ||
        alpha_recorder::obs::clamp_hevc_lookahead(64U) != 32U ||
        !alpha_recorder::obs::try_normalize_hevc_nvenc_gpu_index(99, normalized_gpu_index) ||
        normalized_gpu_index != 99 ||
        alpha_recorder::obs::try_normalize_hevc_nvenc_gpu_index(-2, normalized_gpu_index) ||
        alpha_recorder::obs::try_normalize_hevc_nvenc_gpu_index(
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1, normalized_gpu_index) ||
        alpha_recorder::obs::normalize_hevc_nvenc_gpu_index_from_int64(-2) != -1)
    {
        std::cerr << "hevc encoder setting helpers are incorrect\n";
        return 14;
    }

    const std::size_t mib = 1024U * 1024U;
    if (alpha_recorder::obs::alpha_mask_writer_queue_frame_limit(60U, 1U) != 60U ||
        alpha_recorder::obs::alpha_mask_writer_queue_frame_limit(30000U, 1001U) != 30U ||
        alpha_recorder::obs::alpha_mask_writer_queue_frame_limit(240U, 1U) != 120U ||
        alpha_recorder::obs::alpha_mask_writer_queue_byte_limit(1920U, 1080U, 60U, 1U) != 192U * mib ||
        alpha_recorder::obs::alpha_mask_writer_queue_byte_limit(7680U, 4320U, 60U, 1U) !=
            static_cast<std::size_t>(7680U) * static_cast<std::size_t>(4320U) * 60U ||
        alpha_recorder::obs::alpha_mask_writer_queue_byte_limit(7680U, 4320U, 120U, 1U) != 2048U * mib ||
        alpha_recorder::obs::alpha_mask_writer_queue_byte_limit(0U, 4320U, 60U, 1U) != 192U * mib)
    {
        std::cerr << "writer queue limit helpers are incorrect\n";
        return 15;
    }

    config_t *config = nullptr;
    if (config_open_string(&config, "[AlphaRecorder]\nenabled=true\nfinalization_format=mask_png_mov\nhevc_quality_profile=fast\nhevc_quality_cq=64\nhevc_preset=amf_quality\nhevc_nvenc_tune=ull\nhevc_gop_size=1200\nhevc_b_frames=8\nhevc_lookahead=64\nhevc_adaptive_quantization=true\nhevc_nvenc_split_encode=3\nhevc_nvenc_gpu_index=99\ndiagnostic_logging=true\n") != CONFIG_SUCCESS || config == nullptr)
    {
        std::cerr << "failed to open an in-memory config string\n";
        return 16;
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
        !loaded_settings.hevc_encoder.adaptive_quantization ||
        loaded_settings.hevc_encoder.nvenc_split_encode != alpha_recorder::obs::HevcNvencSplitEncodeMode::ThreeWay ||
        loaded_settings.hevc_encoder.nvenc_gpu_index != 99 ||
        !loaded_settings.diagnostic_logging)
    {
        std::cerr << "valid config values were not preserved by the loader\n";
        return 17;
    }

    if (config_open_string(&config, "[AlphaRecorder]\n") != CONFIG_SUCCESS || config == nullptr)
    {
        std::cerr << "failed to open an in-memory default config string\n";
        return 18;
    }

    const alpha_recorder::obs::Settings missing_key_settings = alpha_recorder::obs::load_settings(config);
    config_close(config);
    if (!missing_key_settings.enabled ||
        missing_key_settings.finalization_format != alpha_recorder::obs::finalization_format_default())
    {
        std::cerr << "missing settings did not use enabled and safe format defaults\n";
        return 19;
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
    config_set_string(file_config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data(), "mask_hevc_nvenc");
    if (config_save(file_config) != CONFIG_SUCCESS)
    {
        std::cerr << "failed to seed the file-backed config\n";
        config_close(file_config);
        return 41;
    }

    const alpha_recorder::obs::Settings rewritten_settings = alpha_recorder::obs::load_settings(file_config);
    const char *rewritten_format = config_get_string(file_config, alpha_recorder::obs::settings_section().data(), alpha_recorder::obs::settings_finalization_format_key().data());
    const alpha_recorder::obs::FinalizationFormat expected_rewritten_format =
        alpha_recorder::obs::FinalizationFormat::MaskHevcNvenc;
    const std::string expected_rewritten_text{alpha_recorder::obs::finalization_format_config_value(expected_rewritten_format)};
    if (!rewritten_settings.enabled || rewritten_settings.finalization_format != expected_rewritten_format || rewritten_format == nullptr || std::string{rewritten_format} != expected_rewritten_text)
    {
        std::cerr << "available hevc config values were not preserved in the persisted config\n";
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
