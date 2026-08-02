#include "alpha_plane_extractor.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

#include <obs-module.h>

#include <graphics/vec4.h>
#include <util/bmem.h>

namespace alpha_recorder::obs
{
    namespace
    {
        constexpr std::string_view kAlphaExtractEffect = R"(
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

        void copy_alpha_plane(std::vector<std::uint8_t> &alpha,
                              const std::uint8_t *source,
                              std::uint32_t source_linesize,
                              std::uint32_t width,
                              std::uint32_t height)
        {
            const std::size_t alpha_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            alpha.resize(alpha_bytes);

            for (std::uint32_t row = 0; row < height; ++row)
            {
                const std::uint8_t *source_row =
                    source + (static_cast<std::size_t>(row) * static_cast<std::size_t>(source_linesize));
                std::uint8_t *dest_row = alpha.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width));
                std::copy(source_row, source_row + width, dest_row);
            }
        }
    } // namespace

    bool AlphaPlaneExtractor::ensure(std::uint32_t width, std::uint32_t height, std::string &error_message)
    {
        if (width == 0U || height == 0U)
        {
            error_message = "Alpha Recorder cannot capture a zero-sized OBS frame.";
            return false;
        }

        if (effect_ != nullptr && mask_texture_ != nullptr && stage_surfaces_ready() && width_ == width &&
            height_ == height)
        {
            return true;
        }

        destroy();

        char *effect_error = nullptr;
        effect_ = gs_effect_create(std::string{kAlphaExtractEffect}.c_str(), "alpha-recorder-alpha-extract.effect",
                                   &effect_error);
        if (effect_ == nullptr)
        {
            error_message = effect_error != nullptr ? std::string{effect_error}
                                                    : std::string{"Alpha Recorder could not create the alpha extraction shader."};
            if (effect_error != nullptr)
            {
                bfree(effect_error);
            }
            return false;
        }

        image_param_ = gs_effect_get_param_by_name(effect_, "image");
        mask_texture_ = gs_texture_create(width, height, GS_R8, 1, nullptr, GS_RENDER_TARGET);
        for (gs_stagesurf_t *&stage_surface : stage_surfaces_)
        {
            stage_surface = gs_stagesurface_create(width, height, GS_R8);
        }

        if (image_param_ == nullptr || mask_texture_ == nullptr || !stage_surfaces_ready())
        {
            error_message = "Alpha Recorder could not allocate GPU resources for alpha extraction.";
            destroy();
            return false;
        }

        width_ = width;
        height_ = height;
        return true;
    }

    bool AlphaPlaneExtractor::capture_latest(std::vector<std::uint8_t> &alpha,
                                             std::uint64_t &timestamp,
                                             std::string &error_message,
                                             CaptureTiming *timing)
    {
        const auto total_start = std::chrono::steady_clock::now();
        CaptureTiming local_timing{};
        alpha.clear();
        timestamp = 0U;

        gs_texture_t *program_texture = obs_get_main_texture();
        if (program_texture == nullptr)
        {
            if (timing != nullptr)
            {
                local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
                *timing = local_timing;
            }
            return true;
        }

        if (staged_surface_count_ == stage_surfaces_.size())
        {
            const auto map_start = std::chrono::steady_clock::now();
            map_staged_surface(next_map_surface_, alpha, error_message);
            local_timing.mapped = true;
            local_timing.map_ns = elapsed_ns(map_start, std::chrono::steady_clock::now());
            if (!error_message.empty())
            {
                if (timing != nullptr)
                {
                    local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
                    *timing = local_timing;
                }
                return false;
            }
            timestamp = staged_timestamps_[next_map_surface_];
            next_map_surface_ = (next_map_surface_ + 1U) % stage_surfaces_.size();
            --staged_surface_count_;
        }

        const auto render_start = std::chrono::steady_clock::now();
        if (!render_alpha_mask(program_texture))
        {
            if (timing != nullptr)
            {
                local_timing.render_ns = elapsed_ns(render_start, std::chrono::steady_clock::now());
                local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
                *timing = local_timing;
            }
            return false;
        }
        local_timing.render_ns = elapsed_ns(render_start, std::chrono::steady_clock::now());

        const auto stage_start = std::chrono::steady_clock::now();
        gs_stage_texture(stage_surfaces_[next_stage_surface_], mask_texture_);
        staged_timestamps_[next_stage_surface_] = obs_get_video_frame_time();
        next_stage_surface_ = (next_stage_surface_ + 1U) % stage_surfaces_.size();
        ++staged_surface_count_;
        local_timing.stage_ns = elapsed_ns(stage_start, std::chrono::steady_clock::now());
        local_timing.total_ns = elapsed_ns(total_start, std::chrono::steady_clock::now());
        if (timing != nullptr)
        {
            *timing = local_timing;
        }

        return true;
    }

    void AlphaPlaneExtractor::destroy() noexcept
    {
        for (gs_stagesurf_t *&stage_surface : stage_surfaces_)
        {
            if (stage_surface != nullptr)
            {
                gs_stagesurface_destroy(stage_surface);
                stage_surface = nullptr;
            }
        }

        if (mask_texture_ != nullptr)
        {
            gs_texture_destroy(mask_texture_);
            mask_texture_ = nullptr;
        }

        if (effect_ != nullptr)
        {
            gs_effect_destroy(effect_);
            effect_ = nullptr;
        }

        image_param_ = nullptr;
        width_ = 0U;
        height_ = 0U;
        staged_timestamps_ = {};
        next_stage_surface_ = 0U;
        next_map_surface_ = 0U;
        staged_surface_count_ = 0U;
    }

    bool AlphaPlaneExtractor::stage_surfaces_ready() const noexcept
    {
        return std::all_of(stage_surfaces_.begin(), stage_surfaces_.end(),
                           [](gs_stagesurf_t *surface) { return surface != nullptr; });
    }

    void AlphaPlaneExtractor::map_staged_surface(std::size_t surface_index,
                                                 std::vector<std::uint8_t> &alpha,
                                                 std::string &error_message)
    {
        std::uint8_t *data = nullptr;
        std::uint32_t linesize = 0U;
        gs_stagesurf_t *const stage_surface = stage_surfaces_[surface_index];
        if (stage_surface == nullptr || !gs_stagesurface_map(stage_surface, &data, &linesize))
        {
            error_message = "Alpha Recorder could not map the staged alpha frame.";
            return;
        }

        if (data == nullptr || linesize < width_)
        {
            gs_stagesurface_unmap(stage_surface);
            error_message = "Alpha Recorder received an invalid staged alpha frame.";
            return;
        }

        copy_alpha_plane(alpha, data, linesize, width_, height_);
        gs_stagesurface_unmap(stage_surface);
    }

    bool AlphaPlaneExtractor::render_alpha_mask(gs_texture_t *program_texture)
    {
        gs_texture_t *previous_render_target = gs_get_render_target();
        gs_zstencil_t *previous_zstencil_target = gs_get_zstencil_target();

        gs_set_render_target(mask_texture_, nullptr);
        vec4 clear_color;
        vec4_set(&clear_color, 0.0F, 0.0F, 0.0F, 1.0F);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0F, 0);
        gs_ortho(0.0F, static_cast<float>(width_), 0.0F, static_cast<float>(height_), -100.0F, 100.0F);
        gs_set_viewport(0, 0, static_cast<int>(width_), static_cast<int>(height_));

        gs_enable_blending(false);
        gs_effect_set_texture(image_param_, program_texture);
        while (gs_effect_loop(effect_, "Draw"))
        {
            gs_draw_sprite(program_texture, 0, width_, height_);
        }
        gs_enable_blending(true);
        gs_set_render_target(previous_render_target, previous_zstencil_target);
        return true;
    }

} // namespace alpha_recorder::obs
