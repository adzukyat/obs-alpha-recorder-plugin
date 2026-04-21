#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "alpha_recorder/frame_pair.hpp"

namespace alpha_recorder
{

    inline constexpr std::uint32_t alpha_container_format_version = 1u;
    inline constexpr std::uint32_t alpha_container_header_size = 56u;
    inline constexpr std::uint32_t alpha_record_header_size = 48u;
    inline constexpr std::uint32_t alpha_index_entry_size = 44u;
    inline constexpr std::uint32_t alpha_record_flag_lz4_block = 1u;
    inline constexpr std::array<char, 8> alpha_container_magic{{'A', 'L', 'P', 'H', 'A', 'S', 'C', '1'}};
    inline constexpr std::array<char, 8> alpha_record_magic{{'A', 'L', 'P', 'H', 'A', 'R', 'C', '1'}};

    [[nodiscard]] inline constexpr std::string_view alpha_overload_status_flag() noexcept
    {
        return "ERR_OVERLOAD";
    }

    struct AlphaContainerHeader
    {
        std::array<char, 8> magic = alpha_container_magic;
        std::uint32_t version = alpha_container_format_version;
        std::uint32_t header_size = alpha_container_header_size;
        std::uint64_t record_count = 0;
        std::uint64_t index_offset = 0;
        std::uint64_t index_entry_count = 0;
        std::uint32_t record_header_size = alpha_record_header_size;
        std::uint32_t index_entry_size = alpha_index_entry_size;
        std::uint32_t flags = 0;
        std::uint32_t reserved = 0;
    };

    struct AlphaRecordHeader
    {
        std::array<char, 8> magic = alpha_record_magic;
        std::uint32_t version = alpha_container_format_version;
        std::uint32_t header_size = alpha_record_header_size;
        std::uint64_t sequence = 0;
        std::uint64_t pts = 0;
        std::uint32_t uncompressed_size = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t flags = alpha_record_flag_lz4_block;
        std::uint32_t reserved = 0;
    };

    struct AlphaIndexEntry
    {
        std::uint64_t sequence = 0;
        std::uint64_t pts = 0;
        std::uint64_t record_offset = 0;
        std::uint32_t record_header_size = alpha_record_header_size;
        std::uint32_t uncompressed_size = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t flags = alpha_record_flag_lz4_block;
        std::uint32_t reserved = 0;
    };

    struct AlphaSessionSummary
    {
        std::string project_name{};
        std::string project_version{};
        std::string finalization_format{"prores_4444"};
        std::filesystem::path sidecar_path{};
        std::filesystem::path manifest_path{};
        std::uint32_t container_format_version = alpha_container_format_version;
        std::uint32_t container_header_size = alpha_container_header_size;
        std::uint32_t record_header_size = alpha_record_header_size;
        std::uint32_t index_entry_size = alpha_index_entry_size;
        std::uint64_t pair_count = 0;
        std::uint64_t first_sequence = 0;
        std::uint64_t last_sequence = 0;
        std::uint64_t first_pts = 0;
        std::uint64_t last_pts = 0;
        std::uint64_t alpha_uncompressed_bytes = 0;
        std::uint64_t alpha_compressed_bytes = 0;
        std::uint64_t index_offset = 0;
        std::uint64_t index_entry_count = 0;
        std::uint64_t sidecar_size_bytes = 0;
        bool overload_detected = false;
    };

    class AlphaLosslessWriter
    {
    public:
        AlphaLosslessWriter() noexcept = default;
        ~AlphaLosslessWriter() noexcept;

        bool open(const std::filesystem::path &sidecar_path) noexcept;
        bool open(const std::filesystem::path &sidecar_path, const std::filesystem::path &manifest_path) noexcept;
        bool close() noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] const std::filesystem::path &path() const noexcept;
        [[nodiscard]] const std::filesystem::path &manifest_path() const noexcept;
        [[nodiscard]] const AlphaSessionSummary &summary() const noexcept;
        [[nodiscard]] bool finalized() const noexcept;

        void set_finalization_format(std::string_view finalization_format) noexcept;
        void mark_overload() noexcept;

        bool write_pair(const FramePair &pair) noexcept;
        bool write_frame(const FramePair &pair) noexcept;

    private:
        std::filesystem::path sidecar_path_{};
        std::filesystem::path manifest_path_{};
        AlphaSessionSummary summary_{};
        std::vector<AlphaIndexEntry> index_entries_{};
        std::ofstream sidecar_stream_{};
        bool open_ = false;
        bool finalized_ = false;
    };

    using SidecarWriter = AlphaLosslessWriter;

    [[nodiscard]] std::vector<std::uint8_t> decode_lz4_literal_block(const std::vector<std::uint8_t> &input);

} // namespace alpha_recorder