#include <cstddef>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <type_traits>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "alpha_recorder/manifest_writer.hpp"
#include "alpha_recorder/sidecar_writer.hpp"

namespace
{

    template <typename ValueT>
    bool read_le(std::istream &stream, ValueT &value)
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

    bool read_magic(std::istream &stream, const std::array<char, 8> &expected)
    {
        std::array<char, 8> magic{};
        stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        return static_cast<bool>(stream) && magic == expected;
    }

    std::vector<std::uint8_t> decode_lz4_literal_block(const std::vector<std::uint8_t> &block)
    {
        std::vector<std::uint8_t> output;
        std::size_t cursor = 0;

        while (cursor < block.size())
        {
            const std::uint8_t token = block[cursor++];
            std::size_t literal_length = token >> 4U;

            if (literal_length == 15U)
            {
                while (true)
                {
                    if (cursor >= block.size())
                    {
                        return {};
                    }

                    const std::uint8_t extra = block[cursor++];
                    literal_length += extra;
                    if (extra != 255U)
                    {
                        break;
                    }
                }
            }

            if (cursor + literal_length > block.size())
            {
                return {};
            }

            output.insert(output.end(), block.begin() + static_cast<std::ptrdiff_t>(cursor), block.begin() + static_cast<std::ptrdiff_t>(cursor + literal_length));
            cursor += literal_length;

            if (cursor != block.size())
            {
                return {};
            }
        }

        return output;
    }

    std::vector<std::uint8_t> read_payload(std::istream &stream, std::uint32_t size)
    {
        std::vector<std::uint8_t> payload(size);
        stream.read(reinterpret_cast<char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!stream)
        {
            return {};
        }

        return payload;
    }

    alpha_recorder::FramePair make_pair(std::uint64_t sequence, std::uint64_t pts, const std::vector<std::uint8_t> &alpha_bytes)
    {
        alpha_recorder::FramePair pair;
        pair.sequence = sequence;
        pair.pts = pts;
        pair.rgb.width = 2;
        pair.rgb.height = 2;
        pair.rgb.stride = 8;
        pair.rgb.bytes = {0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U};
        pair.alpha.width = 2;
        pair.alpha.height = 2;
        pair.alpha.stride = 2;
        pair.alpha.bytes = alpha_bytes;
        return pair;
    }

    bool contains_text(const std::string &text, const std::string &needle)
    {
        return text.find(needle) != std::string::npos;
    }

} // namespace

int main()
{
    const std::filesystem::path temp_root = std::filesystem::temp_directory_path() / "alpha_recorder_sidecar_writer_test";
    std::error_code remove_error;
    std::filesystem::remove_all(temp_root, remove_error);
    std::filesystem::create_directories(temp_root);

    const std::filesystem::path sidecar_path = temp_root / "session.alpha";
    const std::filesystem::path manifest_path = temp_root / "session.manifest.json";

    alpha_recorder::AlphaLosslessWriter writer;
    if (writer.is_open())
    {
        std::cerr << "fresh writer should start closed\n";
        return 1;
    }

    if (!writer.open(sidecar_path, manifest_path))
    {
        std::cerr << "writer refused to open output files\n";
        return 2;
    }

    if (!writer.is_open() || writer.path() != sidecar_path || writer.manifest_path() != manifest_path)
    {
        std::cerr << "writer did not retain its output paths\n";
        return 3;
    }

    const alpha_recorder::FramePair first_pair = make_pair(7U, 1000U, {0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU});
    const alpha_recorder::FramePair second_pair = make_pair(8U, 1040U, {0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U, 0x29U, 0x2AU, 0x2BU, 0x2CU, 0x2DU, 0x2EU, 0x2FU, 0x30U, 0x31U});

    if (!writer.write_pair(first_pair))
    {
        std::cerr << "writer rejected the first pair\n";
        return 4;
    }

    if (!writer.write_frame(second_pair))
    {
        std::cerr << "writer rejected the second pair\n";
        return 5;
    }

    writer.close();

    if (writer.is_open())
    {
        std::cerr << "writer should be closed after finalize\n";
        return 6;
    }

    const alpha_recorder::AlphaSessionSummary &summary = writer.summary();
    if (summary.pair_count != 2U || summary.first_sequence != 7U || summary.last_sequence != 8U || summary.first_pts != 1000U || summary.last_pts != 1040U)
    {
        std::cerr << "writer summary did not capture the written pair metadata\n";
        return 7;
    }

    if (summary.alpha_uncompressed_bytes != 33U || summary.alpha_compressed_bytes != 37U)
    {
        std::cerr << "writer summary did not capture the compressed byte totals\n";
        return 8;
    }

    if (summary.index_entry_count != 2U || summary.index_offset <= alpha_recorder::alpha_container_header_size)
    {
        std::cerr << "writer summary did not capture the index layout\n";
        return 9;
    }

    if (!std::filesystem::exists(sidecar_path) || !std::filesystem::exists(manifest_path))
    {
        std::cerr << "writer did not create both output files\n";
        return 10;
    }

    if (summary.sidecar_size_bytes != std::filesystem::file_size(sidecar_path))
    {
        std::cerr << "summary sidecar size does not match the on-disk file size\n";
        return 11;
    }

    std::ifstream sidecar_stream(sidecar_path, std::ios::binary);
    if (!sidecar_stream)
    {
        std::cerr << "failed to open sidecar for verification\n";
        return 12;
    }

    if (!read_magic(sidecar_stream, alpha_recorder::alpha_container_magic))
    {
        std::cerr << "sidecar magic mismatch\n";
        return 13;
    }

    std::uint32_t container_version = 0;
    std::uint32_t container_header_size = 0;
    std::uint64_t record_count = 0;
    std::uint64_t index_offset = 0;
    std::uint64_t index_entry_count = 0;
    std::uint32_t record_header_size = 0;
    std::uint32_t index_entry_size = 0;
    std::uint32_t container_flags = 0;
    std::uint32_t container_reserved = 0;

    if (!read_le(sidecar_stream, container_version) || !read_le(sidecar_stream, container_header_size) || !read_le(sidecar_stream, record_count) || !read_le(sidecar_stream, index_offset) || !read_le(sidecar_stream, index_entry_count) || !read_le(sidecar_stream, record_header_size) || !read_le(sidecar_stream, index_entry_size) || !read_le(sidecar_stream, container_flags) || !read_le(sidecar_stream, container_reserved))
    {
        std::cerr << "failed to read the sidecar header\n";
        return 14;
    }

    if (container_version != alpha_recorder::alpha_container_format_version || container_header_size != alpha_recorder::alpha_container_header_size || record_count != 2U || index_offset != summary.index_offset || index_entry_count != 2U || record_header_size != alpha_recorder::alpha_record_header_size || index_entry_size != alpha_recorder::alpha_index_entry_size || container_flags != 0U || container_reserved != 0U)
    {
        std::cerr << "sidecar header fields are incorrect\n";
        return 15;
    }

    sidecar_stream.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
    if (!sidecar_stream)
    {
        std::cerr << "failed to seek to the index\n";
        return 16;
    }

    std::array<alpha_recorder::AlphaIndexEntry, 2> entries{};
    for (alpha_recorder::AlphaIndexEntry &entry : entries)
    {
        if (!read_le(sidecar_stream, entry.sequence) || !read_le(sidecar_stream, entry.pts) || !read_le(sidecar_stream, entry.record_offset) || !read_le(sidecar_stream, entry.record_header_size) || !read_le(sidecar_stream, entry.uncompressed_size) || !read_le(sidecar_stream, entry.compressed_size) || !read_le(sidecar_stream, entry.flags) || !read_le(sidecar_stream, entry.reserved))
        {
            std::cerr << "failed to read the index entries\n";
            return 17;
        }
    }

    if (entries[0].sequence != 7U || entries[0].pts != 1000U || entries[0].record_offset != alpha_recorder::alpha_container_header_size || entries[0].uncompressed_size != 15U || entries[0].compressed_size != 17U)
    {
        std::cerr << "first index entry is incorrect\n";
        return 18;
    }

    if (entries[1].sequence != 8U || entries[1].pts != 1040U || entries[1].record_offset != alpha_recorder::alpha_container_header_size + alpha_recorder::alpha_record_header_size + entries[0].compressed_size || entries[1].uncompressed_size != 18U || entries[1].compressed_size != 20U)
    {
        std::cerr << "second index entry is incorrect\n";
        return 19;
    }

    const alpha_recorder::FramePair expected_pairs[] = {first_pair, second_pair};
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        sidecar_stream.seekg(static_cast<std::streamoff>(entries[index].record_offset), std::ios::beg);
        if (!sidecar_stream)
        {
            std::cerr << "failed to seek to record " << index << '\n';
            return 20;
        }

        std::array<char, 8> record_magic{};
        sidecar_stream.read(record_magic.data(), static_cast<std::streamsize>(record_magic.size()));
        if (!sidecar_stream || record_magic != alpha_recorder::alpha_record_magic)
        {
            std::cerr << "record magic mismatch\n";
            return 21;
        }

        std::uint32_t record_version = 0;
        std::uint32_t record_header_size_value = 0;
        std::uint64_t record_sequence = 0;
        std::uint64_t record_pts = 0;
        std::uint32_t record_uncompressed_size = 0;
        std::uint32_t record_compressed_size = 0;
        std::uint32_t record_flags = 0;
        std::uint32_t record_reserved = 0;

        if (!read_le(sidecar_stream, record_version) || !read_le(sidecar_stream, record_header_size_value) || !read_le(sidecar_stream, record_sequence) || !read_le(sidecar_stream, record_pts) || !read_le(sidecar_stream, record_uncompressed_size) || !read_le(sidecar_stream, record_compressed_size) || !read_le(sidecar_stream, record_flags) || !read_le(sidecar_stream, record_reserved))
        {
            std::cerr << "failed to read a record header\n";
            return 22;
        }

        if (record_version != alpha_recorder::alpha_container_format_version || record_header_size_value != alpha_recorder::alpha_record_header_size || record_sequence != expected_pairs[index].sequence || record_pts != expected_pairs[index].pts || record_uncompressed_size != expected_pairs[index].alpha.bytes.size() || record_flags != alpha_recorder::alpha_record_flag_lz4_block || record_reserved != 0U)
        {
            std::cerr << "record header fields are incorrect\n";
            return 23;
        }

        const std::vector<std::uint8_t> compressed_payload = read_payload(sidecar_stream, record_compressed_size);
        if (compressed_payload.size() != record_compressed_size)
        {
            std::cerr << "failed to read the record payload\n";
            return 24;
        }

        const std::vector<std::uint8_t> decoded_payload = decode_lz4_literal_block(compressed_payload);
        if (decoded_payload != expected_pairs[index].alpha.bytes)
        {
            std::cerr << "decoded alpha payload does not match the original bytes\n";
            return 25;
        }
    }

    std::ifstream manifest_stream(manifest_path);
    if (!manifest_stream)
    {
        std::cerr << "failed to open manifest for verification\n";
        return 26;
    }

    const std::string manifest_text((std::istreambuf_iterator<char>(manifest_stream)), std::istreambuf_iterator<char>());
    if (!contains_text(manifest_text, "\"schema\": \"alpha_recorder.session_summary.v1\"") || !contains_text(manifest_text, "\"project_name\": \"alpha_recorder\"") || !contains_text(manifest_text, "\"project_version\": \"0.1.0\"") || !contains_text(manifest_text, "\"finalization_format\": \"mask_png_mov\"") || !contains_text(manifest_text, "\"pair_count\": 2") || !contains_text(manifest_text, "\"record_count\": 2") || !contains_text(manifest_text, "\"first_sequence\": 7") || !contains_text(manifest_text, "\"last_sequence\": 8") || !contains_text(manifest_text, "\"first_pts\": 1000") || !contains_text(manifest_text, "\"last_pts\": 1040") || !contains_text(manifest_text, sidecar_path.filename().generic_string()) || !contains_text(manifest_text, manifest_path.filename().generic_string()))
    {
        std::cerr << "manifest content is missing expected session metadata\n";
        return 27;
    }

    alpha_recorder::AlphaSessionSummary overload_summary = summary;
    overload_summary.manifest_path = temp_root / "session.overload.manifest.json";
    overload_summary.finalization_format = "mask_hevc_nvenc";
    overload_summary.overload_detected = true;

    alpha_recorder::ManifestWriter manifest_writer;
    if (!manifest_writer.write(overload_summary))
    {
        std::cerr << "failed to write a manifest with overload metadata\n";
        return 28;
    }

    std::ifstream overload_manifest_stream(overload_summary.manifest_path);
    if (!overload_manifest_stream)
    {
        std::cerr << "failed to open overload manifest for verification\n";
        return 29;
    }

    const std::string overload_manifest_text((std::istreambuf_iterator<char>(overload_manifest_stream)), std::istreambuf_iterator<char>());
    if (!contains_text(overload_manifest_text, "\"finalization_format\": \"mask_hevc_nvenc\"") || !contains_text(overload_manifest_text, "ERR_OVERLOAD"))
    {
        std::cerr << "overload manifest did not include the expected metadata\n";
        return 30;
    }

#ifdef _WIN32
    const std::filesystem::path locked_manifest_path = temp_root / "session.locked.manifest.json";
    alpha_recorder::AlphaSessionSummary locked_summary = summary;
    locked_summary.manifest_path = locked_manifest_path;
    locked_summary.finalization_format = "mask_png_mov";

    if (!manifest_writer.write(locked_summary))
    {
        std::cerr << "failed to write the locked baseline manifest\n";
        return 31;
    }

    std::ifstream locked_manifest_stream(locked_manifest_path);
    if (!locked_manifest_stream)
    {
        std::cerr << "failed to open the locked baseline manifest\n";
        return 32;
    }

    const std::string locked_manifest_text((std::istreambuf_iterator<char>(locked_manifest_stream)), std::istreambuf_iterator<char>());

    HANDLE locked_manifest_handle = CreateFileW(locked_manifest_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked_manifest_handle == INVALID_HANDLE_VALUE)
    {
        std::cerr << "failed to lock the manifest file for the failure-safety check\n";
        return 33;
    }

    alpha_recorder::AlphaSessionSummary locked_update_summary = locked_summary;
    locked_update_summary.finalization_format = "mask_hevc_nvenc";
    locked_update_summary.overload_detected = true;

    if (manifest_writer.write(locked_update_summary))
    {
        CloseHandle(locked_manifest_handle);
        std::cerr << "manifest rewrite should have failed while the file was locked\n";
        return 34;
    }

    CloseHandle(locked_manifest_handle);

    std::ifstream locked_manifest_verify_stream(locked_manifest_path);
    if (!locked_manifest_verify_stream)
    {
        std::cerr << "failed to reopen the locked manifest after the failed rewrite\n";
        return 35;
    }

    const std::string locked_manifest_verify_text((std::istreambuf_iterator<char>(locked_manifest_verify_stream)), std::istreambuf_iterator<char>());
    if (locked_manifest_verify_text != locked_manifest_text)
    {
        std::cerr << "locked manifest contents changed after the failed rewrite\n";
        return 36;
    }
#endif

    std::cout << "sidecar writer roundtrip test passed\n";
    return 0;
}
