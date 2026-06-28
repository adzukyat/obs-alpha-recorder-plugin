#include "gpu_texture_recording_output.hpp"

#include "alpha_recorder/plugin.hpp"
#include "gpu_texture_alpha_output_sink.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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
    constexpr std::size_t kPhaseTextureCount = 2U;

    enum class TextureHevcBackend
    {
        Unknown,
        Nvenc,
        Amf,
        Qsv,
        Vaapi,
    };

    enum class GpuTextureOutputStopState
    {
        Idle,
        Running,
        StopRequested,
        StopBoundaryReached,
        EndDataCaptureRequested,
        OutputDeactivated,
        MuxFinalized,
        Aborted,
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

float4 PSRed(VertInOut vert_in) : TARGET
{
    float alpha = image.Sample(def_sampler, vert_in.uv).r;
    return float4(alpha, alpha, alpha, 1.0);
}

technique DrawAlpha
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PSAlpha(vert_in);
    }
}

technique DrawRed
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PSRed(vert_in);
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
        std::array<gs_texture_t *, kPhaseTextureCount> alpha_textures{};
        std::array<std::uint64_t, kPhaseTextureCount> alpha_texture_generations{};
        std::array<bool, kPhaseTextureCount> alpha_texture_valid{};
        std::uint32_t write_texture_index = 0U;
        std::uint64_t next_generation = 0U;
    };

    struct GpuTextureRecordingOutputContext
    {
        obs_output_t *output = nullptr;
        obs_source_t *source = nullptr;
        obs_view_t *view = nullptr;
        obs_encoder_t *encoder = nullptr;
        video_t *video = nullptr;
        alpha_recorder::obs::GpuTextureAlphaOutputSink sink{};

        std::filesystem::path path{};
        std::string encoder_id{kDefaultEncoderId};
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::uint32_t fps_num = 0U;
        std::uint32_t fps_den = 1U;
        alpha_recorder::obs::HevcEncoderSettings hevc_encoder{};
        alpha_recorder::obs::MainContentPhase main_phase =
            alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;

        std::mutex mutex{};
        std::condition_variable state_cv{};
        std::vector<alpha_recorder::obs::GpuTexturePacketRecord> alpha_packet_records{};
        std::vector<alpha_recorder::obs::ProgramRenderRecord> alpha_render_records{};
        std::uint32_t start_total_frames = 0U;
        std::uint32_t start_lagged_frames = 0U;
        std::uint32_t stop_total_frames = 0U;
        std::uint32_t stop_lagged_frames = 0U;
        bool has_stop_frame_counters = false;
        bool packet_callback_connected = false;
        bool deactivate_signal_connected = false;
        bool stop_finalized = false;
        GpuTextureOutputStopState stop_state = GpuTextureOutputStopState::Idle;
        std::uint64_t packets_at_stop_request = 0U;
        std::uint64_t packets_at_stop_boundary = 0U;
        std::uint64_t stop_ts_ns = 0U;
        bool has_stop_ts = false;
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

    const char *stop_state_name(GpuTextureOutputStopState state) noexcept
    {
        switch (state)
        {
        case GpuTextureOutputStopState::Idle:
            return "Idle";
        case GpuTextureOutputStopState::Running:
            return "Running";
        case GpuTextureOutputStopState::StopRequested:
            return "StopRequested";
        case GpuTextureOutputStopState::StopBoundaryReached:
            return "StopBoundaryReached";
        case GpuTextureOutputStopState::EndDataCaptureRequested:
            return "EndDataCaptureRequested";
        case GpuTextureOutputStopState::OutputDeactivated:
            return "OutputDeactivated";
        case GpuTextureOutputStopState::MuxFinalized:
            return "MuxFinalized";
        case GpuTextureOutputStopState::Aborted:
            return "Aborted";
        }
        return "Unknown";
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

    void remember_alpha_render_record(
        GpuTextureRecordingOutputContext *context,
        const alpha_recorder::obs::ProgramRenderRecord &record) noexcept
    {
        if (context == nullptr || record.render_time_ns == 0U)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->stop_state != GpuTextureOutputStopState::Running)
        {
            return;
        }
        context->alpha_render_records.push_back(record);
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

    void gpu_texture_recording_packet_time(obs_output_t *,
                                           encoder_packet *,
                                           encoder_packet_time *,
                                           void *);
    void gpu_texture_recording_deactivate(void *data, calldata_t *);

    void remove_packet_time_callback(obs_output_t *output,
                                     GpuTextureRecordingOutputContext *context) noexcept
    {
        if (output != nullptr && context != nullptr)
        {
            obs_output_remove_packet_callback(output, &gpu_texture_recording_packet_time, context);
        }
    }

    void disconnect_deactivate_signal(obs_output_t *output,
                                      GpuTextureRecordingOutputContext *context) noexcept
    {
        if (output == nullptr || context == nullptr)
        {
            return;
        }

        signal_handler_t *signal_handler = obs_output_get_signal_handler(output);
        if (signal_handler != nullptr)
        {
            signal_handler_disconnect(signal_handler, "deactivate", &gpu_texture_recording_deactivate,
                                      context);
        }
    }

    void mark_output_aborted(GpuTextureRecordingOutputContext &context) noexcept
    {
        obs_output_t *output = nullptr;
        bool disconnect_packet_callback = false;
        bool disconnect_signal = false;
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            output = context.output;
            disconnect_packet_callback = context.packet_callback_connected;
            disconnect_signal = context.deactivate_signal_connected;
            context.packet_callback_connected = false;
            context.deactivate_signal_connected = false;
            context.stop_state = GpuTextureOutputStopState::Aborted;
            context.stop_finalized = true;
            context.sink.abort();
            context.state_cv.notify_all();
        }

        if (disconnect_packet_callback)
        {
            remove_packet_time_callback(output, &context);
        }
        if (disconnect_signal)
        {
            disconnect_deactivate_signal(output, &context);
        }
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

    bool ensure_program_alpha_phase_textures(ProgramAlphaSource &source)
    {
        for (gs_texture_t *&texture : source.alpha_textures)
        {
            if (texture == nullptr)
            {
                texture = gs_texture_create(source.width, source.height, GS_R8, 1, nullptr, GS_RENDER_TARGET);
            }
            if (texture == nullptr)
            {
                return false;
            }
        }
        return true;
    }

    void destroy_program_alpha_phase_textures(ProgramAlphaSource &source) noexcept
    {
        for (gs_texture_t *&texture : source.alpha_textures)
        {
            if (texture != nullptr)
            {
                gs_texture_destroy(texture);
                texture = nullptr;
            }
        }
        source.write_texture_index = 0U;
        source.alpha_texture_generations = {};
        source.alpha_texture_valid = {};
    }

    bool render_program_alpha_to_texture(ProgramAlphaSource &source,
                                         gs_texture_t *program_texture,
                                         gs_texture_t *target)
    {
        if (target == nullptr)
        {
            return false;
        }

        gs_texture_t *previous_render_target = gs_get_render_target();
        gs_zstencil_t *previous_zstencil_target = gs_get_zstencil_target();
        gs_set_render_target(target, nullptr);

        vec4 clear_color;
        vec4_set(&clear_color, 0.0F, 0.0F, 0.0F, 1.0F);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0F, 0);
        gs_ortho(0.0F, static_cast<float>(source.width), 0.0F, static_cast<float>(source.height),
                 -100.0F, 100.0F);
        gs_set_viewport(0, 0, static_cast<int>(source.width), static_cast<int>(source.height));

        gs_effect_set_texture(source.image_param, program_texture);
        gs_enable_blending(false);
        while (gs_effect_loop(source.effect, "DrawAlpha"))
        {
            gs_draw_sprite(program_texture, 0, source.width, source.height);
        }
        gs_enable_blending(true);
        gs_set_render_target(previous_render_target, previous_zstencil_target);
        return true;
    }

    void draw_alpha_texture(ProgramAlphaSource &source, gs_texture_t *texture)
    {
        if (texture == nullptr)
        {
            return;
        }

        gs_effect_set_texture(source.image_param, texture);
        gs_enable_blending(false);
        while (gs_effect_loop(source.effect, "DrawRed"))
        {
            gs_draw_sprite(texture, 0, source.width, source.height);
        }
        gs_enable_blending(true);
    }

    gs_texture_t *find_generation_texture(ProgramAlphaSource &source,
                                          std::uint64_t generation) noexcept
    {
        for (std::size_t index = 0U; index < source.alpha_textures.size(); ++index)
        {
            if (source.alpha_texture_valid[index] &&
                source.alpha_texture_generations[index] == generation)
            {
                return source.alpha_textures[index];
            }
        }
        return nullptr;
    }

    alpha_recorder::obs::GpuTexturePacketRecord *find_packet_record_by_pts(
        std::vector<alpha_recorder::obs::GpuTexturePacketRecord> &records,
        std::int64_t pts) noexcept
    {
        auto found = std::find_if(records.begin(), records.end(),
                                  [pts](const alpha_recorder::obs::GpuTexturePacketRecord &record) {
                                      return record.pts == pts;
                                  });
        return found == records.end() ? nullptr : &*found;
    }

    void merge_packet_fields(alpha_recorder::obs::GpuTexturePacketRecord &record,
                             const encoder_packet &packet) noexcept
    {
        record.pts = packet.pts;
        record.dts = packet.dts;
        record.timebase_num = packet.timebase_num;
        record.timebase_den = packet.timebase_den;
        record.keyframe = packet.keyframe;
        record.sys_dts_usec = packet.sys_dts_usec;
    }

    void erase_packet_record_by_pts(std::vector<alpha_recorder::obs::GpuTexturePacketRecord> &records,
                                    std::int64_t pts) noexcept
    {
        const auto old_end = records.end();
        records.erase(std::remove_if(records.begin(), records.end(),
                                     [pts](const alpha_recorder::obs::GpuTexturePacketRecord &record) {
                                         return record.pts == pts;
                                     }),
                      old_end);
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
        destroy_program_alpha_phase_textures(*source);
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
            !ensure_program_alpha_effect(*source) || !ensure_program_alpha_phase_textures(*source))
        {
            return;
        }

        gs_texture_t *program_texture = obs_get_main_texture();
        if (program_texture == nullptr)
        {
            return;
        }

        const std::uint64_t render_time = obs_get_video_frame_time();
        const std::uint64_t generation = source->next_generation++;
        const std::uint32_t write_index = source->write_texture_index % source->alpha_textures.size();
        gs_texture_t *write_texture = source->alpha_textures[write_index];
        if (!render_program_alpha_to_texture(*source, program_texture, write_texture))
        {
            return;
        }

        bool previous_phase = true;
        if (source->context != nullptr)
        {
            std::lock_guard<std::mutex> lock(source->context->mutex);
            previous_phase =
                source->context->main_phase == alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        }
        bool emitted = true;
        std::uint64_t emitted_generation = generation;
        gs_texture_t *output_texture = write_texture;
        if (previous_phase)
        {
            if (generation == 0U)
            {
                emitted = false;
                output_texture = nullptr;
            }
            else if (gs_texture_t *delayed_texture = find_generation_texture(*source, generation - 1U);
                delayed_texture != nullptr)
            {
                emitted_generation = generation - 1U;
                output_texture = delayed_texture;
            }
            else
            {
                emitted = false;
                output_texture = nullptr;
            }
        }

        if (emitted)
        {
            draw_alpha_texture(*source, output_texture);
        }
        remember_alpha_render_record(
            source->context,
            alpha_recorder::obs::ProgramRenderRecord{generation, render_time, emitted_generation, emitted});

        source->alpha_texture_generations[write_index] = generation;
        source->alpha_texture_valid[write_index] = true;
        source->write_texture_index = (write_index + 1U) % source->alpha_textures.size();
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
            }
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
        context->main_phase = obs_data_get_bool(settings, "main_texture_encoded")
                                   ? alpha_recorder::obs::MainContentPhase::LiveProgramGeneration
                                   : alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
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
        mark_output_aborted(*context);
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
        context.source = obs_source_create_private(
            alpha_recorder::obs::gpu_texture_program_alpha_source_id(),
            kSourceName,
            source_settings);
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
        if (context->stop_state == GpuTextureOutputStopState::StopRequested &&
            context->has_stop_ts &&
            packet->sys_dts_usec >= static_cast<std::int64_t>(context->stop_ts_ns / 1000U))
        {
            erase_packet_record_by_pts(context->alpha_packet_records, packet->pts);
            context->stop_state = GpuTextureOutputStopState::StopBoundaryReached;
            context->packets_at_stop_boundary =
                static_cast<std::uint64_t>(context->alpha_packet_records.size());
            context->state_cv.notify_all();
            blog(LOG_INFO,
                 "[alpha_recorder_gpu_texture] stop boundary reached stop_ts_usec=%lld boundary_sys_dts_usec=%lld boundary_pts=%lld packets_before_boundary=%llu",
                 static_cast<long long>(context->stop_ts_ns / 1000U),
                 static_cast<long long>(packet->sys_dts_usec),
                 static_cast<long long>(packet->pts),
                 static_cast<unsigned long long>(context->packets_at_stop_boundary));
            return;
        }

        if (context->stop_state != GpuTextureOutputStopState::Running &&
            context->stop_state != GpuTextureOutputStopState::StopRequested)
        {
            return;
        }

        if (packet->pts >= 0)
        {
            alpha_recorder::obs::GpuTexturePacketRecord *record =
                find_packet_record_by_pts(context->alpha_packet_records, packet->pts);
            if (record == nullptr)
            {
                context->alpha_packet_records.push_back({});
                record = &context->alpha_packet_records.back();
            }

            merge_packet_fields(*record, *packet);
            record->input_cts = packet_time->cts;
            record->has_input_cts = true;
        }
    }

    void gpu_texture_recording_deactivate(void *data, calldata_t *)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr)
        {
            return;
        }

        obs_output_t *output = nullptr;
        bool disconnect_packet_callback = false;
        std::uint64_t packets_at_deactivate = 0U;
        std::uint64_t packets_at_stop_request = 0U;
        GpuTextureOutputStopState previous_state = GpuTextureOutputStopState::Idle;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            output = context->output;
            previous_state = context->stop_state;
            if (context->stop_state == GpuTextureOutputStopState::Running ||
                context->stop_state == GpuTextureOutputStopState::StopRequested ||
                context->stop_state == GpuTextureOutputStopState::StopBoundaryReached ||
                context->stop_state == GpuTextureOutputStopState::EndDataCaptureRequested ||
                context->stop_state == GpuTextureOutputStopState::MuxFinalized)
            {
                context->stop_state = GpuTextureOutputStopState::OutputDeactivated;
            }
            disconnect_packet_callback = context->packet_callback_connected;
            context->packet_callback_connected = false;
            if (!context->has_stop_frame_counters)
            {
                context->stop_total_frames = obs_get_total_frames();
                context->stop_lagged_frames = obs_get_lagged_frames();
                context->has_stop_frame_counters = true;
            }
            packets_at_deactivate = static_cast<std::uint64_t>(context->alpha_packet_records.size());
            packets_at_stop_request = context->packets_at_stop_request;
        }

        if (disconnect_packet_callback)
        {
            remove_packet_time_callback(output, context);
        }

        {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->state_cv.notify_all();
        }

        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] deactivate observed previous_state=%s packets_at_stop=%llu packets_at_deactivate=%llu",
             stop_state_name(previous_state),
             static_cast<unsigned long long>(packets_at_stop_request),
             static_cast<unsigned long long>(packets_at_deactivate));
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
            context->alpha_packet_records.clear();
            context->alpha_render_records.clear();
            context->start_total_frames = 0U;
            context->start_lagged_frames = 0U;
            context->stop_total_frames = 0U;
            context->stop_lagged_frames = 0U;
            context->has_stop_frame_counters = false;
            context->packet_callback_connected = false;
            context->deactivate_signal_connected = false;
            context->stop_finalized = false;
            context->stop_state = GpuTextureOutputStopState::Idle;
            context->packets_at_stop_request = 0U;
            context->packets_at_stop_boundary = 0U;
            context->stop_ts_ns = 0U;
            context->has_stop_ts = false;
        }

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

        signal_handler_t *signal_handler = obs_output_get_signal_handler(context->output);
        if (signal_handler == nullptr)
        {
            obs_output_set_last_error(context->output,
                                      "Alpha Recorder GPU texture output could not observe output deactivation");
            context->sink.abort();
            release_graph(*context);
            return false;
        }
        signal_handler_connect(signal_handler, "deactivate", &gpu_texture_recording_deactivate, context);
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->deactivate_signal_connected = true;
        }

        obs_output_add_packet_callback(context->output, &gpu_texture_recording_packet_time, context);
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->packet_callback_connected = true;
            context->stop_state = GpuTextureOutputStopState::Running;
            context->state_cv.notify_all();
        }
        if (!obs_output_begin_data_capture(context->output, 0))
        {
            obs_output_remove_packet_callback(context->output, &gpu_texture_recording_packet_time, context);
            disconnect_deactivate_signal(context->output, context);
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                context->packet_callback_connected = false;
                context->deactivate_signal_connected = false;
                context->stop_state = GpuTextureOutputStopState::Aborted;
            }
            obs_output_set_last_error(context->output,
                                      "Alpha Recorder GPU texture output could not begin data capture");
            context->sink.abort();
            release_graph(*context);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->start_total_frames = obs_get_total_frames();
            context->start_lagged_frames = obs_get_lagged_frames();
        }

        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] started path=\"%s\" encoder=%s backend=%s size=%ux%u fps=%u/%u cqp=%u gop=%u b_frames=%u preset=%s tune=%s split=%s gpu=%d phase=%s",
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
             static_cast<int>(context->hevc_encoder.nvenc_gpu_index),
             context->main_phase == alpha_recorder::obs::MainContentPhase::LiveProgramGeneration
                 ? "live"
                 : "previous");
        obs_output_set_last_error(context->output, nullptr);
        return true;
    }

    void gpu_texture_recording_stop(void *data, std::uint64_t ts)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr || context->output == nullptr)
        {
            return;
        }

        std::uint64_t packets_at_stop_request = 0U;
        GpuTextureOutputStopState previous_state = GpuTextureOutputStopState::Idle;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            previous_state = context->stop_state;
            if (context->stop_state == GpuTextureOutputStopState::Running)
            {
                context->stop_state = GpuTextureOutputStopState::StopBoundaryReached;
                context->packets_at_stop_request =
                    static_cast<std::uint64_t>(context->alpha_packet_records.size());
                context->packets_at_stop_boundary = context->packets_at_stop_request;
                context->stop_ts_ns = ts;
                context->has_stop_ts = true;
                context->stop_total_frames = obs_get_total_frames();
                context->stop_lagged_frames = obs_get_lagged_frames();
                context->has_stop_frame_counters = true;
                context->state_cv.notify_all();
            }
            packets_at_stop_request = context->packets_at_stop_request;
        }

        const alpha_recorder::obs::GpuTextureAlphaOutputSinkStats &stats = context->sink.stats();
        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] stop requested previous_state=%s packets_at_stop=%llu packets=%llu keyframes=%llu packet_bytes=%llu muxed_packets=%llu finalized=%s first_pts=%lld last_pts=%lld path=\"%s\"",
             stop_state_name(previous_state),
             static_cast<unsigned long long>(packets_at_stop_request),
             static_cast<unsigned long long>(stats.packet_count),
             static_cast<unsigned long long>(stats.keyframe_count),
             static_cast<unsigned long long>(stats.packet_bytes),
             static_cast<unsigned long long>(stats.muxed_packet_count),
             stats.finalized ? "true" : "false",
             static_cast<long long>(stats.first_pts),
             static_cast<long long>(stats.last_pts),
             context->path.generic_string().c_str());
    }

    void gpu_texture_recording_packet(void *data, encoder_packet *packet)
    {
        auto *context = static_cast<GpuTextureRecordingOutputContext *>(data);
        if (context == nullptr || packet == nullptr || packet->type != OBS_ENCODER_VIDEO)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->stop_state != GpuTextureOutputStopState::Running &&
            context->stop_state != GpuTextureOutputStopState::StopRequested)
        {
            return;
        }

        alpha_recorder::obs::GpuTexturePacketRecord *record =
            find_packet_record_by_pts(context->alpha_packet_records, packet->pts);
        if (record == nullptr)
        {
            context->alpha_packet_records.push_back({});
            record = &context->alpha_packet_records.back();
        }

        merge_packet_fields(*record, *packet);
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
        output_info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
        output_info.encoded_video_codecs = "hevc";
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

    void gpu_texture_recording_output_request_stop(obs_output_t *output) noexcept
    {
        if (output == nullptr)
        {
            return;
        }

        GpuTextureRecordingOutputContext *context = find_context(output);
        GpuTextureOutputStopState state = GpuTextureOutputStopState::Idle;
        std::uint64_t packet_count = 0U;
        if (context != nullptr)
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            state = context->stop_state;
            packet_count = static_cast<std::uint64_t>(context->alpha_packet_records.size());
        }

        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] requesting stop state=%s alpha_packets=%llu",
             stop_state_name(state),
             static_cast<unsigned long long>(packet_count));
        obs_output_stop(output);
    }

    bool gpu_texture_recording_output_wait_stop_boundary(obs_output_t *output,
                                                         std::uint32_t timeout_ms,
                                                         std::string *error_message) noexcept
    {
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output context is unavailable");
            return false;
        }

        const auto started = std::chrono::steady_clock::now();
        GpuTextureOutputStopState state = GpuTextureOutputStopState::Idle;
        std::uint64_t packets_at_stop = 0U;
        std::uint64_t packets_at_boundary = 0U;
        std::uint64_t packet_count = 0U;
        std::int64_t last_pts = 0;
        {
            std::unique_lock<std::mutex> lock(context->mutex);
            const bool ready = context->state_cv.wait_for(
                lock,
                std::chrono::milliseconds{timeout_ms},
                [&context]() {
                    return context->stop_state == GpuTextureOutputStopState::StopBoundaryReached ||
                           context->stop_state == GpuTextureOutputStopState::MuxFinalized ||
                           context->stop_state == GpuTextureOutputStopState::EndDataCaptureRequested ||
                           context->stop_state == GpuTextureOutputStopState::OutputDeactivated ||
                           context->stop_state == GpuTextureOutputStopState::Aborted;
                });
            state = context->stop_state;
            packets_at_stop = context->packets_at_stop_request;
            packets_at_boundary = context->packets_at_stop_boundary;
            packet_count = static_cast<std::uint64_t>(context->alpha_packet_records.size());
            last_pts = context->sink.stats().last_pts;

            if (!ready)
            {
                assign_error(error_message,
                             "Alpha Recorder timed out waiting for the GPU texture alpha stop boundary.");
                blog(LOG_WARNING,
                     "[alpha_recorder_gpu_texture] stop boundary wait timed out timeout_ms=%u state=%s packets_at_stop=%llu packets_at_boundary=%llu packets_now=%llu last_pts=%lld",
                     timeout_ms,
                     stop_state_name(state),
                     static_cast<unsigned long long>(packets_at_stop),
                     static_cast<unsigned long long>(packets_at_boundary),
                     static_cast<unsigned long long>(packet_count),
                     static_cast<long long>(last_pts));
                return false;
            }

            if (state == GpuTextureOutputStopState::Aborted)
            {
                assign_error(error_message,
                             "Alpha Recorder GPU texture alpha output was aborted before the stop boundary.");
                return false;
            }
        }

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] stop boundary wait complete elapsed_ms=%lld state=%s packets_at_stop=%llu packets_at_boundary=%llu packets_now=%llu last_pts=%lld",
             static_cast<long long>(elapsed_ms),
             stop_state_name(state),
             static_cast<unsigned long long>(packets_at_stop),
             static_cast<unsigned long long>(packets_at_boundary),
             static_cast<unsigned long long>(packet_count),
             static_cast<long long>(last_pts));
        return true;
    }

    void gpu_texture_recording_output_end_data_capture(obs_output_t *output) noexcept
    {
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context != nullptr)
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->stop_state == GpuTextureOutputStopState::Running ||
                context->stop_state == GpuTextureOutputStopState::StopRequested ||
                context->stop_state == GpuTextureOutputStopState::MuxFinalized ||
                context->stop_state == GpuTextureOutputStopState::StopBoundaryReached)
            {
                context->stop_state = GpuTextureOutputStopState::EndDataCaptureRequested;
                context->state_cv.notify_all();
            }
            blog(LOG_INFO,
                 "[alpha_recorder_gpu_texture] ending data capture state=%s packets=%llu finalized=%s",
                 stop_state_name(context->stop_state),
                 static_cast<unsigned long long>(context->alpha_packet_records.size()),
                 context->sink.stats().finalized ? "true" : "false");
        }
        obs_output_end_data_capture(output);
    }

    void gpu_texture_recording_output_set_main_texture_encoded(obs_output_t *output,
                                                               bool main_texture_encoded) noexcept
    {
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            return;
        }

        const alpha_recorder::obs::MainContentPhase phase =
            main_texture_encoded ? alpha_recorder::obs::MainContentPhase::LiveProgramGeneration
                                 : alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            changed = context->main_phase != phase;
            context->main_phase = phase;
            if (changed &&
                (context->stop_state == GpuTextureOutputStopState::Running ||
                 context->stop_state == GpuTextureOutputStopState::StopRequested))
            {
                context->alpha_packet_records.clear();
                context->alpha_render_records.clear();
            }
        }
        if (changed)
        {
            blog(LOG_INFO,
                 "[alpha_recorder_gpu_texture] main content phase updated phase=%s timeline_ledger_reset=true",
                 main_texture_encoded ? "live" : "previous");
        }
    }

    bool gpu_texture_recording_output_wait_deactivated(obs_output_t *output,
                                                       std::uint32_t timeout_ms,
                                                       std::string *error_message) noexcept
    {
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output context is unavailable");
            return false;
        }

        const auto started = std::chrono::steady_clock::now();
        GpuTextureOutputStopState state = GpuTextureOutputStopState::Idle;
        std::uint64_t packet_count = 0U;
        std::uint64_t packets_at_stop = 0U;
        std::int64_t last_pts = 0;
        bool finalized = false;
        {
            std::unique_lock<std::mutex> lock(context->mutex);
            const bool ready = context->state_cv.wait_for(
                lock,
                std::chrono::milliseconds{timeout_ms},
                [&context]() {
                    return context->stop_state == GpuTextureOutputStopState::OutputDeactivated ||
                           context->stop_state == GpuTextureOutputStopState::Aborted;
                });
            state = context->stop_state;
            packet_count = static_cast<std::uint64_t>(context->alpha_packet_records.size());
            packets_at_stop = context->packets_at_stop_request;
            const GpuTextureAlphaOutputSinkStats &stats = context->sink.stats();
            last_pts = stats.last_pts;
            finalized = stats.finalized;

            if (!ready)
            {
                assign_error(error_message,
                             "Alpha Recorder timed out waiting for the GPU texture alpha output to deactivate.");
                blog(LOG_WARNING,
                     "[alpha_recorder_gpu_texture] deactivate wait timed out timeout_ms=%u state=%s packets_at_stop=%llu packets_now=%llu last_pts=%lld finalized=%s",
                     timeout_ms,
                     stop_state_name(state),
                     static_cast<unsigned long long>(packets_at_stop),
                     static_cast<unsigned long long>(packet_count),
                     static_cast<long long>(last_pts),
                     finalized ? "true" : "false");
                return false;
            }

            if (state == GpuTextureOutputStopState::Aborted)
            {
                assign_error(error_message, "Alpha Recorder GPU texture alpha output was aborted before deactivation.");
                return false;
            }
        }

        bool disconnect_signal = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            disconnect_signal = context->deactivate_signal_connected;
            context->deactivate_signal_connected = false;
        }
        if (disconnect_signal)
        {
            disconnect_deactivate_signal(output, context);
        }

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] deactivate wait complete elapsed_ms=%lld state=%s packets_at_stop=%llu packets_now=%llu last_pts=%lld finalized=%s",
             static_cast<long long>(elapsed_ms),
             stop_state_name(state),
             static_cast<unsigned long long>(packets_at_stop),
             static_cast<unsigned long long>(packet_count),
             static_cast<long long>(last_pts),
             finalized ? "true" : "false");
        return true;
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

    bool gpu_texture_recording_output_finalize_mux(obs_output_t *output,
                                                   std::string *error_message) noexcept
    {
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            assign_error(error_message, "Alpha Recorder GPU texture output context is unavailable");
            return false;
        }

        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->stop_finalized)
        {
            return context->sink.stats().finalized;
        }
        if (context->stop_state != GpuTextureOutputStopState::StopBoundaryReached)
        {
            assign_error(error_message,
                         std::string{"Alpha Recorder GPU texture output cannot finalize before the stop boundary; state="} +
                             stop_state_name(context->stop_state));
            return false;
        }

        if (!context->sink.finalize(error_message))
        {
            return false;
        }

        context->sink.close_storage();
        context->stop_finalized = true;
        context->stop_state = GpuTextureOutputStopState::MuxFinalized;
        context->state_cv.notify_all();
        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] mux finalized packets=%llu muxed_packets=%llu last_pts=%lld",
             static_cast<unsigned long long>(context->sink.stats().packet_count),
             static_cast<unsigned long long>(context->sink.stats().muxed_packet_count),
             static_cast<long long>(context->sink.stats().last_pts));
        return true;
    }

    void gpu_texture_recording_output_abort_mux(obs_output_t *output) noexcept
    {
        GpuTextureRecordingOutputContext *context = find_context(output);
        if (context == nullptr)
        {
            return;
        }

        mark_output_aborted(*context);
    }

    bool gpu_texture_recording_output_compute_visible_range(obs_output_t *output,
                                                            const std::vector<GpuTexturePacketRecord> &main_packets,
                                                            bool main_texture_encoded,
                                                            AlphaVisiblePacketRange &range,
                                                            std::string *error_message) noexcept
    {
        range = {};
        if (main_packets.empty())
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

        alpha_recorder::obs::GpuTextureTimelineInput input{};
        input.main_packets = main_packets;
        input.alpha_packets = context->alpha_packet_records;
        input.alpha_renders = context->alpha_render_records;
        input.main_phase = main_texture_encoded
                               ? alpha_recorder::obs::MainContentPhase::LiveProgramGeneration
                               : alpha_recorder::obs::MainContentPhase::PreviousProgramGeneration;
        input.fps_num = context->fps_num;
        input.fps_den = context->fps_den;

        std::uint64_t alpha_packets_with_generation = 0U;
        std::uint64_t alpha_packets_with_input_cts = 0U;
        std::uint64_t alpha_min_generation = 0U;
        std::uint64_t alpha_max_generation = 0U;
        std::uint64_t alpha_min_cts = 0U;
        std::uint64_t alpha_max_cts = 0U;
        std::int64_t alpha_min_sys_dts_usec = 0;
        std::int64_t alpha_max_sys_dts_usec = 0;
        bool alpha_generation_seen = false;
        bool alpha_cts_seen = false;
        bool alpha_sys_dts_seen = false;
        for (const alpha_recorder::obs::GpuTexturePacketRecord &packet : context->alpha_packet_records)
        {
            if (!alpha_sys_dts_seen)
            {
                alpha_min_sys_dts_usec = packet.sys_dts_usec;
                alpha_max_sys_dts_usec = packet.sys_dts_usec;
                alpha_sys_dts_seen = true;
            }
            else
            {
                alpha_min_sys_dts_usec = std::min(alpha_min_sys_dts_usec, packet.sys_dts_usec);
                alpha_max_sys_dts_usec = std::max(alpha_max_sys_dts_usec, packet.sys_dts_usec);
            }
            if (packet.has_input_cts)
            {
                ++alpha_packets_with_input_cts;
                if (!alpha_cts_seen)
                {
                    alpha_min_cts = packet.input_cts;
                    alpha_max_cts = packet.input_cts;
                    alpha_cts_seen = true;
                }
                else
                {
                    alpha_min_cts = std::min(alpha_min_cts, packet.input_cts);
                    alpha_max_cts = std::max(alpha_max_cts, packet.input_cts);
                }
            }
            if (!packet.has_generation)
            {
                continue;
            }
            ++alpha_packets_with_generation;
            if (!alpha_generation_seen)
            {
                alpha_min_generation = packet.emitted_generation;
                alpha_max_generation = packet.emitted_generation;
                alpha_generation_seen = true;
            }
            else
            {
                alpha_min_generation = std::min(alpha_min_generation, packet.emitted_generation);
                alpha_max_generation = std::max(alpha_max_generation, packet.emitted_generation);
            }
        }
        std::uint64_t render_min_cts = 0U;
        std::uint64_t render_max_cts = 0U;
        bool render_cts_seen = false;
        for (const alpha_recorder::obs::ProgramRenderRecord &render : context->alpha_render_records)
        {
            if (!render_cts_seen)
            {
                render_min_cts = render.render_time_ns;
                render_max_cts = render.render_time_ns;
                render_cts_seen = true;
            }
            else
            {
                render_min_cts = std::min(render_min_cts, render.render_time_ns);
                render_max_cts = std::max(render_max_cts, render.render_time_ns);
            }
        }

        const alpha_recorder::obs::GpuTextureTimelineSolveResult solve =
            alpha_recorder::obs::solve_gpu_texture_timeline(input);
        const std::uint64_t logged_alpha_packets_with_generation =
            solve.solution.alpha_packets_with_generation != 0U
                ? solve.solution.alpha_packets_with_generation
                : alpha_packets_with_generation;
        if (solve.error != alpha_recorder::obs::TimelineSolveError::None)
        {
            assign_error(error_message, alpha_recorder::obs::timeline_solve_error_message(solve.error));
            blog(LOG_WARNING,
                 "[alpha_recorder_gpu_texture] timeline solve failed error=%s main_packets=%llu alpha_packets=%llu alpha_packets_with_input_cts=%llu alpha_input_cts_range=%llu..%llu alpha_sys_dts_usec_range=%lld..%lld alpha_packets_with_generation=%llu alpha_generation_range=%llu..%llu alpha_renders=%llu alpha_render_cts_range=%llu..%llu phase=%s alpha_epoch_source=%s alpha_latency_frames=%llu alpha_latency_ns=%llu alpha_epoch_candidates=%llu",
                 alpha_recorder::obs::timeline_solve_error_name(solve.error),
                 static_cast<unsigned long long>(main_packets.size()),
                 static_cast<unsigned long long>(context->alpha_packet_records.size()),
                 static_cast<unsigned long long>(alpha_packets_with_input_cts),
                 static_cast<unsigned long long>(alpha_min_cts),
                 static_cast<unsigned long long>(alpha_max_cts),
                 static_cast<long long>(alpha_min_sys_dts_usec),
                 static_cast<long long>(alpha_max_sys_dts_usec),
                 static_cast<unsigned long long>(logged_alpha_packets_with_generation),
                 static_cast<unsigned long long>(alpha_min_generation),
                 static_cast<unsigned long long>(alpha_max_generation),
                 static_cast<unsigned long long>(context->alpha_render_records.size()),
                 static_cast<unsigned long long>(render_min_cts),
                 static_cast<unsigned long long>(render_max_cts),
                 main_texture_encoded ? "live" : "previous",
                 alpha_recorder::obs::alpha_epoch_source_name(solve.solution.alpha_epoch_source),
                 static_cast<unsigned long long>(solve.solution.alpha_latency_frames),
                 static_cast<unsigned long long>(solve.solution.alpha_latency_ns),
                 static_cast<unsigned long long>(solve.solution.alpha_epoch_candidate_count));
            return false;
        }

        range = solve.solution.range;
        blog(LOG_INFO,
             "[alpha_recorder_gpu_texture] certified edit range media_time=%lld first_visible_alpha_pts=%lld duration=%lld main_generation=%llu alpha_generation=%llu alpha_pts_step=%lld main_packets=%llu alpha_packets=%llu alpha_packets_with_generation=%llu alpha_renders=%llu phase=%s alpha_epoch_source=%s alpha_latency_frames=%llu alpha_latency_ns=%llu alpha_epoch_candidates=%llu",
             static_cast<long long>(range.media_time),
             static_cast<long long>(solve.solution.first_visible_alpha_pts),
             static_cast<long long>(range.duration),
             static_cast<unsigned long long>(solve.solution.main_generation),
             static_cast<unsigned long long>(solve.solution.alpha_generation),
             static_cast<long long>(solve.solution.alpha_pts_step),
             static_cast<unsigned long long>(solve.solution.main_packet_count),
             static_cast<unsigned long long>(solve.solution.alpha_packet_count),
             static_cast<unsigned long long>(solve.solution.alpha_packets_with_generation),
             static_cast<unsigned long long>(context->alpha_render_records.size()),
             main_texture_encoded ? "live" : "previous",
             alpha_recorder::obs::alpha_epoch_source_name(solve.solution.alpha_epoch_source),
             static_cast<unsigned long long>(solve.solution.alpha_latency_frames),
             static_cast<unsigned long long>(solve.solution.alpha_latency_ns),
             static_cast<unsigned long long>(solve.solution.alpha_epoch_candidate_count));
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
