#include "alpha_recorder/manifest_writer.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

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

        void write_json_string(std::ostream &stream, std::string_view text)
        {
            stream.put('"');

            for (const char character : text)
            {
                switch (character)
                {
                case '"':
                    stream << "\\\"";
                    break;
                case '\\':
                    stream << "\\\\";
                    break;
                case '\b':
                    stream << "\\b";
                    break;
                case '\f':
                    stream << "\\f";
                    break;
                case '\n':
                    stream << "\\n";
                    break;
                case '\r':
                    stream << "\\r";
                    break;
                case '\t':
                    stream << "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(character) < 0x20U)
                    {
                        static constexpr char hex_digits[] = "0123456789abcdef";
                        stream << "\\u00";
                        stream.put(hex_digits[(static_cast<unsigned char>(character) >> 4U) & 0x0FU]);
                        stream.put(hex_digits[static_cast<unsigned char>(character) & 0x0FU]);
                    }
                    else
                    {
                        stream.put(character);
                    }
                    break;
                }
            }

            stream.put('"');
        }

        template <typename ValueT>
        void write_json_number(std::ostream &stream, std::string_view key, ValueT value, bool trailing_comma)
        {
            stream << "  \"" << key << "\": " << value;
            stream << (trailing_comma ? ",\n" : "\n");
        }

    } // namespace

    bool ManifestWriter::write(const AlphaSessionSummary &summary) noexcept
    {
        try
        {
            if (summary.manifest_path.empty())
            {
                return false;
            }

            if (!ensure_parent_directory(summary.manifest_path))
            {
                return false;
            }

            std::ofstream stream(summary.manifest_path, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return false;
            }

            stream << "{\n";
            stream << "  \"schema\": ";
            write_json_string(stream, "alpha_recorder.session_summary.v1");
            stream << ",\n";
            write_json_number(stream, "container_format_version", summary.container_format_version, true);
            stream << "  \"project_name\": ";
            write_json_string(stream, summary.project_name);
            stream << ",\n";
            stream << "  \"project_version\": ";
            write_json_string(stream, summary.project_version);
            stream << ",\n";
            stream << "  \"sidecar_path\": ";
            write_json_string(stream, summary.sidecar_path.generic_string());
            stream << ",\n";
            stream << "  \"manifest_path\": ";
            write_json_string(stream, summary.manifest_path.generic_string());
            stream << ",\n";
            write_json_number(stream, "pair_count", summary.pair_count, true);
            write_json_number(stream, "record_count", summary.pair_count, true);
            write_json_number(stream, "first_sequence", summary.first_sequence, true);
            write_json_number(stream, "last_sequence", summary.last_sequence, true);
            write_json_number(stream, "first_pts", summary.first_pts, true);
            write_json_number(stream, "last_pts", summary.last_pts, true);
            write_json_number(stream, "alpha_uncompressed_bytes", summary.alpha_uncompressed_bytes, true);
            write_json_number(stream, "alpha_compressed_bytes", summary.alpha_compressed_bytes, true);
            write_json_number(stream, "index_offset", summary.index_offset, true);
            write_json_number(stream, "index_entry_count", summary.index_entry_count, true);
            write_json_number(stream, "sidecar_size_bytes", summary.sidecar_size_bytes, false);
            stream << "}\n";

            return static_cast<bool>(stream);
        }
        catch (...)
        {
            return false;
        }
    }

} // namespace alpha_recorder