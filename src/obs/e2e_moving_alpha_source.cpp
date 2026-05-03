#include "alpha_recorder/plugin.hpp"

#include <algorithm>
#include <cstdint>

#include <obs-module.h>

#include <graphics/graphics.h>
#include <graphics/vec4.h>

namespace
{
    constexpr const char *kSourceId = "alpha_recorder_e2e_moving_alpha";

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
        const std::uint32_t travel_y = source->height > box_size ? source->height - box_size : 1U;
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
        const float y = static_cast<float>((frame * ((source->step / 2U) + 3U)) % travel_y);

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
    }

    const char *moving_alpha_name(void *)
    {
        return "Alpha Recorder E2E Moving Alpha";
    }

} // namespace

namespace alpha_recorder::obs
{
    bool register_e2e_sources() noexcept
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
} // namespace alpha_recorder::obs
