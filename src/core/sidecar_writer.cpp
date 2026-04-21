#include "alpha_recorder/sidecar_writer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <ostream>
#include <system_error>
#include <type_traits>

#include "alpha_recorder/manifest_writer.hpp"
#include "alpha_recorder/version.hpp"

namespace alpha_recorder
{
    namespace
    {

        bool ensure_parent_directory(const std::filesystem::path &path) noexcept
        {
            const std::filesystem::path parent_path = path.parent_path();
            if (parent_path.empty())
            {
                return true;
            }

            std::error_code error;
            std::filesystem::create_directories(parent_path, error);
            return !error;
        }

        template <typename ValueT>
        bool write_le(std::ostream &stream, ValueT value) noexcept
        {
            static_assert(std::is_integral<ValueT>::value, "value must be integral");

            using UnsignedValueT = typename std::make_unsigned<ValueT>::type;
            const UnsignedValueT unsigned_value = static_cast<UnsignedValueT>(value);

            std::array<char, sizeof(ValueT)> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                bytes[index] = static_cast<char>((unsigned_value >> (index * 8U)) & static_cast<UnsignedValueT>(0xFFU));
            }

            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(stream);
        }

        bool write_magic(std::ostream &stream, const std::array<char, 8> &magic) noexcept
        {
            stream.write(magic.data(), static_cast<std::streamsize>(magic.size()));
            return static_cast<bool>(stream);
        }

        bool write_container_header(std::ostream &stream, const AlphaContainerHeader &header) noexcept
        {
            return write_magic(stream, header.magic) && write_le(stream, header.version) && write_le(stream, header.header_size) && write_le(stream, header.record_count) && write_le(stream, header.index_offset) && write_le(stream, header.index_entry_count) && write_le(stream, header.record_header_size) && write_le(stream, header.index_entry_size) && write_le(stream, header.flags) && write_le(stream, header.reserved);
        }

        bool write_record_header(std::ostream &stream, const AlphaRecordHeader &header) noexcept
        {
            return write_magic(stream, header.magic) && write_le(stream, header.version) && write_le(stream, header.header_size) && write_le(stream, header.sequence) && write_le(stream, header.pts) && write_le(stream, header.uncompressed_size) && write_le(stream, header.compressed_size) && write_le(stream, header.flags) && write_le(stream, header.reserved);
        }

        bool write_index_entry(std::ostream &stream, const AlphaIndexEntry &entry) noexcept
        {
            return write_le(stream, entry.sequence) && write_le(stream, entry.pts) && write_le(stream, entry.record_offset) && write_le(stream, entry.record_header_size) && write_le(stream, entry.uncompressed_size) && write_le(stream, entry.compressed_size) && write_le(stream, entry.flags) && write_le(stream, entry.reserved);
        }

        std::filesystem::path default_manifest_path(const std::filesystem::path &sidecar_path)
        {
            std::filesystem::path manifest_path = sidecar_path;
            manifest_path.replace_extension(".manifest.json");
            return manifest_path;
        }

        std::vector<std::uint8_t> encode_lz4_literal_block(const std::vector<std::uint8_t> &input)
        {
            std::vector<std::uint8_t> output;
            output.reserve(input.size() + 8U);

            const std::size_t literal_size = input.size();
            const std::uint8_t literal_nibble = static_cast<std::uint8_t>(std::min<std::size_t>(literal_size, 15U));
            output.push_back(static_cast<std::uint8_t>(literal_nibble << 4U));

            if (literal_size >= 15U)
            {
                std::size_t remaining = literal_size - 15U;

                while (remaining >= 255U)
                {
                    output.push_back(255U);
                    remaining -= 255U;
                }

                output.push_back(static_cast<std::uint8_t>(remaining));
            }

            output.insert(output.end(), input.begin(), input.end());
            return output;
        }

        std::uint64_t current_position(std::ofstream &stream) noexcept
        {
            const std::streampos position = stream.tellp();
            if (position == std::streampos(-1))
            {
                return 0;
            }

            return static_cast<std::uint64_t>(position);
        }

    } // namespace

    AlphaLosslessWriter::~AlphaLosslessWriter() noexcept
    {
        close();
    }

    bool AlphaLosslessWriter::open(const std::filesystem::path &sidecar_path) noexcept
    {
        return open(sidecar_path, default_manifest_path(sidecar_path));
    }

    bool AlphaLosslessWriter::open(const std::filesystem::path &sidecar_path, const std::filesystem::path &manifest_path) noexcept
    {
        if (sidecar_path.empty() || manifest_path.empty() || open_)
        {
            return false;
        }

        try
        {
            if (!ensure_parent_directory(sidecar_path) || !ensure_parent_directory(manifest_path))
            {
                return false;
            }

            sidecar_stream_.close();
            sidecar_stream_.clear();
            sidecar_stream_.open(sidecar_path, std::ios::binary | std::ios::trunc);
            if (!sidecar_stream_)
            {
                return false;
            }

            sidecar_path_ = sidecar_path;
            manifest_path_ = manifest_path;
            summary_ = AlphaSessionSummary{};
            summary_.project_name.assign(project_name().data(), project_name().size());
            summary_.project_version.assign(project_version().data(), project_version().size());
            summary_.sidecar_path = sidecar_path_;
            summary_.manifest_path = manifest_path_;
            summary_.container_format_version = alpha_container_format_version;
            summary_.container_header_size = alpha_container_header_size;
            summary_.record_header_size = alpha_record_header_size;
            summary_.index_entry_size = alpha_index_entry_size;
            index_entries_.clear();

            const AlphaContainerHeader header{};
            if (!write_container_header(sidecar_stream_, header))
            {
                sidecar_stream_.close();
                finalized_ = false;
                return false;
            }

            open_ = true;
            finalized_ = false;
            return true;
        }
        catch (...)
        {
            sidecar_stream_.close();
            open_ = false;
            finalized_ = false;
            return false;
        }
    }

    bool AlphaLosslessWriter::close() noexcept
    {
        if (!sidecar_stream_.is_open())
        {
            open_ = false;
            return true;
        }

        bool success = true;

        try
        {
            const std::uint64_t index_offset = current_position(sidecar_stream_);

            summary_.index_offset = index_offset;
            summary_.index_entry_count = static_cast<std::uint64_t>(index_entries_.size());

            AlphaContainerHeader header{};
            header.record_count = summary_.pair_count;
            header.index_offset = index_offset;
            header.index_entry_count = static_cast<std::uint64_t>(index_entries_.size());

            sidecar_stream_.seekp(0, std::ios::beg);
            if (!sidecar_stream_ || !write_container_header(sidecar_stream_, header))
            {
                sidecar_stream_.close();
                open_ = false;
                index_entries_.clear();
                finalized_ = false;
                return false;
            }

            sidecar_stream_.seekp(static_cast<std::streamoff>(index_offset), std::ios::beg);
            if (!sidecar_stream_)
            {
                sidecar_stream_.close();
                open_ = false;
                index_entries_.clear();
                finalized_ = false;
                return false;
            }

            for (const AlphaIndexEntry &entry : index_entries_)
            {
                if (!write_index_entry(sidecar_stream_, entry))
                {
                    sidecar_stream_.close();
                    open_ = false;
                    index_entries_.clear();
                    finalized_ = false;
                    return false;
                }
            }

            sidecar_stream_.flush();
            if (!sidecar_stream_)
            {
                sidecar_stream_.close();
                open_ = false;
                index_entries_.clear();
                finalized_ = false;
                return false;
            }

            summary_.sidecar_size_bytes = current_position(sidecar_stream_);

            sidecar_stream_.close();
            open_ = false;
            index_entries_.clear();

            ManifestWriter manifest_writer;
            success = manifest_writer.write(summary_);
            finalized_ = success;
        }
        catch (...)
        {
            sidecar_stream_.close();
            open_ = false;
            index_entries_.clear();
            finalized_ = false;
            success = false;
        }

        return success;
    }

    bool AlphaLosslessWriter::is_open() const noexcept
    {
        return open_ && sidecar_stream_.is_open();
    }

    const std::filesystem::path &AlphaLosslessWriter::path() const noexcept
    {
        return sidecar_path_;
    }

    const std::filesystem::path &AlphaLosslessWriter::manifest_path() const noexcept
    {
        return manifest_path_;
    }

    const AlphaSessionSummary &AlphaLosslessWriter::summary() const noexcept
    {
        return summary_;
    }

    bool AlphaLosslessWriter::finalized() const noexcept
    {
        return finalized_;
    }

    void AlphaLosslessWriter::set_finalization_format(std::string_view finalization_format) noexcept
    {
        summary_.finalization_format.assign(finalization_format.data(), finalization_format.size());
    }

    void AlphaLosslessWriter::mark_overload() noexcept
    {
        summary_.overload_detected = true;
    }

    bool AlphaLosslessWriter::write_pair(const FramePair &pair) noexcept
    {
        if (!is_open() || !pair.is_complete())
        {
            return false;
        }

        if (pair.alpha.bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return false;
        }

        try
        {
            const std::vector<std::uint8_t> compressed_bytes = encode_lz4_literal_block(pair.alpha.bytes);
            if (compressed_bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            {
                return false;
            }

            const std::uint64_t record_offset = current_position(sidecar_stream_);

            AlphaRecordHeader record_header{};
            record_header.sequence = pair.sequence;
            record_header.pts = pair.pts;
            record_header.uncompressed_size = static_cast<std::uint32_t>(pair.alpha.bytes.size());
            record_header.compressed_size = static_cast<std::uint32_t>(compressed_bytes.size());

            if (!write_record_header(sidecar_stream_, record_header))
            {
                sidecar_stream_.close();
                open_ = false;
                index_entries_.clear();
                return false;
            }

            sidecar_stream_.write(reinterpret_cast<const char *>(compressed_bytes.data()), static_cast<std::streamsize>(compressed_bytes.size()));
            if (!sidecar_stream_)
            {
                sidecar_stream_.close();
                open_ = false;
                index_entries_.clear();
                return false;
            }

            AlphaIndexEntry index_entry{};
            index_entry.sequence = pair.sequence;
            index_entry.pts = pair.pts;
            index_entry.record_offset = record_offset;
            index_entry.record_header_size = alpha_record_header_size;
            index_entry.uncompressed_size = record_header.uncompressed_size;
            index_entry.compressed_size = record_header.compressed_size;

            index_entries_.push_back(index_entry);
            ++summary_.pair_count;
            summary_.index_entry_count = summary_.pair_count;
            summary_.alpha_uncompressed_bytes += record_header.uncompressed_size;
            summary_.alpha_compressed_bytes += record_header.compressed_size;

            if (summary_.pair_count == 1U)
            {
                summary_.first_sequence = pair.sequence;
                summary_.first_pts = pair.pts;
            }

            summary_.last_sequence = pair.sequence;
            summary_.last_pts = pair.pts;
            return true;
        }
        catch (...)
        {
            sidecar_stream_.close();
            open_ = false;
            index_entries_.clear();
            finalized_ = false;
            finalized_ = false;
            return false;
        }
    }

    bool AlphaLosslessWriter::write_frame(const FramePair &pair) noexcept
    {
        return write_pair(pair);
    }

    std::vector<std::uint8_t> decode_lz4_literal_block(const std::vector<std::uint8_t> &input)
    {
        std::vector<std::uint8_t> output;
        if (input.empty())
        {
            return output;
        }

        std::size_t cursor = 0;
        while (cursor < input.size())
        {
            const std::uint8_t token = input[cursor++];
            if ((token & 0x0FU) != 0U)
            {
                return {};
            }

            std::size_t literal_length = static_cast<std::size_t>(token >> 4U);
            if (literal_length == 15U)
            {
                while (true)
                {
                    if (cursor >= input.size())
                    {
                        return {};
                    }

                    const std::uint8_t extra = input[cursor++];
                    literal_length += extra;
                    if (extra != 255U)
                    {
                        break;
                    }
                }
            }

            if (cursor + literal_length > input.size())
            {
                return {};
            }

            output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(cursor),
                          input.begin() + static_cast<std::ptrdiff_t>(cursor + literal_length));
            cursor += literal_length;
        }

        return output;
    }

} // namespace alpha_recorder