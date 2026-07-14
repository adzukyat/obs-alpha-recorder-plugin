#include <algorithm>
#include <cstdint>

#include <obs-module.h>

#include <graphics/graphics.h>
#include <graphics/vec4.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("alpha_recorder", "en-US")

namespace
{
    constexpr const char *kSourceId = "alpha_recorder_e2e_moving_alpha";
    constexpr std::uint32_t kFrameCodeBits = 12U;
    constexpr std::uint32_t kFrameCodeTileSize = 24U;
    constexpr std::uint32_t kFrameCodeGap = 4U;
    constexpr std::uint32_t kFrameCodeX = 16U;
    constexpr std::uint32_t kFrameCodeY = 16U;

    struct MovingAlphaSource
    {
        std::uint32_t width = 1280U;
        std::uint32_t height = 720U;
        std::uint32_t box_size = 96U;
        std::uint32_t step = 17U;
        std::uint64_t start_time = 0U;
        vec4 color{};
    };

    void moving_alpha_update(void *data, obs_data_t *settings)
    {
        auto *source = static_cast<MovingAlphaSource *>(data);
        source->width = static_cast<std::uint32_t>(obs_data_get_int(settings, "width"));
        source->height = static_cast<std::uint32_t>(obs_data_get_int(settings, "height"));
        source->box_size = static_cast<std::uint32_t>(obs_data_get_int(settings, "box_size"));
        source->step = static_cast<std::uint32_t>(obs_data_get_int(settings, "step"));

        if (source->width == 0U)
        {
            source->width = 1280U;
        }
        if (source->height == 0U)
        {
            source->height = 720U;
        }
        if (source->box_size == 0U)
        {
            source->box_size = 96U;
        }
        if (source->step == 0U)
        {
            source->step = 17U;
        }

        const std::uint32_t color = static_cast<std::uint32_t>(obs_data_get_int(settings, "color"));
        vec4_from_rgba(&source->color, color == 0U ? 0xFF00FFFFU : color);
    }

    void *moving_alpha_create(obs_data_t *settings, obs_source_t *)
    {
        auto *source = new MovingAlphaSource{};
        moving_alpha_update(source, settings);
        return source;
    }

    void moving_alpha_destroy(void *data)
    {
        delete static_cast<MovingAlphaSource *>(data);
    }

    void moving_alpha_defaults(obs_data_t *settings)
    {
        obs_data_set_default_int(settings, "width", 1280);
        obs_data_set_default_int(settings, "height", 720);
        obs_data_set_default_int(settings, "box_size", 96);
        obs_data_set_default_int(settings, "step", 17);
        obs_data_set_default_int(settings, "color", 0xFF00FFFF);
    }

    std::uint32_t moving_alpha_width(void *data)
    {
        return static_cast<MovingAlphaSource *>(data)->width;
    }

    std::uint32_t moving_alpha_height(void *data)
    {
        return static_cast<MovingAlphaSource *>(data)->height;
    }

    void moving_alpha_render(void *data, gs_effect_t *)
    {
        auto *source = static_cast<MovingAlphaSource *>(data);
        const std::uint32_t box_size = std::min(source->box_size, std::min(source->width, source->height));
        if (box_size == 0U)
        {
            return;
        }

        const std::uint32_t travel_x = source->width > box_size ? source->width - box_size : 1U;
        const std::uint32_t reserved_y = std::min(source->height, kFrameCodeY + kFrameCodeTileSize + kFrameCodeGap);
        const std::uint32_t travel_y = source->height > reserved_y + box_size ? source->height - reserved_y - box_size : 1U;
        const std::uint64_t frame_time = obs_get_video_frame_time();
        if (source->start_time == 0U || frame_time < source->start_time)
        {
            source->start_time = frame_time;
        }

        obs_video_info video_info = {};
        const bool have_video_info = obs_get_video_info(&video_info);
        const std::uint64_t fps_num = have_video_info && video_info.fps_num != 0U ? video_info.fps_num : 60U;
        const std::uint64_t fps_den = have_video_info && video_info.fps_den != 0U ? video_info.fps_den : 1U;
        const std::uint64_t frame_interval = (1000000000ULL * fps_den) / fps_num;
        const std::uint64_t frame = frame_interval == 0U ? 0U : ((frame_time - source->start_time) / frame_interval);
        const float x = static_cast<float>((frame * source->step) % travel_x);
        const float y = static_cast<float>(reserved_y + ((frame * ((source->step / 2U) + 3U)) % travel_y));

        gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");

        gs_effect_set_vec4(color_param, &source->color);
        gs_matrix_push();
        gs_matrix_translate3f(x, y, 0.0F);
        while (gs_effect_loop(solid, "Solid"))
        {
            gs_draw_sprite(nullptr, 0, box_size, box_size);
        }
        gs_matrix_pop();

        vec4 code_color;
        vec4_set(&code_color, 1.0F, 1.0F, 1.0F, 1.0F);
        gs_effect_set_vec4(color_param, &code_color);

        for (std::uint32_t index = 0U; index < kFrameCodeBits + 2U; ++index)
        {
            const bool sync_marker = index == 0U || index == kFrameCodeBits + 1U;
            const bool bit_set = sync_marker || ((frame >> (index - 1U)) & 1U) != 0U;
            if (!bit_set)
            {
                continue;
            }

            const float code_x = static_cast<float>(kFrameCodeX + index * (kFrameCodeTileSize + kFrameCodeGap));
            const float code_y = static_cast<float>(kFrameCodeY);

            gs_matrix_push();
            gs_matrix_translate3f(code_x, code_y, 0.0F);
            while (gs_effect_loop(solid, "Solid"))
            {
                gs_draw_sprite(nullptr, 0, kFrameCodeTileSize, kFrameCodeTileSize);
            }
            gs_matrix_pop();
        }
    }

    const char *moving_alpha_name(void *)
    {
        return "Alpha Recorder E2E Moving Alpha";
    }

} // namespace

namespace
{
    bool register_moving_alpha_source() noexcept
    {
        obs_source_info info = {};
        info.id = kSourceId;
        info.type = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
        info.get_name = moving_alpha_name;
        info.create = moving_alpha_create;
        info.destroy = moving_alpha_destroy;
        info.update = moving_alpha_update;
        info.get_defaults = moving_alpha_defaults;
        info.get_width = moving_alpha_width;
        info.get_height = moving_alpha_height;
        info.video_render = moving_alpha_render;

        obs_register_source(&info);
        return true;
    }
} // namespace

extern "C" bool obs_module_load(void)
{
    return register_moving_alpha_source();
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Alpha Recorder OBS app E2E moving-alpha source";
}
