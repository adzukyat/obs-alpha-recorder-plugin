#pragma once

#include "recording_telemetry.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <graphics/graphics.h>

namespace alpha_recorder::obs
{

    class AlphaPlaneExtractor
    {
    public:
        bool ensure(std::uint32_t width, std::uint32_t height, std::string &error_message);

        bool capture_latest(std::vector<std::uint8_t> &alpha,
                            std::uint64_t &timestamp,
                            std::string &error_message,
                            CaptureTiming *timing = nullptr);

        void destroy() noexcept;

    private:
        [[nodiscard]] bool stage_surfaces_ready() const noexcept;
        void map_staged_surface(std::size_t surface_index,
                                std::vector<std::uint8_t> &alpha,
                                std::string &error_message);
        bool render_alpha_mask(gs_texture_t *program_texture);

        gs_effect_t *effect_ = nullptr;
        gs_eparam_t *image_param_ = nullptr;
        gs_texture_t *mask_texture_ = nullptr;
        static constexpr std::size_t kStageSurfaceCount = 4U;
        std::array<gs_stagesurf_t *, kStageSurfaceCount> stage_surfaces_{};
        std::array<std::uint64_t, kStageSurfaceCount> staged_timestamps_{};
        std::uint32_t width_ = 0U;
        std::uint32_t height_ = 0U;
        std::size_t next_stage_surface_ = 0U;
        std::size_t next_map_surface_ = 0U;
        std::size_t staged_surface_count_ = 0U;
    };

} // namespace alpha_recorder::obs
