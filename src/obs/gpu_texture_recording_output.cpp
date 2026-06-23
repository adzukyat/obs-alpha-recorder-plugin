#include "gpu_texture_recording_output.hpp"

#include "alpha_recorder/plugin.hpp"
#include "gpu_texture_alpha_output_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <obs-module.h>

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/bmem.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace
{
    constexpr const char *kOutputName = "Alpha Recorder GPU Texture Recording";
    constexpr const char *kSourceName = "Alpha Recorder Program Alpha";
    constexpr const char *kDefaultEncoderId = "obs_nvenc_hevc_tex";

    enum class TextureHevcBackend
    {
        Unknown,
        Nvenc,
        Amf,
        Qsv,
        Vaapi,
    };

    struct GpuTextureRecordingOutputContext;

    constexpr std::string_view kProgramAlphaEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;

sampler_state def_sampler {
    Filter   = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertInOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VertInOut VSDefault(VertInOut vert_in)
{
    VertInOut vert_out;
    vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv  = vert_in.uv;
    return vert_out;
}

float4 PSAlpha(VertInOut vert_in) : TARGET
{
    float alpha = image.Sample(def_sampler, vert_in.uv).a;
    return float4(alpha, alpha, alpha, 1.0);
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PSAlpha(vert_in);
    }
}
)";

    struct ProgramAlphaSource
    {
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        gs_effect_t *effect = nullptr;
        gs_eparam_t *image_param = nullptr;
        GpuTextureRecordingOutputContext *context = nullptr;
    };

    struct GpuTextureRecordingOutputContext
    {
        obs_output_t *output = nullptr;
        obs_source_t *source = nullptr;
        obs_view_t *view = nullptr;
        obs_encoder_t *encoder = nullptr;
        obs_encoder_t *audio_encoder = nullptr;
        video_t *video = nullptr;
        alpha_recorder::obs::GpuTextureAlphaOutputSink sink{};

        std::filesystem::path path{};
        std::string encoder_id{kDefaultEncoderId};
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::uint32_t fps_num = 0U;
        std::uint32_t fps_den = 1U;
        alpha_recorder::obs::HevcEncoderSettings hevc_encoder{};

        std::mutex mutex{};
        std::vector<alpha_recorder::obs::GpuTextureRecordingTiming> alpha_packet_times{};
        std::vector<std::uint64_t> alpha_input_render_times{};
        std::vector<std::int64_t> alpha_content_cts_offset_samples{};
        bool packet_callback_connected = false;
        bool stop_finalized = false;
    };

    std::mutex g_contexts_mutex;
    std::vector<std::pair<obs_output_t *, GpuTextureRecordingOutputContext *>> g_contexts;

    void assign_error(std::string *error_message, const char *message)
    {
        if (error_message != nullptr)
        {
            *error_message = message;
        }
    }

    void assign_error(std::string *error_message, const std::string &message)
    {
        if (error_message != nullptr)
        {
            *error_message = message;
        }
    }

    TextureHevcBackend backend_for_encoder_id(std::string_view encoder_id) noexcept
    {
        if (encoder_id == "obs_nvenc_hevc_tex")
        {
            return TextureHevcBackend::Nvenc;
        }
        if (encoder_id == "h265_texture_amf")
        {
            return TextureHevcBackend::Amf;
        }
        if (encoder_id == "obs_qsv11_hevc")
        {
            return TextureHevcBackend::Qsv;
        }
        if (encoder_id == "hevc_ffmpeg_vaapi_tex")
        {
            return TextureHevcBackend::Vaapi;
        }

        return TextureHevcBackend::Unknown;
    }

    const char *backend_name(TextureHevcBackend backend) noexcept
    {
        switch (backend)
        {
        case TextureHevcBackend::Nvenc:
            return "NVENC";
        case TextureHevcBackend::Amf:
            return "AMF";
        case TextureHevcBackend::Qsv:
            return "QSV";
        case TextureHevcBackend::Vaapi:
            return "VAAPI";
        case TextureHevcBackend::Unknown:
            break;
        }

        return "unknown";
    }

    std::uint64_t abs_delta(std::uint64_t lhs, std::uint64_t rhs) noexcept
    {
        return lhs >= rhs ? lhs - rhs : rhs - lhs;
    }

    std::uint64_t abs_i64(std::int64_t value) noexcept
    {
        return value >= 0 ? static_cast<std::uint64_t>(value)
                          : static_cast<std::uint64_t>(-(value + 1)) + 1ULL;
    }

    std::int64_t signed_delta_ns(std::uint64_t lhs, std::uint64_t rhs) noexcept
    {
        const std::uint64_t delta = abs_delta(lhs, rhs);
        const std::uint64_t clamped = std::min<std::uint64_t>(
            delta, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        return lhs >= rhs ? static_cast<std::int64_t>(clamped) : -static_cast<std::int64_t>(clamped);
    }

    std::uint64_t add_signed_ns(std::uint64_t value, std::int64_t delta) noexcept
    {
        if (delta >= 0)
        {
            const auto unsigned_delta = static_cast<std::uint64_t>(delta);
            return value > std::numeric_limits<std::uint64_t>::max() - unsigned_delta
                       ? std::numeric_limits<std::uint64_t>::max()
                       : value + unsigned_delta;
        }

        const auto unsigned_delta = static_cast<std::uint64_t>(-(delta + 1)) + 1ULL;
        return unsigned_delta > value ? 0U : value - unsigned_delta;
    }

    std::uint64_t frame_interval_ns(const GpuTextureRecordingOutputContext &context) noexcept
    {
        const std::uint32_t fps_num = context.fps_num == 0U ? 60U : context.fps_num;
        const std::uint32_t fps_den = context.fps_den == 0U ? 1U : context.fps_den;
        return std::max<std::uint64_t>(
            1ULL,
            (1000000000ULL * static_cast<std::uint64_t>(fps_den)) /
                static_cast<std::uint64_t>(fps_num));
    }

    std::int64_t median_offset(std::vector<std::int64_t> samples)
    {
        if (samples.empty())
        {
            return 0;
        }

        const auto middle = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2U);
        std::nth_element(samples.begin(), middle, samples.end());
        return *middle;
    }

    void remember_alpha_input_render_time(GpuTextureRecordingOutputContext *context,
                                          std::uint64_t render_time) noexcept
    {
        if (context == nullptr || render_time == 0U)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        context->alpha_input_render_times.push_back(render_time);
    }

    GpuTextureRecordingOutputContext *find_context(obs_output_t *output) noexcept
    {
        std::lock_guard<std::mutex> lock(g_contexts_mutex);
        const auto found = std::find_if(g_contexts.begin(), g_contexts.end(),
                                        [output](const auto &entry) { return entry.first == output; });
        return found == g_contexts.end() ? nullptr : found->second;
    }

    void remember_context(obs_output_t *output, GpuTextureRecordingOutputContext *context)
    {
        std::lock_guard<std::mutex> lock(g_contexts_mutex);
        g_contexts.emplace_back(output, context);
    }

    void forget_context(obs_output_t *output)
    {
        std::lock_guard<std::mutex> lock(g_contexts_mutex);
        g_contexts.erase(std::remove_if(g_contexts.begin(), g_contexts.end(),
                                        [output](const auto &entry) { return entry.first == output; }),
                         g_contexts.end());
    }

    const char *short_nvenc_preset(alpha_recorder::obs::HevcEncoderPreset preset) noexcept
    {
        switch (preset)
        {
        case alpha_recorder::obs::HevcEncoderPreset::NvencP1:
            return "p1";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP2:
            return "p2";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP3:
            return "p3";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP4:
            return "p4";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP5:
            return "p5";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP6:
            return "p6";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP7:
            return "p7";
        case alpha_recorder::obs::HevcEncoderPreset::NvencLossless:
        case alpha_recorder::obs::HevcEncoderPreset::AmfSpeed:
        case alpha_recorder::obs::HevcEncoderPreset::AmfBalanced:
        case alpha_recorder::obs::HevcEncoderPreset::AmfQuality:
            return "p1";
        }

        return "p3";
    }

    std::uint32_t profile_cqp(const alpha_recorder::obs::HevcEncoderSettings &settings) noexcept
    {
        if (settings.quality_cq <= 51U)
        {
            return settings.quality_cq;
        }

        switch (settings.quality_profile)
        {
        case alpha_recorder::obs::HevcQualityProfile::Lossless:
            return 0U;
        case alpha_recorder::obs::HevcQualityProfile::HighQuality:
            return 19U;
        case alpha_recorder::obs::HevcQualityProfile::Balanced:
            return 23U;
        case alpha_recorder::obs::HevcQualityProfile::Fast:
            return 28U;
        }

        return 19U;
    }

    int split_encode_value(alpha_recorder::obs::HevcNvencSplitEncodeMode mode) noexcept
    {
        switch (mode)
        {
        case alpha_recorder::obs::HevcNvencSplitEncodeMode::Auto:
            return 0;
        case alpha_recorder::obs::HevcNvencSplitEncodeMode::Disabled:
            return 15;
        case alpha_recorder::obs::HevcNvencSplitEncodeMode::Forced:
            return 1;
        case alpha_recorder::obs::HevcNvencSplitEncodeMode::TwoWay:
            return 2;
        case alpha_recorder::obs::HevcNvencSplitEncodeMode::ThreeWay:
            return 3;
        }

        return 0;
    }

    std::uint32_t configured_gop_frames(const GpuTextureRecordingOutputContext &context) noexcept
    {
        const std::uint32_t requested = alpha_recorder::obs::clamp_hevc_gop_size(context.hevc_encoder.gop_size);
        if (requested != 0U)
        {
            return requested;
        }

        return context.fps_num == 0U ? 30U : context.fps_num;
    }

    std::uint32_t configured_keyint_seconds(const GpuTextureRecordingOutputContext &context) noexcept
    {
        const std::uint32_t fps_num = context.fps_num == 0U ? 30U : context.fps_num;
        const std::uint32_t fps_den = context.fps_den == 0U ? 1U : context.fps_den;
        const std::uint32_t gop_frames = configured_gop_frames(context);
        if (fps_num == 0U)
        {
            return 0U;
        }

        const std::uint64_t numerator =
            static_cast<std::uint64_t>(gop_frames) * static_cast<std::uint64_t>(fps_den);
        const std::uint64_t rounded = (numerator + (fps_num / 2U)) / fps_num;
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(std::max<std::uint64_t>(1U, rounded), 10U));
    }

    const char *amf_preset_value(alpha_recorder::obs::HevcEncoderPreset preset) noexcept
    {
        switch (preset)
        {
        case alpha_recorder::obs::HevcEncoderPreset::AmfSpeed:
            return "speed";
        case alpha_recorder::obs::HevcEncoderPreset::AmfBalanced:
            return "balanced";
        case alpha_recorder::obs::HevcEncoderPreset::AmfQuality:
            return "quality";
        case alpha_recorder::obs::HevcEncoderPreset::NvencLossless:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP1:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP2:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP3:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP4:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP5:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP6:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP7:
            break;
        }

        return "balanced";
    }

    const char *qsv_target_usage_value(alpha_recorder::obs::HevcEncoderPreset preset) noexcept
    {
        switch (preset)
        {
        case alpha_recorder::obs::HevcEncoderPreset::NvencP1:
            return "TU7";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP2:
            return "TU6";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP3:
            return "TU5";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP4:
            return "TU4";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP5:
            return "TU3";
        case alpha_recorder::obs::HevcEncoderPreset::NvencP6:
            return "TU2";
        case alpha_recorder::obs::HevcEncoderPreset::NvencLossless:
        case alpha_recorder::obs::HevcEncoderPreset::NvencP7:
            return "TU1";
        case alpha_recorder::obs::HevcEncoderPreset::AmfSpeed:
            return "TU7";
        case alpha_recorder::obs::HevcEncoderPreset::AmfBalanced:
            return "TU4";
        case alpha_recorder::obs::HevcEncoderPreset::AmfQuality:
            return "TU1";
        }

        return "TU4";
    }

    const char *qsv_latency_value(alpha_recorder::obs::HevcNvencTune tune) noexcept
    {
        switch (tune)
        {
        case alpha_recorder::obs::HevcNvencTune::LowLatency:
            return "low";
        case alpha_recorder::obs::HevcNvencTune::UltraLowLatency:
            return "ultra-low";
        case alpha_recorder::obs::HevcNvencTune::Lossless:
        case alpha_recorder::obs::HevcNvencTune::HighQuality:
            break;
        }

        return "normal";
    }

    bool path_is_read_write_accessible(const std::filesystem::path &path) noexcept
    {
#if defined(_WIN32)
        (void)path;
        return false;
#else
        return !path.empty() && ::access(path.c_str(), R_OK | W_OK) == 0;
#endif
    }

    std::string obs_vaapi_hevc_default_device_path()
    {
        obs_data_t *defaults = obs_encoder_defaults("hevc_ffmpeg_vaapi_tex");
        if (defaults == nullptr)
        {
            return {};
        }

        std::string device = obs_data_get_string(defaults, "vaapi_device");
        obs_data_release(defaults);
        return device;
    }

    std::string obs_vaapi_hevc_property_device_path()
    {
        obs_properties_t *properties = obs_get_encoder_properties("hevc_ffmpeg_vaapi_tex");
        if (properties == nullptr)
        {
            return {};
        }

        std::string device;
        obs_property_t *device_property = obs_properties_get(properties, "vaapi_device");
        if (device_property != nullptr)
        {
            const size_t count = obs_property_list_item_count(device_property);
            for (size_t index = 0; index < count; ++index)
            {
                if (obs_property_list_item_disabled(device_property, index))
                {
                    continue;
                }

                const char *value = obs_property_list_item_string(device_property, index);
                const std::string_view path{value != nullptr ? value : ""};
                if (path.find("/dev/dri/by-path/") != std::string_view::npos)
                {
                    device = value;
                    break;
                }
            }
        }

        obs_properties_destroy(properties);
        return device;
    }

    std::string discover_vaapi_device_path()
    {
#if defined(_WIN32) || defined(__APPLE__)
        return {};
#else
        if (const char *configured = std::getenv("ALPHA_RECORDER_VAAPI_DEVICE");
            configured != nullptr && *configured != '\0' &&
            path_is_read_write_accessible(std::filesystem::path{configured}))
        {
            return configured;
        }

        if (std::string device = obs_vaapi_hevc_default_device_path();
            path_is_read_write_accessible(std::filesystem::path{device}))
        {
            return device;
        }

        if (std::string device = obs_vaapi_hevc_property_device_path();
            path_is_read_write_accessible(std::filesystem::path{device}))
        {
            return device;
        }

        return {};
#endif
    }

    void program_alpha_update(void *data, obs_data_t *settings)
    {
        auto *source = static_cast<ProgramAlphaSource *>(data);
        source->width = static_cast<std::uint32_t>(obs_data_get_int(settings, "width"));
        source->height = static_cast<std::uint32_t>(obs_data_get_int(settings, "height"));
        const long long context_ptr = obs_data_get_int(settings, "context_ptr");
        source->context = reinterpret_cast<GpuTextureRecordingOutputContext *>(
            static_cast<std::intptr_t>(context_ptr));
    }

    bool ensure_program_alpha_effect(ProgramAlphaSource &source)
    {
        if (source.effect != nullptr && source.image_param != nullptr)
        {
            return true;
        }

        char *effect_error = nullptr;
        source.effect = gs_effect_create(std::string{kProgramAlphaEffect}.c_str(),
                                         "alpha-recorder-program-alpha.effect",
                                         &effect_error);
        if (source.effect == nullptr)
        {
            blog(LOG_ERROR, "[alpha_recorder_gpu_texture] could not create program alpha shader: %s",
                 effect_error != nullptr ? effect_error : "unknown error");
            if (effect_error != nullptr)
            {
                bfree(effect_error);
            }
            return false;
        }

        source.image_param = gs_effect_get_param_by_name(source.effect, "image");
        return source.image_param != nullptr;
    }

    void *program_alpha_create(obs_data_t *settings, obs_source_t *)
    {
        auto *source = new ProgramAlphaSource{};
        program_alpha_update(source, settings);
        return source;
    }

    void program_alpha_destroy(void *data)
    {
        auto *source = static_cast<ProgramAlphaSource *>(data);
        if (source == nullptr)
        {
            return;
        }
        if (source->effect != nullptr)
        {
            gs_effect_destroy(source->effect);
            source->effect = nullptr;
        }
        delete source;
    }

    std::uint32_t program_alpha_width(void *data)
    {
        return static_cast<ProgramAlphaSource *>(data)->width;
    }

    std::uint32_t program_alpha_height(void *data)
    {
        return static_cast<ProgramAlphaSource *>(data)->height;
    }

    void program_alpha_render(void *data, gs_effect_t *)
    {
        auto *source = static_cast<ProgramAlphaSource *>(data);
        if (source == nullptr || source->width == 0U || source->height == 0U ||
            !ensure_program_alpha_effect(*source))
        {
            return;
        }

        gs_texture_t *program_texture = obs_get_main_texture();
        if (program_texture == nullptr)
        {
            return;
        }

        remember_alpha_input_render_time(source->context, obs_get_video_frame_time());

        gs_effect_set_texture(source->image_param, program_texture);
        gs_enable_blending(false);
        while (gs_effect_loop(source->effect, "Draw"))
        {
            gs_draw_sprite(program_texture, 0, source->width, source->height);
        }
        gs_enable_blending(true);
    }

    const char *program_alpha_name(void *)
    {
        return kSourceName;
    }

    void gpu_texture_recording_packet_time(obs_output_t *,
                                           encoder_packet *packet,
                                           encoder_packet_time *packet_time,
                                           void *param);

    void release_graph(GpuTextureRecordingOutputContext &context)
    {
        if (context.output != nullptr)
        {
            if (context.packet_callback_connected)
            {
                obs_output_remove_packet_callback(context.output, &gpu_texture_recording_packet_time, &context);
                context.packet_callback_connected = false;
            }
            if (!obs_output_active(context.output))
            {
                obs_output_set_video_encoder(context.output, nullptr);
                obs_output_set_audio_encoder(context.output, nullptr, 0U);
            }
        }

        if (context.audio_encoder != nullptr)
        {
            obs_encoder_release(context.audio_encoder);
            context.audio_encoder = nullptr;
        }

        if (context.encoder != nullptr)
        {
            obs_encoder_release(context.encoder);
            context.encoder = nullptr;
        }

        context.video = nullptr;

        if (context.view != nullptr)
        {
            obs_view_set_source(context.view, 0U, nullptr);
            obs_view_remove(context.view);
            obs_view_destroy(context.view);
            context.view = nullptr;
        }

        if (context.source != nullptr)
        {
            obs_source_release(context.source);
            context.source = nullptr;
        }
    }

    void gpu_texture_recording_update(void *data, obs_data_t *settings)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr || settings == nullptr)
        {
            return;
        }

        const char *path = obs_data_get_string(settings, "path");
        context->path = path != nullptr ? std::filesystem::path{path} : std::filesystem::path{};

        const char *encoder_id = obs_data_get_string(settings, "encoder_id");
        context->encoder_id = encoder_id != nullptr && *encoder_id != '\0' ? encoder_id : kDefaultEncoderId;

        const long long width = obs_data_get_int(settings, "width");
        const long long height = obs_data_get_int(settings, "height");
        const long long fps_num = obs_data_get_int(settings, "fps_num");
        const long long fps_den = obs_data_get_int(settings, "fps_den");
        context->width = width > 0 ? static_cast<std::uint32_t>(width) : 0U;
        context->height = height > 0 ? static_cast<std::uint32_t>(height) : 0U;
        context->fps_num = fps_num > 0 ? static_cast<std::uint32_t>(fps_num) : 0U;
        context->fps_den = fps_den > 0 ? static_cast<std::uint32_t>(fps_den) : 1U;

        const char *profile = obs_data_get_string(settings, "hevc_quality_profile");
        alpha_recorder::obs::HevcQualityProfile parsed_profile = context->hevc_encoder.quality_profile;
        if (profile != nullptr && alpha_recorder::obs::try_parse_hevc_quality_profile(profile, parsed_profile))
        {
            context->hevc_encoder.quality_profile = parsed_profile;
        }

        const char *preset = obs_data_get_string(settings, "hevc_preset");
        alpha_recorder::obs::HevcEncoderPreset parsed_preset = context->hevc_encoder.preset;
        if (preset != nullptr && alpha_recorder::obs::try_parse_hevc_encoder_preset(preset, parsed_preset))
        {
            context->hevc_encoder.preset = parsed_preset;
        }

        const char *tune = obs_data_get_string(settings, "hevc_nvenc_tune");
        alpha_recorder::obs::HevcNvencTune parsed_tune = context->hevc_encoder.nvenc_tune;
        if (tune != nullptr && alpha_recorder::obs::try_parse_hevc_nvenc_tune(tune, parsed_tune))
        {
            context->hevc_encoder.nvenc_tune = parsed_tune;
        }

        const char *split = obs_data_get_string(settings, "hevc_nvenc_split_encode");
        alpha_recorder::obs::HevcNvencSplitEncodeMode parsed_split = context->hevc_encoder.nvenc_split_encode;
        if (split != nullptr && alpha_recorder::obs::try_parse_hevc_nvenc_split_encode(split, parsed_split))
        {
            context->hevc_encoder.nvenc_split_encode = parsed_split;
        }

        context->hevc_encoder.quality_cq =
            alpha_recorder::obs::clamp_hevc_quality_cq(static_cast<std::uint32_t>(std::max<long long>(
                0, obs_data_get_int(settings, "hevc_quality_cq"))));
        context->hevc_encoder.gop_size =
            alpha_recorder::obs::clamp_hevc_gop_size(static_cast<std::uint32_t>(std::max<long long>(
                0, obs_data_get_int(settings, "hevc_gop_size"))));
        context->hevc_encoder.b_frames =
            alpha_recorder::obs::clamp_hevc_b_frames(static_cast<std::uint32_t>(std::max<long long>(
                0, obs_data_get_int(settings, "hevc_b_frames"))));
        context->hevc_encoder.lookahead =
            alpha_recorder::obs::clamp_hevc_lookahead(static_cast<std::uint32_t>(std::max<long long>(
                0, obs_data_get_int(settings, "hevc_lookahead"))));
        context->hevc_encoder.adaptive_quantization =
            obs_data_get_bool(settings, "hevc_adaptive_quantization");
        context->hevc_encoder.nvenc_gpu_index =
            alpha_recorder::obs::normalize_hevc_nvenc_gpu_index_from_int64(
                obs_data_get_int(settings, "hevc_nvenc_gpu_index"));
    }

    void *gpu_texture_recording_create(obs_data_t *settings, obs_output_t *output)
    {
        auto *context = new GpuTextureRecordingOutputContext{};
        context->output = output;
        gpu_texture_recording_update(context, settings);
        remember_context(output, context);
        return context;
    }

    void gpu_texture_recording_destroy(void *data)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr)
        {
            return;
        }

        if (context->output != nullptr)
        {
            forget_context(context->output);
        }
        context->sink.abort();
        release_graph(*context);
        delete context;
    }

    bool ensure_output_directory(const std::filesystem::path &path, std::string *error_message)
    {
        if (path.empty())
        {
            assign_error(error_message, "Alpha Recorder GPU texture output path is empty");
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                assign_error(error_message, "Alpha Recorder GPU texture output directory could not be created");
                return false;
            }
        }

        return true;
    }

    obs_data_t *make_encoder_settings(const GpuTextureRecordingOutputContext &context)
    {
        obs_data_t *settings = obs_data_create();
        const TextureHevcBackend backend = backend_for_encoder_id(context.encoder_id);
        const bool lossless =
            context.hevc_encoder.quality_profile == alpha_recorder::obs::HevcQualityProfile::Lossless;
        const std::uint32_t gop_frames = configured_gop_frames(context);
        const std::uint32_t keyint_sec = configured_keyint_seconds(context);
        const std::uint32_t cqp = profile_cqp(context.hevc_encoder);
        const std::uint32_t b_frames = lossless ? 0U : alpha_recorder::obs::clamp_hevc_b_frames(
                                                            context.hevc_encoder.b_frames);

        obs_data_set_int(settings, "bitrate", 40000);
        obs_data_set_int(settings, "max_bitrate", 40000);

        switch (backend)
        {
        case TextureHevcBackend::Nvenc:
            obs_data_set_string(settings, "rate_control", lossless ? "lossless" : "CQP");
            obs_data_set_int(settings, "cqp", static_cast<long long>(cqp));
            obs_data_set_int(settings, "keyint_sec", 0);
            obs_data_set_int(settings, "bf", static_cast<long long>(b_frames));
            obs_data_set_string(settings, "preset", short_nvenc_preset(context.hevc_encoder.preset));
            obs_data_set_string(settings, "preset2", short_nvenc_preset(context.hevc_encoder.preset));
            obs_data_set_string(settings, "tune",
                                alpha_recorder::obs::hevc_nvenc_tune_config_value(
                                    context.hevc_encoder.nvenc_tune)
                                    .data());
            obs_data_set_string(settings, "multipass", "disabled");
            obs_data_set_bool(settings, "lookahead", context.hevc_encoder.lookahead > 0U);
            obs_data_set_bool(settings, "adaptive_quantization", context.hevc_encoder.adaptive_quantization);
            obs_data_set_bool(settings, "repeat_headers", true);
            obs_data_set_int(settings, "split_encode", split_encode_value(context.hevc_encoder.nvenc_split_encode));
            if (context.hevc_encoder.nvenc_gpu_index >= 0)
            {
                obs_data_set_int(settings, "gpu", context.hevc_encoder.nvenc_gpu_index);
                obs_data_set_int(settings, "device", context.hevc_encoder.nvenc_gpu_index);
            }
            break;

        case TextureHevcBackend::Amf:
            obs_data_set_string(settings, "rate_control", "CQP");
            obs_data_set_int(settings, "cqp", static_cast<long long>(cqp));
            obs_data_set_int(settings, "keyint_sec", static_cast<long long>(keyint_sec));
            obs_data_set_int(settings, "bf", static_cast<long long>(b_frames));
            obs_data_set_string(settings, "preset", amf_preset_value(context.hevc_encoder.preset));
            obs_data_set_string(settings, "profile", "main");
            obs_data_set_bool(settings, "pre_analysis", context.hevc_encoder.lookahead > 0U);
            obs_data_set_bool(settings, "repeat_headers", true);
            break;

        case TextureHevcBackend::Qsv:
            obs_data_set_string(settings, "rate_control", "CQP");
            obs_data_set_string(settings, "target_usage", qsv_target_usage_value(context.hevc_encoder.preset));
            obs_data_set_string(settings, "profile", "main");
            obs_data_set_string(settings, "latency", qsv_latency_value(context.hevc_encoder.nvenc_tune));
            obs_data_set_int(settings, "keyint_sec", static_cast<long long>(keyint_sec));
            obs_data_set_int(settings, "bframes", static_cast<long long>(b_frames));
            obs_data_set_int(settings, "bf", static_cast<long long>(b_frames));
            obs_data_set_int(settings, "cqp", static_cast<long long>(cqp));
            obs_data_set_int(settings, "qpi", static_cast<long long>(cqp));
            obs_data_set_int(settings, "qpp", static_cast<long long>(cqp));
            obs_data_set_int(settings, "qpb", static_cast<long long>(cqp));
            obs_data_set_int(settings, "icq_quality", static_cast<long long>(cqp));
            obs_data_set_bool(settings, "repeat_headers", true);
            break;

        case TextureHevcBackend::Vaapi:
            obs_data_set_string(settings, "vaapi_device", discover_vaapi_device_path().c_str());
            obs_data_set_string(settings, "rate_control", "CQP");
            obs_data_set_int(settings, "profile", 1);
            obs_data_set_int(settings, "level", -99);
            obs_data_set_int(settings, "qp", static_cast<long long>(cqp));
            obs_data_set_int(settings, "keyint_sec", static_cast<long long>(keyint_sec));
            obs_data_set_int(settings, "bf", static_cast<long long>(b_frames));
            obs_data_set_int(settings, "maxrate", 0);
            break;

        case TextureHevcBackend::Unknown:
            obs_data_set_string(settings, "rate_control", "CQP");
            obs_data_set_int(settings, "cqp", static_cast<long long>(cqp));
            obs_data_set_int(settings, "keyint_sec", static_cast<long long>(keyint_sec));
            obs_data_set_int(settings, "bf", static_cast<long long>(b_frames));
            break;
        }

        const std::string opts = "keyint=" + std::to_string(gop_frames);
        obs_data_set_string(settings, "opts", opts.c_str());
        obs_data_set_string(settings, "ffmpeg_opts", opts.c_str());
        return settings;
    }

    obs_encoder_t *create_timing_audio_encoder()
    {
        obs_data_t *settings = obs_data_create();
        obs_data_set_int(settings, "bitrate", 32);
        obs_encoder_t *encoder = obs_audio_encoder_create("ffmpeg_aac",
                                                          "Alpha Recorder GPU Texture Timing Audio Encoder",
                                                          settings,
                                                          0U,
                                                          nullptr);
        if (encoder == nullptr)
        {
            encoder = obs_audio_encoder_create("CoreAudio_AAC",
                                               "Alpha Recorder GPU Texture Timing Audio Encoder",
                                               settings,
                                               0U,
                                               nullptr);
        }
        obs_data_release(settings);
        return encoder;
    }

    bool setup_graph(GpuTextureRecordingOutputContext &context, std::string *error_message)
    {
        obs_video_info main_video_info = {};
        if (!obs_get_video_info(&main_video_info))
        {
            assign_error(error_message, "Alpha Recorder GPU texture output could not read OBS video info");
            return false;
        }

        const std::uint32_t width = context.width != 0U ? context.width : main_video_info.output_width;
        const std::uint32_t height = context.height != 0U ? context.height : main_video_info.output_height;
        if (width == 0U || height == 0U)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output received an invalid video size");
            return false;
        }

        obs_data_t *source_settings = obs_data_create();
        obs_data_set_int(source_settings, "width", width);
        obs_data_set_int(source_settings, "height", height);
        obs_data_set_int(source_settings, "context_ptr",
                         static_cast<long long>(reinterpret_cast<std::intptr_t>(&context)));
        context.source = obs_source_create(alpha_recorder::obs::gpu_texture_program_alpha_source_id(),
                                           kSourceName,
                                           source_settings,
                                           nullptr);
        obs_data_release(source_settings);
        if (context.source == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output could not create the program alpha source");
            return false;
        }

        context.view = obs_view_create();
        if (context.view == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output could not create an OBS view");
            return false;
        }
        obs_view_set_source(context.view, 0U, context.source);

        obs_video_info alpha_video_info = main_video_info;
        alpha_video_info.base_width = width;
        alpha_video_info.base_height = height;
        alpha_video_info.output_width = width;
        alpha_video_info.output_height = height;
        alpha_video_info.output_format = VIDEO_FORMAT_NV12;
        alpha_video_info.gpu_conversion = true;
        alpha_video_info.colorspace = VIDEO_CS_709;
        alpha_video_info.range = VIDEO_RANGE_FULL;
        alpha_video_info.scale_type = OBS_SCALE_BICUBIC;
        if (context.fps_num != 0U)
        {
            alpha_video_info.fps_num = context.fps_num;
            alpha_video_info.fps_den = context.fps_den == 0U ? 1U : context.fps_den;
        }

        context.video = obs_view_add2(context.view, &alpha_video_info);
        if (context.video == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output could not create an aux video mix");
            return false;
        }

        obs_data_t *encoder_settings = make_encoder_settings(context);
        context.encoder = obs_video_encoder_create(context.encoder_id.c_str(),
                                                   "Alpha Recorder GPU Texture Encoder",
                                                   encoder_settings,
                                                   nullptr);
        obs_data_release(encoder_settings);
        if (context.encoder == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output could not create the texture encoder");
            return false;
        }

        const std::uint32_t encoder_caps = obs_encoder_get_caps(context.encoder);
        if ((encoder_caps & OBS_ENCODER_CAP_PASS_TEXTURE) == 0U)
        {
            assign_error(error_message, "Alpha Recorder GPU texture encoder does not support texture input");
            return false;
        }

        obs_encoder_set_video(context.encoder, context.video);
        obs_output_set_video_encoder(context.output, context.encoder);

        context.audio_encoder = create_timing_audio_encoder();
        if (context.audio_encoder == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output could not create the timing audio encoder");
            return false;
        }
        audio_t *audio = obs_get_audio();
        if (audio == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output could not access OBS audio for timing");
            return false;
        }
        obs_encoder_set_audio(context.audio_encoder, audio);
        obs_output_set_audio_encoder(context.output, context.audio_encoder, 0U);
        return true;
    }

    void gpu_texture_recording_packet_time(obs_output_t *,
                                           encoder_packet *packet,
                                           encoder_packet_time *packet_time,
                                           void *param)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(param);
        if (context == nullptr || packet == nullptr || packet->type != OBS_ENCODER_VIDEO ||
            packet_time == nullptr || packet_time->cts == 0U)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        if (packet->pts >= 0)
        {
            const auto input_index = static_cast<std::uint64_t>(packet->pts);
            if (input_index < context->alpha_input_render_times.size())
            {
                const std::uint64_t render_time =
                    context->alpha_input_render_times[static_cast<std::size_t>(input_index)];
                const std::int64_t sample = signed_delta_ns(render_time, packet_time->cts);
                const std::uint64_t plausible_offset = frame_interval_ns(*context) * 4ULL;
                if (abs_i64(sample) <= plausible_offset &&
                    context->alpha_content_cts_offset_samples.size() < 240U)
                {
                    context->alpha_content_cts_offset_samples.push_back(sample);
                }
            }
        }
        context->alpha_packet_times.push_back(
            alpha_recorder::obs::GpuTextureRecordingTiming{packet->pts, packet_time->cts});
    }

    bool gpu_texture_recording_start(void *data)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr || context->output == nullptr)
        {
            return false;
        }

        context->sink.abort();
        release_graph(*context);
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->alpha_packet_times.clear();
            context->alpha_input_render_times.clear();
            context->alpha_content_cts_offset_samples.clear();
        }
        context->stop_finalized = false;

        std::string error_message;
        if (!ensure_output_directory(context->path, &error_message) ||
            !context->sink.open(alpha_recorder::obs::GpuTextureAlphaOutputSinkConfig{context->path},
                                &error_message) ||
            !setup_graph(*context, &error_message))
        {
            obs_output_set_last_error(context->output, error_message.c_str());
            context->sink.abort();
            release_graph(*context);
            return false;
        }

        if (!obs_output_can_begin_data_capture(context->output, 0))
        {
            obs_output_set_last_error(context->output,
                                      "Alpha Recorder GPU texture output cannot begin data capture");
            context->sink.abort();
            release_graph(*context);
            return false;
        }

        if (!obs_output_initialize_encoders(context->output, 0))
        {
            obs_output_set_last_error(context->output,
                                      "Alpha Recorder GPU texture output could not initialize the texture encoder");
            context->sink.abort();
            release_graph(*context);
            return false;
        }

        if (!context->sink.begin_mux(context->output, &error_message))
        {
            obs_output_set_last_error(context->output, error_message.c_str());
            context->sink.abort();
            release_graph(*context);
            return false;
        }

        obs_output_add_packet_callback(context->output, &gpu_texture_recording_packet_time, context);
        context->packet_callback_connected = true;
        if (!obs_output_begin_data_capture(context->output, 0))
        {
            obs_output_remove_packet_callback(context->output, &gpu_texture_recording_packet_time, context);
            context->packet_callback_connected = false;
            obs_output_set_last_error(context->output,
                                      "Alpha Recorder GPU texture output could not begin data capture");
            context->sink.abort();
            release_graph(*context);
            return false;
        }

        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] started path=\"%s\" encoder=%s backend=%s size=%ux%u fps=%u/%u cqp=%u gop=%u b_frames=%u preset=%s tune=%s split=%s gpu=%d",
             context->path.generic_string().c_str(),
             context->encoder_id.c_str(),
             backend_name(backend_for_encoder_id(context->encoder_id)),
             context->width,
             context->height,
             context->fps_num,
             context->fps_den,
             profile_cqp(context->hevc_encoder),
             configured_gop_frames(*context),
             context->hevc_encoder.b_frames,
             short_nvenc_preset(context->hevc_encoder.preset),
             alpha_recorder::obs::hevc_nvenc_tune_config_value(context->hevc_encoder.nvenc_tune).data(),
             alpha_recorder::obs::hevc_nvenc_split_encode_config_value(
                 context->hevc_encoder.nvenc_split_encode)
                 .data(),
             static_cast<int>(context->hevc_encoder.nvenc_gpu_index));
        obs_output_set_last_error(context->output, nullptr);
        return true;
    }

    void gpu_texture_recording_stop(void *data, std::uint64_t)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr || context->output == nullptr)
        {
            return;
        }

        if (context->stop_finalized)
        {
            obs_output_end_data_capture(context->output);
            return;
        }

        if (context->packet_callback_connected)
        {
            obs_output_remove_packet_callback(context->output, &gpu_texture_recording_packet_time, context);
            context->packet_callback_connected = false;
        }

        std::string error_message;
        if (!context->sink.finalize(&error_message) && !error_message.empty())
        {
            obs_output_set_last_error(context->output, error_message.c_str());
            blog(LOG_ERROR, "[alpha_recorder_gpu_texture] %s", error_message.c_str());
        }

        obs_output_end_data_capture(context->output);
        context->sink.close_storage();

        const alpha_recorder::obs::GpuTextureAlphaOutputSinkStats &stats = context->sink.stats();
        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] stopped packets=%llu keyframes=%llu packet_bytes=%llu muxed_packets=%llu finalized=%s first_pts=%lld last_pts=%lld path=\"%s\"",
             static_cast<unsigned long long>(stats.packet_count),
             static_cast<unsigned long long>(stats.keyframe_count),
             static_cast<unsigned long long>(stats.packet_bytes),
             static_cast<unsigned long long>(stats.muxed_packet_count),
             stats.finalized ? "true" : "false",
             static_cast<long long>(stats.first_pts),
             static_cast<long long>(stats.last_pts),
             context->path.generic_string().c_str());

        context->stop_finalized = true;
    }

    void gpu_texture_recording_packet(void *data, encoder_packet *packet)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr || packet == nullptr || packet->type != OBS_ENCODER_VIDEO)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        std::string error_message;
        if (!context->sink.submit_packet(packet, &error_message) && !error_message.empty())
        {
            obs_output_set_last_error(context->output, error_message.c_str());
            blog(LOG_ERROR, "[alpha_recorder_gpu_texture] %s", error_message.c_str());
        }
    }

    std::uint64_t gpu_texture_recording_total_bytes(void *data)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        return context == nullptr ? 0U : context->sink.stats().packet_bytes;
    }

} // namespace

namespace alpha_recorder::obs
{
    const char *gpu_texture_hevc_encoder_id_for_format(FinalizationFormat format) noexcept
    {
        switch (format)
        {
        case FinalizationFormat::MaskHevcNvenc:
            return "obs_nvenc_hevc_tex";
        case FinalizationFormat::MaskHevcAmf:
            return "h265_texture_amf";
        case FinalizationFormat::MaskHevcQsv:
            return "obs_qsv11_hevc";
        case FinalizationFormat::MaskHevcVaapi:
            return "hevc_ffmpeg_vaapi_tex";
        case FinalizationFormat::MaskPngMov:
            break;
        }

        return nullptr;
    }

    bool finalization_format_uses_gpu_texture_path(FinalizationFormat format) noexcept
    {
        return gpu_texture_hevc_encoder_id_for_format(format) != nullptr;
    }

    bool gpu_texture_hevc_encoder_runtime_available(FinalizationFormat format,
                                                    std::string *reason) noexcept
    {
        const char *encoder_id = gpu_texture_hevc_encoder_id_for_format(format);
        if (encoder_id == nullptr)
        {
            assign_error(reason, "this finalization format does not use an OBS GPU texture encoder");
            return false;
        }

        if (obs_get_encoder_type(encoder_id) != OBS_ENCODER_VIDEO)
        {
            assign_error(reason, std::string{finalization_format_display_name(format)} +
                                     " is not available because OBS did not register encoder '" +
                                     encoder_id + "'");
            return false;
        }

        const char *codec = obs_get_encoder_codec(encoder_id);
        if (codec == nullptr || std::string_view{codec} != "hevc")
        {
            assign_error(reason, std::string{finalization_format_display_name(format)} +
                                     " is not available because encoder '" + encoder_id +
                                     "' is not an HEVC encoder");
            return false;
        }

        const std::uint32_t caps = obs_get_encoder_caps(encoder_id);
        if ((caps & OBS_ENCODER_CAP_PASS_TEXTURE) == 0U)
        {
            assign_error(reason, std::string{finalization_format_display_name(format)} +
                                     " is not available because encoder '" + encoder_id +
                                     "' does not accept OBS texture input");
            return false;
        }

        if (format == FinalizationFormat::MaskHevcVaapi)
        {
            const std::string device = discover_vaapi_device_path();
            if (device.empty())
            {
                assign_error(reason,
                             "HEVC VAAPI Mask is not available because no readable/writable /dev/dri VAAPI "
                             "device was found. On WSL, try loading vgem and ensure the distro can run "
                             "`LIBVA_DRIVER_NAME=d3d12 vainfo --display drm --device /dev/dri/card0`; if OBS "
                             "does not expose a HEVC-capable render node, set ALPHA_RECORDER_VAAPI_DEVICE "
                             "to the working device path.");
                return false;
            }
        }

        if (reason != nullptr)
        {
            reason->clear();
        }
        return true;
    }

    bool register_gpu_texture_recording_output() noexcept
    {
        obs_source_info source_info = {};
        source_info.id = gpu_texture_program_alpha_source_id();
        source_info.type = OBS_SOURCE_TYPE_INPUT;
        source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
        source_info.get_name = program_alpha_name;
        source_info.create = program_alpha_create;
        source_info.destroy = program_alpha_destroy;
        source_info.update = program_alpha_update;
        source_info.get_width = program_alpha_width;
        source_info.get_height = program_alpha_height;
        source_info.video_render = program_alpha_render;
        obs_register_source(&source_info);

        obs_output_info output_info = {};
        output_info.id = gpu_texture_recording_output_id();
        output_info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO | OBS_OUTPUT_ENCODED;
        output_info.encoded_video_codecs = "hevc";
        output_info.encoded_audio_codecs = "aac";
        output_info.get_name = [](void *) { return kOutputName; };
        output_info.create = gpu_texture_recording_create;
        output_info.destroy = gpu_texture_recording_destroy;
        output_info.update = gpu_texture_recording_update;
        output_info.start = gpu_texture_recording_start;
        output_info.stop = gpu_texture_recording_stop;
        output_info.encoded_packet = gpu_texture_recording_packet;
        output_info.get_total_bytes = gpu_texture_recording_total_bytes;
        obs_register_output(&output_info);
        return true;
    }

    void unregister_gpu_texture_recording_output() noexcept
    {
    }

    bool gpu_texture_recording_output_set_visible_range(obs_output_t *output,
                                                        const AlphaVisiblePacketRange &range,
                                                        std::string *error_message) noexcept
    {
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output context is unavailable");
            return false;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        return context->sink.set_visible_range(range, error_message);
    }

    bool gpu_texture_recording_output_compute_visible_range(obs_output_t *output,
                                                            std::uint64_t main_first_packet_cts,
                                                            std::uint64_t main_packet_count,
                                                            bool main_texture_encoded,
                                                            AlphaVisiblePacketRange &range,
                                                            std::string *error_message) noexcept
    {
        range = {};
        if (main_first_packet_cts == 0U || main_packet_count == 0U)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output cannot compute an edit range without main recording packet timing");
            return false;
        }

        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output context is unavailable");
            return false;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->alpha_packet_times.empty())
        {
            assign_error(error_message, "Alpha Recorder GPU texture output did not receive alpha packet timing");
            return false;
        }

        std::vector<std::int64_t> content_offset_samples = context->alpha_content_cts_offset_samples;
        const std::uint64_t frame_interval = frame_interval_ns(*context);
        const std::int64_t expected_texture_offset = static_cast<std::int64_t>(std::min<std::uint64_t>(
            frame_interval, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
        std::int64_t first_content_offset_candidate = 0;
        std::uint64_t content_offset_candidates = 0U;
        std::size_t selected_render_bias = 0U;

        const auto collect_samples = [&](std::size_t render_bias,
                                         std::int64_t &first_candidate,
                                         std::uint64_t &candidates) {
            std::vector<std::int64_t> samples;
            first_candidate = 0;
            candidates = 0U;
            for (const GpuTextureRecordingTiming &timing : context->alpha_packet_times)
            {
                if (timing.pts < 0)
                {
                    continue;
                }
                const auto input_index = static_cast<std::uint64_t>(timing.pts) +
                                         static_cast<std::uint64_t>(render_bias);
                if (input_index >= context->alpha_input_render_times.size())
                {
                    continue;
                }
                const std::uint64_t render_time =
                    context->alpha_input_render_times[static_cast<std::size_t>(input_index)];
                const std::int64_t sample = signed_delta_ns(render_time, timing.cts);
                if (candidates == 0U)
                {
                    first_candidate = sample;
                }
                ++candidates;
                if (abs_i64(sample) <= frame_interval * 16ULL)
                {
                    samples.push_back(sample);
                }
            }
            return samples;
        };

        std::uint64_t best_bias_error = std::numeric_limits<std::uint64_t>::max();
        const std::size_t max_render_bias =
            std::min<std::size_t>(30U, context->alpha_input_render_times.size());
        for (std::size_t render_bias = 0U; render_bias <= max_render_bias; ++render_bias)
        {
            std::int64_t first_candidate = 0;
            std::uint64_t candidates = 0U;
            std::vector<std::int64_t> samples = collect_samples(render_bias, first_candidate, candidates);
            if (samples.empty())
            {
                continue;
            }
            const std::int64_t candidate_median = median_offset(samples);
            const std::uint64_t error = abs_i64(candidate_median - expected_texture_offset);
            if (error < best_bias_error)
            {
                best_bias_error = error;
                selected_render_bias = render_bias;
                first_content_offset_candidate = first_candidate;
                content_offset_candidates = candidates;
                content_offset_samples = std::move(samples);
            }
        }

        if (best_bias_error > frame_interval * 4ULL)
        {
            std::int64_t first_candidate = 0;
            std::uint64_t candidates = 0U;
            std::vector<std::int64_t> samples = collect_samples(0U, first_candidate, candidates);
            if (!samples.empty())
            {
                selected_render_bias = 0U;
                first_content_offset_candidate = first_candidate;
                content_offset_candidates = candidates;
                content_offset_samples = std::move(samples);
            }
        }

        const std::int64_t measured_alpha_content_cts_offset =
            median_offset(content_offset_samples);
        const bool use_packet_cts_for_alignment =
            !main_texture_encoded && selected_render_bias > 4U;
        const std::int64_t alpha_content_cts_offset =
            use_packet_cts_for_alignment ? 0 : measured_alpha_content_cts_offset;
        const std::uint64_t target_content_cts =
            add_signed_ns(main_first_packet_cts, main_texture_encoded ? alpha_content_cts_offset : 0);

        const auto best = std::min_element(
            context->alpha_packet_times.begin(),
            context->alpha_packet_times.end(),
            [target_content_cts, alpha_content_cts_offset](const GpuTextureRecordingTiming &lhs,
                                                           const GpuTextureRecordingTiming &rhs) {
                const std::uint64_t lhs_content_cts = add_signed_ns(lhs.cts, alpha_content_cts_offset);
                const std::uint64_t rhs_content_cts = add_signed_ns(rhs.cts, alpha_content_cts_offset);
                const std::uint64_t lhs_delta = abs_delta(lhs_content_cts, target_content_cts);
                const std::uint64_t rhs_delta = abs_delta(rhs_content_cts, target_content_cts);
                if (lhs_delta != rhs_delta)
                {
                    return lhs_delta < rhs_delta;
                }
                return lhs.pts < rhs.pts;
            });
        if (best == context->alpha_packet_times.end() || best->pts < 0 ||
            static_cast<std::uint64_t>(best->pts) >= context->alpha_packet_times.size() ||
            main_packet_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            assign_error(error_message, "Alpha Recorder GPU texture output computed an invalid edit range");
            return false;
        }

        range.media_time = best->pts;
        const std::uint64_t available_alpha_packets =
            static_cast<std::uint64_t>(context->alpha_packet_times.size()) -
            static_cast<std::uint64_t>(best->pts);
        range.duration = static_cast<std::int64_t>(std::min(main_packet_count, available_alpha_packets));
        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] dynamic edit range media_time=%lld duration=%lld main_first_cts=%llu target_content_cts=%llu alpha_cts=%llu alpha_content_cts=%llu delta_ns=%llu alpha_content_offset_ns=%lld measured_alpha_content_offset_ns=%lld alpha_render_bias=%llu alpha_offset_samples=%llu alpha_offset_candidates=%llu first_alpha_offset_candidate_ns=%lld alpha_input_renders=%llu alpha_timings=%llu main_texture=%s packet_cts_alignment=%s",
             static_cast<long long>(range.media_time),
             static_cast<long long>(range.duration),
             static_cast<unsigned long long>(main_first_packet_cts),
             static_cast<unsigned long long>(target_content_cts),
             static_cast<unsigned long long>(best->cts),
             static_cast<unsigned long long>(add_signed_ns(best->cts, alpha_content_cts_offset)),
             static_cast<unsigned long long>(abs_delta(add_signed_ns(best->cts, alpha_content_cts_offset),
                                                       target_content_cts)),
             static_cast<long long>(alpha_content_cts_offset),
             static_cast<long long>(measured_alpha_content_cts_offset),
             static_cast<unsigned long long>(selected_render_bias),
             static_cast<unsigned long long>(content_offset_samples.size()),
             static_cast<unsigned long long>(content_offset_candidates),
             static_cast<long long>(first_content_offset_candidate),
             static_cast<unsigned long long>(context->alpha_input_render_times.size()),
             static_cast<unsigned long long>(context->alpha_packet_times.size()),
             main_texture_encoded ? "true" : "false",
             use_packet_cts_for_alignment ? "true" : "false");
        return true;
    }

    GpuTextureRecordingOutputStats gpu_texture_recording_output_stats(obs_output_t *output) noexcept
    {
        GpuTextureRecordingOutputStats result{};
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            return result;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        const GpuTextureAlphaOutputSinkStats &stats = context->sink.stats();
        result.packet_count = stats.packet_count;
        result.keyframe_count = stats.keyframe_count;
        result.packet_bytes = stats.packet_bytes;
        result.muxed_packet_count = stats.muxed_packet_count;
        result.first_pts = stats.first_pts;
        result.last_pts = stats.last_pts;
        result.finalized = stats.finalized;
        return result;
    }

} // namespace alpha_recorder::obs
