#include "alpha_recorder/sidecar_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

namespace alpha_recorder
{
    namespace
    {

        template <typename ValueT>
        bool read_le(std::istream &stream, ValueT &value) noexcept
        {
            static_assert(std::is_integral<ValueT>::value, "value must be integral");

            std::array<char, sizeof(ValueT)> bytes{};
            stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!stream)
            {
                return false;
            }

            using UnsignedValueT = typename std::make_unsigned<ValueT>::type;
            UnsignedValueT result = 0;
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                result |= static_cast<UnsignedValueT>(static_cast<unsigned char>(bytes[index])) << (index * 8U);
            }

            value = static_cast<ValueT>(result);
            return true;
        }

        bool read_magic(std::istream &stream, const std::array<char, 8> &expected) noexcept
        {
            std::array<char, 8> magic{};
            stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
            return static_cast<bool>(stream) && magic == expected;
        }

        void set_error(std::string *error_message, std::string message)
        {
            if (error_message != nullptr)
            {
                *error_message = std::move(message);
            }
        }

    } // namespace

    bool AlphaSidecarReader::open(const std::filesystem::path &sidecar_path, std::string *error_message) noexcept
    {
        close();

        if (sidecar_path.empty())
        {
            set_error(error_message, "sidecar path is empty");
            return false;
        }

        try
        {
            std::ifstream stream(sidecar_path, std::ios::binary);
            if (!stream)
            {
                set_error(error_message, std::string{"failed to open sidecar: "} + sidecar_path.generic_string());
                return false;
            }

            AlphaContainerHeader header{};
            if (!read_magic(stream, alpha_container_magic) || !read_le(stream, header.version) ||
                !read_le(stream, header.header_size) || !read_le(stream, header.record_count) ||
                !read_le(stream, header.index_offset) || !read_le(stream, header.index_entry_count) ||
                !read_le(stream, header.record_header_size) || !read_le(stream, header.index_entry_size) ||
                !read_le(stream, header.flags) || !read_le(stream, header.reserved))
            {
                set_error(error_message, std::string{"failed to read sidecar header: "} + sidecar_path.generic_string());
                return false;
            }

            if (header.version != alpha_container_format_version || header.header_size != alpha_container_header_size ||
                header.record_header_size != alpha_record_header_size || header.index_entry_size != alpha_index_entry_size ||
                header.record_count != header.index_entry_count || header.flags != 0U || header.reserved != 0U)
            {
                set_error(error_message, "sidecar header fields are invalid");
                return false;
            }

            std::error_code file_error;
            const std::uintmax_t file_size = std::filesystem::file_size(sidecar_path, file_error);
            if (file_error)
            {
                set_error(error_message, std::string{"failed to read sidecar file size: "} + sidecar_path.generic_string());
                return false;
            }

            if (header.index_offset < alpha_container_header_size || header.index_offset > file_size)
            {
                set_error(error_message, "sidecar index offset is invalid");
                return false;
            }

            const std::uintmax_t expected_index_bytes = header.index_entry_count * static_cast<std::uintmax_t>(alpha_index_entry_size);
            if (header.index_offset > file_size || expected_index_bytes > file_size - header.index_offset)
            {
                set_error(error_message, "sidecar index extends past the end of the file");
                return false;
            }

            stream.seekg(static_cast<std::streamoff>(header.index_offset), std::ios::beg);
            if (!stream)
            {
                set_error(error_message, "failed to seek to the sidecar index");
                return false;
            }

            std::vector<AlphaIndexEntry> index_entries(static_cast<std::size_t>(header.index_entry_count));
            for (AlphaIndexEntry &entry : index_entries)
            {
                if (!read_le(stream, entry.sequence) || !read_le(stream, entry.pts) || !read_le(stream, entry.record_offset) ||
                    !read_le(stream, entry.record_header_size) || !read_le(stream, entry.uncompressed_size) ||
                    !read_le(stream, entry.compressed_size) || !read_le(stream, entry.flags) || !read_le(stream, entry.reserved))
                {
                    set_error(error_message, "failed to read the sidecar index entries");
                    return false;
                }

                if (entry.record_header_size != alpha_record_header_size || entry.flags != alpha_record_flag_lz4_block || entry.reserved != 0U)
                {
                    set_error(error_message, "sidecar index entry fields are invalid");
                    return false;
                }

                if (entry.record_offset < alpha_container_header_size || entry.record_offset > file_size)
                {
                    set_error(error_message, "sidecar record offset is invalid");
                    return false;
                }

                if (entry.record_offset + static_cast<std::uintmax_t>(entry.record_header_size) > file_size ||
                    entry.record_offset + static_cast<std::uintmax_t>(entry.record_header_size) + static_cast<std::uintmax_t>(entry.compressed_size) > file_size)
                {
                    set_error(error_message, "sidecar record extends past the end of the file");
                    return false;
                }
            }

            sidecar_path_ = sidecar_path;
            header_ = header;
            index_entries_ = std::move(index_entries);
            stream_.swap(stream);
            file_size_ = file_size;
            open_ = true;
            return true;
        }
        catch (...)
        {
            set_error(error_message, "failed to open sidecar reader due to an unexpected error");
            close();
            return false;
        }
    }

    void AlphaSidecarReader::close() noexcept
    {
        if (stream_.is_open())
        {
            stream_.close();
        }

        stream_.clear();
        sidecar_path_.clear();
        header_ = AlphaContainerHeader{};
        index_entries_.clear();
        file_size_ = 0;
        open_ = false;
    }

    bool AlphaSidecarReader::is_open() const noexcept
    {
        return open_ && stream_.is_open();
    }

    const std::filesystem::path &AlphaSidecarReader::path() const noexcept
    {
        return sidecar_path_;
    }

    const AlphaContainerHeader &AlphaSidecarReader::header() const noexcept
    {
        return header_;
    }

    const std::vector<AlphaIndexEntry> &AlphaSidecarReader::index_entries() const noexcept
    {
        return index_entries_;
    }

    bool AlphaSidecarReader::read_frame(const AlphaIndexEntry &entry, AlphaSidecarFrame &frame, std::string *error_message) noexcept
    {
        if (!is_open())
        {
            set_error(error_message, "sidecar reader is not open");
            return false;
        }

        try
        {
            if (entry.record_offset < alpha_container_header_size || entry.record_offset > file_size_)
            {
                set_error(error_message, "sidecar record offset is invalid");
                return false;
            }

            if (entry.record_offset + static_cast<std::uintmax_t>(entry.record_header_size) > file_size_ ||
                entry.record_offset + static_cast<std::uintmax_t>(entry.record_header_size) + static_cast<std::uintmax_t>(entry.compressed_size) > file_size_)
            {
                set_error(error_message, "sidecar record extends past the end of the file");
                return false;
            }

            stream_.clear();
            stream_.seekg(static_cast<std::streamoff>(entry.record_offset), std::ios::beg);
            if (!stream_)
            {
                set_error(error_message, "failed to seek to a sidecar record");
                return false;
            }

            if (!read_magic(stream_, alpha_record_magic))
            {
                set_error(error_message, "sidecar record magic mismatch");
                return false;
            }

            AlphaRecordHeader header{};
            if (!read_le(stream_, header.version) || !read_le(stream_, header.header_size) || !read_le(stream_, header.sequence) ||
                !read_le(stream_, header.pts) || !read_le(stream_, header.uncompressed_size) || !read_le(stream_, header.compressed_size) ||
                !read_le(stream_, header.flags) || !read_le(stream_, header.reserved))
            {
                set_error(error_message, "failed to read a sidecar record header");
                return false;
            }

            if (header.version != alpha_container_format_version || header.header_size != alpha_record_header_size ||
                header.sequence != entry.sequence || header.pts != entry.pts || header.uncompressed_size != entry.uncompressed_size ||
                header.compressed_size != entry.compressed_size || header.flags != alpha_record_flag_lz4_block || header.reserved != 0U)
            {
                set_error(error_message, "sidecar record header fields are invalid");
                return false;
            }

            std::vector<std::uint8_t> compressed_payload(static_cast<std::size_t>(entry.compressed_size));
            if (!compressed_payload.empty())
            {
                stream_.read(reinterpret_cast<char *>(compressed_payload.data()), static_cast<std::streamsize>(compressed_payload.size()));
                if (!stream_)
                {
                    set_error(error_message, "failed to read the sidecar record payload");
                    return false;
                }
            }

            const std::vector<std::uint8_t> decoded_payload = decode_lz4_literal_block(compressed_payload);
            if (decoded_payload.size() != static_cast<std::size_t>(entry.uncompressed_size))
            {
                set_error(error_message, "sidecar record payload did not decode to the expected size");
                return false;
            }

            frame.index = entry;
            frame.alpha_bytes = decoded_payload;
            return true;
        }
        catch (...)
        {
            set_error(error_message, "failed to read a sidecar record due to an unexpected error");
            return false;
        }
    }

} // namespace alpha_recorder