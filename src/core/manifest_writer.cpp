#include "alpha_recorder/manifest_writer.hpp"

#include <jansson.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
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

        std::filesystem::path manifest_temp_path(const std::filesystem::path &manifest_path)
        {
            std::filesystem::path temp_path = manifest_path;
            temp_path += ".tmp";
            return temp_path;
        }

        void remove_manifest_file(const std::filesystem::path &path) noexcept
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }

        template <typename ValueT>
        void write_json_number(std::ostream &stream, std::string_view key, ValueT value, bool trailing_comma)
        {
            stream << "  \"" << key << "\": " << value;
            stream << (trailing_comma ? ",\n" : "\n");
        }

        bool read_json_string(json_t *object, const char *key, std::string &value, std::string *error_message)
        {
            json_t *const entry = json_object_get(object, key);
            const char *const text = json_string_value(entry);
            if (text == nullptr)
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"manifest field \""} + key + "\" is missing or not a string";
                }
                return false;
            }

            value = text;
            return true;
        }

        bool read_json_u64(json_t *object, const char *key, std::uint64_t &value, std::string *error_message)
        {
            json_t *const entry = json_object_get(object, key);
            if (entry == nullptr || !json_is_integer(entry))
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"manifest field \""} + key + "\" is missing or not an integer";
                }
                return false;
            }

            const json_int_t parsed_value = json_integer_value(entry);
            if (parsed_value < 0)
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"manifest field \""} + key + "\" is negative";
                }
                return false;
            }

            value = static_cast<std::uint64_t>(parsed_value);
            return true;
        }

        bool read_json_u32(json_t *object, const char *key, std::uint32_t &value, std::string *error_message)
        {
            std::uint64_t parsed_value = 0;
            if (!read_json_u64(object, key, parsed_value, error_message))
            {
                return false;
            }

            if (parsed_value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"manifest field \""} + key + "\" is out of range";
                }
                return false;
            }

            value = static_cast<std::uint32_t>(parsed_value);
            return true;
        }

        bool read_manifest_summary(json_t *root, const std::filesystem::path &manifest_path, AlphaSessionSummary &summary,
                                   std::string *error_message)
        {
            summary = AlphaSessionSummary{};

            std::string schema;
            if (!read_json_string(root, "schema", schema, error_message))
            {
                return false;
            }

            if (schema != manifest_schema_name())
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"manifest schema does not match "} + std::string(manifest_schema_name());
                }
                return false;
            }

            std::string sidecar_path_text;
            std::string manifest_path_text;
            std::uint64_t pair_count = 0;
            std::uint64_t record_count = 0;

            if (!read_json_u32(root, "container_format_version", summary.container_format_version, error_message) ||
                !read_json_string(root, "project_name", summary.project_name, error_message) ||
                !read_json_string(root, "project_version", summary.project_version, error_message) ||
                !read_json_string(root, "finalization_format", summary.finalization_format, error_message) ||
                !read_json_string(root, "sidecar_path", sidecar_path_text, error_message) ||
                !read_json_string(root, "manifest_path", manifest_path_text, error_message) ||
                !read_json_u64(root, "pair_count", pair_count, error_message) ||
                !read_json_u64(root, "record_count", record_count, error_message) ||
                !read_json_u64(root, "first_sequence", summary.first_sequence, error_message) ||
                !read_json_u64(root, "last_sequence", summary.last_sequence, error_message) ||
                !read_json_u64(root, "first_pts", summary.first_pts, error_message) ||
                !read_json_u64(root, "last_pts", summary.last_pts, error_message) ||
                !read_json_u64(root, "alpha_uncompressed_bytes", summary.alpha_uncompressed_bytes, error_message) ||
                !read_json_u64(root, "alpha_compressed_bytes", summary.alpha_compressed_bytes, error_message) ||
                !read_json_u64(root, "index_offset", summary.index_offset, error_message) ||
                !read_json_u64(root, "index_entry_count", summary.index_entry_count, error_message) ||
                !read_json_u64(root, "sidecar_size_bytes", summary.sidecar_size_bytes, error_message))
            {
                return false;
            }

            summary.pair_count = pair_count;
            if (record_count != pair_count)
            {
                if (error_message != nullptr)
                {
                    *error_message = "manifest pair_count does not match record_count";
                }
                return false;
            }

            summary.sidecar_path = std::filesystem::u8path(sidecar_path_text);
            summary.manifest_path = std::filesystem::u8path(manifest_path_text);
            summary.overload_detected = false;

            json_t *const status_flags = json_object_get(root, "status_flags");
            if (status_flags != nullptr && json_is_array(status_flags))
            {
                const std::size_t status_count = json_array_size(status_flags);
                for (std::size_t index = 0; index < status_count; ++index)
                {
                    json_t *const status_value = json_array_get(status_flags, index);
                    const char *const status_text = json_string_value(status_value);
                    if (status_text != nullptr && std::string_view{status_text} == alpha_overload_status_flag())
                    {
                        summary.overload_detected = true;
                    }
                }
            }

            (void)manifest_path;
            return true;
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

            const std::filesystem::path temp_manifest_path = manifest_temp_path(summary.manifest_path);

            {
                std::ofstream stream(temp_manifest_path, std::ios::binary | std::ios::trunc);
                if (!stream)
                {
                    return false;
                }

                stream << "{\n";
                stream << "  \"schema\": ";
                write_json_string(stream, manifest_schema_name());
                stream << ",\n";
                write_json_number(stream, "container_format_version", summary.container_format_version, true);
                stream << "  \"project_name\": ";
                write_json_string(stream, summary.project_name);
                stream << ",\n";
                stream << "  \"project_version\": ";
                write_json_string(stream, summary.project_version);
                stream << ",\n";
                stream << "  \"finalization_format\": ";
                write_json_string(stream, summary.finalization_format);
                stream << ",\n";
                stream << "  \"sidecar_path\": ";
                write_json_string(stream, summary.sidecar_path.generic_u8string());
                stream << ",\n";
                stream << "  \"manifest_path\": ";
                write_json_string(stream, summary.manifest_path.generic_u8string());
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
                write_json_number(stream, "sidecar_size_bytes", summary.sidecar_size_bytes, summary.overload_detected);
                if (summary.overload_detected)
                {
                    stream << "  \"status_flags\": [";
                    write_json_string(stream, "ERR_OVERLOAD");
                    stream << "]\n";
                }
                stream << "}\n";

                stream.flush();
                if (!stream)
                {
                    remove_manifest_file(temp_manifest_path);
                    return false;
                }
            }

            std::error_code rename_error;
            std::filesystem::rename(temp_manifest_path, summary.manifest_path, rename_error);
            if (rename_error)
            {
                remove_manifest_file(temp_manifest_path);
                return false;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ManifestWriter::read(const std::filesystem::path &manifest_path, AlphaSessionSummary &summary,
                              std::string *error_message) noexcept
    {
        try
        {
            if (manifest_path.empty())
            {
                if (error_message != nullptr)
                {
                    *error_message = "manifest path is empty";
                }
                return false;
            }

            std::ifstream stream(manifest_path, std::ios::binary);
            if (!stream)
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"failed to open manifest: "} + manifest_path.generic_u8string();
                }
                return false;
            }

            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (text.empty() && !stream.eof())
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"failed to read manifest: "} + manifest_path.generic_u8string();
                }
                return false;
            }

            json_error_t json_error{};
            json_t *const root = json_loads(text.c_str(), 0, &json_error);
            if (root == nullptr)
            {
                if (error_message != nullptr)
                {
                    *error_message = std::string{"failed to parse manifest JSON: "} + json_error.text;
                }
                return false;
            }

            const bool success = read_manifest_summary(root, manifest_path, summary, error_message);
            json_decref(root);
            return success;
        }
        catch (...)
        {
            if (error_message != nullptr)
            {
                *error_message = "manifest parsing failed due to an unexpected error";
            }
            return false;
        }
    }

} // namespace alpha_recorder
