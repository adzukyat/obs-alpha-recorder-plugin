#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "alpha_recorder/plugin.hpp"

namespace alpha_recorder::obs
{

    struct AlphaMaskVideoWriterConfig
    {
        std::filesystem::path output_path{};
        FinalizationFormat finalization_format = FinalizationFormat::MaskPngMov;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t fps_num = 30;
        std::uint32_t fps_den = 1;
    };

    class AlphaMaskVideoWriter
    {
    public:
        AlphaMaskVideoWriter() noexcept = default;
        ~AlphaMaskVideoWriter() noexcept;

        AlphaMaskVideoWriter(const AlphaMaskVideoWriter &) = delete;
        AlphaMaskVideoWriter &operator=(const AlphaMaskVideoWriter &) = delete;

        [[nodiscard]] bool open(const AlphaMaskVideoWriterConfig &config,
                                std::string *error_message = nullptr) noexcept;
        [[nodiscard]] bool write_frame(const std::uint8_t *alpha,
                                       std::uint32_t stride,
                                       std::string *error_message = nullptr) noexcept;
        [[nodiscard]] bool write_frame(std::vector<std::uint8_t> alpha,
                                       std::string *error_message = nullptr) noexcept;
        [[nodiscard]] bool write_frame(std::shared_ptr<const std::vector<std::uint8_t>> alpha,
                                       std::string *error_message = nullptr) noexcept;
        [[nodiscard]] bool close(std::string *error_message = nullptr) noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] const std::filesystem::path &path() const noexcept;
        [[nodiscard]] std::uint64_t frame_count() const noexcept;

    private:
        struct Impl;
        Impl *impl_ = nullptr;
    };

    [[nodiscard]] bool finalization_format_runtime_available(FinalizationFormat format,
                                                              std::string *reason = nullptr) noexcept;
    [[nodiscard]] FinalizationFormat preferred_runtime_finalization_format() noexcept;

} // namespace alpha_recorder::obs
