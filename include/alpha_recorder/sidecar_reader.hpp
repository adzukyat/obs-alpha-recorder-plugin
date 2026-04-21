#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "alpha_recorder/sidecar_writer.hpp"

namespace alpha_recorder
{

    struct AlphaSidecarFrame
    {
        AlphaIndexEntry index{};
        std::vector<std::uint8_t> alpha_bytes{};
    };

    class AlphaSidecarReader
    {
    public:
        AlphaSidecarReader() noexcept = default;
        ~AlphaSidecarReader() noexcept = default;

        bool open(const std::filesystem::path &sidecar_path, std::string *error_message = nullptr) noexcept;
        void close() noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] const std::filesystem::path &path() const noexcept;
        [[nodiscard]] const AlphaContainerHeader &header() const noexcept;
        [[nodiscard]] const std::vector<AlphaIndexEntry> &index_entries() const noexcept;

        bool read_frame(const AlphaIndexEntry &entry, AlphaSidecarFrame &frame,
                        std::string *error_message = nullptr) noexcept;

    private:
        std::filesystem::path sidecar_path_{};
        AlphaContainerHeader header_{};
        std::vector<AlphaIndexEntry> index_entries_{};
        std::ifstream stream_{};
        std::uintmax_t file_size_ = 0;
        bool open_ = false;
    };

} // namespace alpha_recorder