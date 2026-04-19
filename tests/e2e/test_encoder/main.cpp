#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "alpha_recorder/e2e_scenario.hpp"
#include "alpha_recorder/sidecar_writer.hpp"

namespace
{

    std::filesystem::path read_argument(int argc, char **argv, std::string_view name)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view current = argv[index];
            if (current == name && index + 1 < argc)
            {
                return std::filesystem::path{argv[index + 1]};
            }
        }

        return {};
    }

    std::filesystem::path read_environment_path(const char *name)
    {
        size_t required_size = 0;
        if (getenv_s(&required_size, nullptr, 0, name) != 0 || required_size == 0U)
        {
            return {};
        }

        std::string value(required_size, '\0');
        if (getenv_s(&required_size, value.data(), value.size(), name) != 0 || required_size == 0U)
        {
            return {};
        }

        if (!value.empty() && value.back() == '\0')
        {
            value.pop_back();
        }

        return std::filesystem::path{value};
    }

    std::filesystem::path resolve_artifact_root(int argc, char **argv)
    {
        const std::filesystem::path cli_root = read_argument(argc, argv, "--artifact-root");
        if (!cli_root.empty())
        {
            return cli_root;
        }

        const std::filesystem::path env_root = read_environment_path("ALPHA_RECORDER_E2E_ARTIFACT_ROOT");
        if (!env_root.empty())
        {
            return env_root;
        }

        const std::filesystem::path stage_root = read_environment_path("ALPHA_RECORDER_STAGE_DIR");
        if (!stage_root.empty())
        {
            return stage_root / "e2e";
        }

        std::error_code temp_error;
        const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_error);
        if (!temp_error && !temp_root.empty())
        {
            return temp_root / "alpha_recorder_e2e";
        }

        return std::filesystem::current_path() / "alpha_recorder_e2e";
    }

    bool read_file_bytes(const std::filesystem::path &file_path, std::vector<std::uint8_t> &bytes)
    {
        std::ifstream stream(file_path, std::ios::binary);
        if (!stream)
        {
            return false;
        }

        bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        return true;
    }

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

    bool contains_text(const std::string &text, const std::string &needle)
    {
        return text.find(needle) != std::string::npos;
    }

    std::string read_text_file(const std::filesystem::path &file_path)
    {
        std::ifstream stream(file_path, std::ios::binary);
        if (!stream)
        {
            return {};
        }

        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    void skip_json_whitespace(std::string_view text, std::size_t &pos)
    {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0)
        {
            ++pos;
        }
    }

    bool parse_hex_digit(char character, std::uint32_t &value)
    {
        if (character >= '0' && character <= '9')
        {
            value = static_cast<std::uint32_t>(character - '0');
            return true;
        }

        if (character >= 'a' && character <= 'f')
        {
            value = static_cast<std::uint32_t>(10 + character - 'a');
            return true;
        }

        if (character >= 'A' && character <= 'F')
        {
            value = static_cast<std::uint32_t>(10 + character - 'A');
            return true;
        }

        return false;
    }

    bool append_utf8(std::string &text, std::uint32_t code_point)
    {
        if (code_point <= 0x7FU)
        {
            text.push_back(static_cast<char>(code_point));
            return true;
        }

        if (code_point <= 0x7FFU)
        {
            text.push_back(static_cast<char>(0xC0U | ((code_point >> 6U) & 0x1FU)));
            text.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            return true;
        }

        if (code_point <= 0xFFFFU)
        {
            text.push_back(static_cast<char>(0xE0U | ((code_point >> 12U) & 0x0FU)));
            text.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            text.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            return true;
        }

        if (code_point <= 0x10FFFFU)
        {
            text.push_back(static_cast<char>(0xF0U | ((code_point >> 18U) & 0x07U)));
            text.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            text.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            text.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            return true;
        }

        return false;
    }

    bool consume_json_literal(std::string_view text, std::size_t &pos, std::string_view literal, std::string &error)
    {
        if (text.size() - pos < literal.size() || text.substr(pos, literal.size()) != literal)
        {
            error.assign("invalid JSON literal in manifest");
            return false;
        }

        pos += literal.size();
        return true;
    }

    bool parse_json_string(std::string_view text, std::size_t &pos, std::string &value, std::string &error)
    {
        if (pos >= text.size() || text[pos] != '"')
        {
            error.assign("expected JSON string in manifest");
            return false;
        }

        ++pos;
        value.clear();

        while (pos < text.size())
        {
            const char character = text[pos++];
            if (character == '"')
            {
                return true;
            }

            if (character == '\\')
            {
                if (pos >= text.size())
                {
                    error.assign("unterminated escape sequence in manifest string");
                    return false;
                }

                const char escape = text[pos++];
                switch (escape)
                {
                case '"':
                    value.push_back('"');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '/':
                    value.push_back('/');
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'u':
                {
                    if (pos + 4U > text.size())
                    {
                        error.assign("unterminated unicode escape in manifest string");
                        return false;
                    }

                    std::uint32_t code_point = 0;
                    for (std::size_t index = 0; index < 4U; ++index)
                    {
                        std::uint32_t digit = 0;
                        if (!parse_hex_digit(text[pos + index], digit))
                        {
                            error.assign("invalid unicode escape in manifest string");
                            return false;
                        }

                        code_point = static_cast<std::uint32_t>((code_point << 4U) | digit);
                    }

                    pos += 4U;

                    if (code_point >= 0xD800U && code_point <= 0xDBFFU)
                    {
                        if (pos + 6U > text.size() || text[pos] != '\\' || text[pos + 1U] != 'u')
                        {
                            error.assign("invalid unicode surrogate pair in manifest string");
                            return false;
                        }

                        pos += 2U;

                        std::uint32_t low_surrogate = 0;
                        for (std::size_t index = 0; index < 4U; ++index)
                        {
                            std::uint32_t digit = 0;
                            if (!parse_hex_digit(text[pos + index], digit))
                            {
                                error.assign("invalid unicode surrogate pair in manifest string");
                                return false;
                            }

                            low_surrogate = static_cast<std::uint32_t>((low_surrogate << 4U) | digit);
                        }

                        pos += 4U;
                        if (low_surrogate < 0xDC00U || low_surrogate > 0xDFFFU)
                        {
                            error.assign("invalid unicode surrogate pair in manifest string");
                            return false;
                        }

                        code_point = 0x10000U + (((code_point - 0xD800U) << 10U) | (low_surrogate - 0xDC00U));
                    }
                    else if (code_point >= 0xDC00U && code_point <= 0xDFFFU)
                    {
                        error.assign("unexpected low surrogate in manifest string");
                        return false;
                    }

                    if (!append_utf8(value, code_point))
                    {
                        error.assign("invalid unicode code point in manifest string");
                        return false;
                    }
                    break;
                }
                default:
                    error.assign("invalid escape sequence in manifest string");
                    return false;
                }

                continue;
            }

            if (static_cast<unsigned char>(character) < 0x20U)
            {
                error.assign("unescaped control character in manifest string");
                return false;
            }

            value.push_back(character);
        }

        error.assign("unterminated JSON string in manifest");
        return false;
    }

    bool parse_json_uint64(std::string_view text, std::size_t &pos, std::uint64_t &value, std::string &error)
    {
        if (pos >= text.size() || std::isdigit(static_cast<unsigned char>(text[pos])) == 0)
        {
            error.assign("expected unsigned integer in manifest");
            return false;
        }

        if (text[pos] == '0')
        {
            ++pos;
            if (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0)
            {
                error.assign("manifest integer values must not contain leading zeros");
                return false;
            }

            value = 0U;
            return true;
        }

        std::uint64_t result = 0;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0)
        {
            const std::uint64_t digit = static_cast<std::uint64_t>(text[pos] - '0');
            if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            {
                error.assign("manifest integer value overflowed");
                return false;
            }

            result = (result * 10U) + digit;
            ++pos;
        }

        value = result;
        return true;
    }

    bool skip_json_number(std::string_view text, std::size_t &pos, std::string &error)
    {
        if (pos < text.size() && text[pos] == '-')
        {
            ++pos;
        }

        if (pos >= text.size())
        {
            error.assign("unexpected end of manifest while parsing JSON number");
            return false;
        }

        if (text[pos] == '0')
        {
            ++pos;
        }
        else if (std::isdigit(static_cast<unsigned char>(text[pos])) != 0)
        {
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0)
            {
                ++pos;
            }
        }
        else
        {
            error.assign("invalid JSON number in manifest");
            return false;
        }

        if (pos < text.size() && text[pos] == '.')
        {
            ++pos;
            if (pos >= text.size() || std::isdigit(static_cast<unsigned char>(text[pos])) == 0)
            {
                error.assign("invalid JSON number in manifest");
                return false;
            }

            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0)
            {
                ++pos;
            }
        }

        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E'))
        {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
            {
                ++pos;
            }

            if (pos >= text.size() || std::isdigit(static_cast<unsigned char>(text[pos])) == 0)
            {
                error.assign("invalid JSON number in manifest");
                return false;
            }

            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0)
            {
                ++pos;
            }
        }

        return true;
    }

    bool skip_json_value(std::string_view text, std::size_t &pos, std::string &error);

    bool skip_json_object(std::string_view text, std::size_t &pos, std::string &error)
    {
        if (pos >= text.size() || text[pos] != '{')
        {
            error.assign("expected JSON object in manifest");
            return false;
        }

        ++pos;
        skip_json_whitespace(text, pos);
        if (pos < text.size() && text[pos] == '}')
        {
            ++pos;
            return true;
        }

        while (true)
        {
            std::string ignored_key;
            if (!parse_json_string(text, pos, ignored_key, error))
            {
                return false;
            }

            skip_json_whitespace(text, pos);
            if (pos >= text.size() || text[pos] != ':')
            {
                error.assign("expected ':' after manifest object key");
                return false;
            }

            ++pos;
            if (!skip_json_value(text, pos, error))
            {
                return false;
            }

            skip_json_whitespace(text, pos);
            if (pos >= text.size())
            {
                error.assign("unexpected end of manifest while parsing object");
                return false;
            }

            if (text[pos] == ',')
            {
                ++pos;
                skip_json_whitespace(text, pos);
                continue;
            }

            if (text[pos] == '}')
            {
                ++pos;
                return true;
            }

            error.assign("expected ',' or '}' in manifest object");
            return false;
        }
    }

    bool skip_json_array(std::string_view text, std::size_t &pos, std::string &error)
    {
        if (pos >= text.size() || text[pos] != '[')
        {
            error.assign("expected JSON array in manifest");
            return false;
        }

        ++pos;
        skip_json_whitespace(text, pos);
        if (pos < text.size() && text[pos] == ']')
        {
            ++pos;
            return true;
        }

        while (true)
        {
            if (!skip_json_value(text, pos, error))
            {
                return false;
            }

            skip_json_whitespace(text, pos);
            if (pos >= text.size())
            {
                error.assign("unexpected end of manifest while parsing array");
                return false;
            }

            if (text[pos] == ',')
            {
                ++pos;
                skip_json_whitespace(text, pos);
                continue;
            }

            if (text[pos] == ']')
            {
                ++pos;
                return true;
            }

            error.assign("expected ',' or ']' in manifest array");
            return false;
        }
    }

    bool skip_json_value(std::string_view text, std::size_t &pos, std::string &error)
    {
        skip_json_whitespace(text, pos);
        if (pos >= text.size())
        {
            error.assign("unexpected end of manifest while parsing JSON value");
            return false;
        }

        switch (text[pos])
        {
        case '"':
        {
            std::string ignored_value;
            return parse_json_string(text, pos, ignored_value, error);
        }
        case '{':
            return skip_json_object(text, pos, error);
        case '[':
            return skip_json_array(text, pos, error);
        case 't':
            return consume_json_literal(text, pos, "true", error);
        case 'f':
            return consume_json_literal(text, pos, "false", error);
        case 'n':
            return consume_json_literal(text, pos, "null", error);
        default:
            return skip_json_number(text, pos, error);
        }
    }

    struct ManifestFields
    {
        std::string schema{};
        std::uint64_t container_format_version = 0;
        std::string project_name{};
        std::string project_version{};
        std::string sidecar_path{};
        std::string manifest_path{};
        std::uint64_t pair_count = 0;
        std::uint64_t record_count = 0;
        std::uint64_t first_sequence = 0;
        std::uint64_t last_sequence = 0;
        std::uint64_t first_pts = 0;
        std::uint64_t last_pts = 0;
        std::uint64_t alpha_uncompressed_bytes = 0;
        std::uint64_t alpha_compressed_bytes = 0;
        std::uint64_t index_offset = 0;
        std::uint64_t index_entry_count = 0;
        std::uint64_t sidecar_size_bytes = 0;
        bool schema_seen = false;
        bool container_format_version_seen = false;
        bool project_name_seen = false;
        bool project_version_seen = false;
        bool sidecar_path_seen = false;
        bool manifest_path_seen = false;
        bool pair_count_seen = false;
        bool record_count_seen = false;
        bool first_sequence_seen = false;
        bool last_sequence_seen = false;
        bool first_pts_seen = false;
        bool last_pts_seen = false;
        bool alpha_uncompressed_bytes_seen = false;
        bool alpha_compressed_bytes_seen = false;
        bool index_offset_seen = false;
        bool index_entry_count_seen = false;
        bool sidecar_size_bytes_seen = false;
    };

    bool parse_manifest_json(std::string_view text, ManifestFields &manifest, std::string &error)
    {
        std::size_t pos = 0;
        skip_json_whitespace(text, pos);
        if (pos >= text.size() || text[pos] != '{')
        {
            error.assign("manifest is not a JSON object");
            return false;
        }

        ++pos;
        skip_json_whitespace(text, pos);
        if (pos < text.size() && text[pos] == '}')
        {
            error.assign("manifest is missing required fields");
            return false;
        }

        auto require_string = [&](std::string &target, bool &seen, std::string_view field_name) -> bool
        {
            if (seen)
            {
                error.assign("duplicate manifest field: ");
                error.append(field_name);
                return false;
            }

            if (!parse_json_string(text, pos, target, error))
            {
                return false;
            }

            seen = true;
            return true;
        };

        auto require_number = [&](std::uint64_t &target, bool &seen, std::string_view field_name) -> bool
        {
            if (seen)
            {
                error.assign("duplicate manifest field: ");
                error.append(field_name);
                return false;
            }

            if (!parse_json_uint64(text, pos, target, error))
            {
                return false;
            }

            seen = true;
            return true;
        };

        while (true)
        {
            std::string key;
            if (!parse_json_string(text, pos, key, error))
            {
                return false;
            }

            skip_json_whitespace(text, pos);
            if (pos >= text.size() || text[pos] != ':')
            {
                error.assign("expected ':' after manifest field name");
                return false;
            }

            ++pos;
            skip_json_whitespace(text, pos);

            if (key == "schema")
            {
                if (!require_string(manifest.schema, manifest.schema_seen, key))
                {
                    return false;
                }
            }
            else if (key == "container_format_version")
            {
                if (!require_number(manifest.container_format_version, manifest.container_format_version_seen, key))
                {
                    return false;
                }
            }
            else if (key == "project_name")
            {
                if (!require_string(manifest.project_name, manifest.project_name_seen, key))
                {
                    return false;
                }
            }
            else if (key == "project_version")
            {
                if (!require_string(manifest.project_version, manifest.project_version_seen, key))
                {
                    return false;
                }
            }
            else if (key == "sidecar_path")
            {
                if (!require_string(manifest.sidecar_path, manifest.sidecar_path_seen, key))
                {
                    return false;
                }
            }
            else if (key == "manifest_path")
            {
                if (!require_string(manifest.manifest_path, manifest.manifest_path_seen, key))
                {
                    return false;
                }
            }
            else if (key == "pair_count")
            {
                if (!require_number(manifest.pair_count, manifest.pair_count_seen, key))
                {
                    return false;
                }
            }
            else if (key == "record_count")
            {
                if (!require_number(manifest.record_count, manifest.record_count_seen, key))
                {
                    return false;
                }
            }
            else if (key == "first_sequence")
            {
                if (!require_number(manifest.first_sequence, manifest.first_sequence_seen, key))
                {
                    return false;
                }
            }
            else if (key == "last_sequence")
            {
                if (!require_number(manifest.last_sequence, manifest.last_sequence_seen, key))
                {
                    return false;
                }
            }
            else if (key == "first_pts")
            {
                if (!require_number(manifest.first_pts, manifest.first_pts_seen, key))
                {
                    return false;
                }
            }
            else if (key == "last_pts")
            {
                if (!require_number(manifest.last_pts, manifest.last_pts_seen, key))
                {
                    return false;
                }
            }
            else if (key == "alpha_uncompressed_bytes")
            {
                if (!require_number(manifest.alpha_uncompressed_bytes, manifest.alpha_uncompressed_bytes_seen, key))
                {
                    return false;
                }
            }
            else if (key == "alpha_compressed_bytes")
            {
                if (!require_number(manifest.alpha_compressed_bytes, manifest.alpha_compressed_bytes_seen, key))
                {
                    return false;
                }
            }
            else if (key == "index_offset")
            {
                if (!require_number(manifest.index_offset, manifest.index_offset_seen, key))
                {
                    return false;
                }
            }
            else if (key == "index_entry_count")
            {
                if (!require_number(manifest.index_entry_count, manifest.index_entry_count_seen, key))
                {
                    return false;
                }
            }
            else if (key == "sidecar_size_bytes")
            {
                if (!require_number(manifest.sidecar_size_bytes, manifest.sidecar_size_bytes_seen, key))
                {
                    return false;
                }
            }
            else
            {
                if (!skip_json_value(text, pos, error))
                {
                    return false;
                }
            }

            skip_json_whitespace(text, pos);
            if (pos >= text.size())
            {
                error.assign("unexpected end of manifest while parsing fields");
                return false;
            }

            if (text[pos] == ',')
            {
                ++pos;
                skip_json_whitespace(text, pos);
                continue;
            }

            if (text[pos] == '}')
            {
                ++pos;
                break;
            }

            error.assign("expected ',' or '}' after manifest field");
            return false;
        }

        skip_json_whitespace(text, pos);
        if (pos != text.size())
        {
            error.assign("unexpected trailing content after manifest JSON");
            return false;
        }

        if (!manifest.schema_seen || !manifest.container_format_version_seen || !manifest.project_name_seen || !manifest.project_version_seen || !manifest.sidecar_path_seen || !manifest.manifest_path_seen || !manifest.pair_count_seen || !manifest.record_count_seen || !manifest.first_sequence_seen || !manifest.last_sequence_seen || !manifest.first_pts_seen || !manifest.last_pts_seen || !manifest.alpha_uncompressed_bytes_seen || !manifest.alpha_compressed_bytes_seen || !manifest.index_offset_seen || !manifest.index_entry_count_seen || !manifest.sidecar_size_bytes_seen)
        {
            error.assign("manifest is missing required fields");
            return false;
        }

        return true;
    }

    std::vector<std::uint8_t> expected_rgb_bytes(const alpha_recorder::e2e::E2EScenario &scenario)
    {
        std::vector<std::uint8_t> bytes;
        for (std::uint64_t index = 0; index < scenario.expected_pair_count; ++index)
        {
            const alpha_recorder::FramePair pair = alpha_recorder::e2e::make_test_pair(index);
            bytes.insert(bytes.end(), pair.rgb.bytes.begin(), pair.rgb.bytes.end());
        }

        return bytes;
    }

} // namespace

int main(int argc, char **argv)
{
    const std::filesystem::path scenario_path = read_argument(argc, argv, "--scenario");
    if (scenario_path.empty())
    {
        std::cerr << "missing required --scenario argument\n";
        return 1;
    }

    if (!std::filesystem::exists(scenario_path))
    {
        std::cerr << "scenario file does not exist: " << scenario_path.string() << '\n';
        return 2;
    }

    alpha_recorder::e2e::E2EScenario scenario;
    std::string scenario_error;
    if (!alpha_recorder::e2e::load_scenario(scenario_path, scenario, scenario_error))
    {
        std::cerr << scenario_error << '\n';
        return 3;
    }

    const std::filesystem::path artifact_root = resolve_artifact_root(argc, argv);
    const std::filesystem::path output_root = alpha_recorder::e2e::resolve_output_root(artifact_root, scenario);
    const std::filesystem::path rgb_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.rgb_artifact);
    const std::filesystem::path sidecar_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_sidecar);
    const std::filesystem::path manifest_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_manifest);

    if (!std::filesystem::exists(rgb_path))
    {
        std::cerr << "RGB artifact does not exist: " << rgb_path.string() << '\n';
        return 4;
    }

    if (!std::filesystem::exists(sidecar_path))
    {
        std::cerr << "alpha sidecar does not exist: " << sidecar_path.string() << '\n';
        return 5;
    }

    if (!std::filesystem::exists(manifest_path))
    {
        std::cerr << "manifest does not exist: " << manifest_path.string() << '\n';
        return 6;
    }

    std::vector<std::uint8_t> rgb_bytes;
    if (!read_file_bytes(rgb_path, rgb_bytes))
    {
        std::cerr << "failed to read RGB artifact: " << rgb_path.string() << '\n';
        return 7;
    }

    const std::vector<std::uint8_t> expected_rgb = expected_rgb_bytes(scenario);
    if (rgb_bytes != expected_rgb)
    {
        std::cerr << "RGB artifact bytes do not match the expected accepted pairs\n";
        return 8;
    }

    std::ifstream sidecar_stream(sidecar_path, std::ios::binary);
    if (!sidecar_stream)
    {
        std::cerr << "failed to open sidecar for verification\n";
        return 9;
    }

    if (!read_magic(sidecar_stream, alpha_recorder::alpha_container_magic))
    {
        std::cerr << "sidecar magic mismatch\n";
        return 10;
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
        return 11;
    }

    if (container_version != alpha_recorder::alpha_container_format_version || container_header_size != alpha_recorder::alpha_container_header_size || record_count != scenario.expected_pair_count || index_offset <= alpha_recorder::alpha_container_header_size || index_entry_count != scenario.expected_pair_count || record_header_size != alpha_recorder::alpha_record_header_size || index_entry_size != alpha_recorder::alpha_index_entry_size || container_flags != 0U || container_reserved != 0U)
    {
        std::cerr << "sidecar header fields are incorrect\n";
        return 12;
    }

    sidecar_stream.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
    if (!sidecar_stream)
    {
        std::cerr << "failed to seek to the index\n";
        return 13;
    }

    std::vector<alpha_recorder::AlphaIndexEntry> entries(static_cast<std::size_t>(scenario.expected_pair_count));
    for (alpha_recorder::AlphaIndexEntry &entry : entries)
    {
        if (!read_le(sidecar_stream, entry.sequence) || !read_le(sidecar_stream, entry.pts) || !read_le(sidecar_stream, entry.record_offset) || !read_le(sidecar_stream, entry.record_header_size) || !read_le(sidecar_stream, entry.uncompressed_size) || !read_le(sidecar_stream, entry.compressed_size) || !read_le(sidecar_stream, entry.flags) || !read_le(sidecar_stream, entry.reserved))
        {
            std::cerr << "failed to read the index entries\n";
            return 14;
        }
    }

    std::uint64_t total_uncompressed_bytes = 0;
    std::uint64_t total_compressed_bytes = 0;

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const alpha_recorder::FramePair expected_pair = alpha_recorder::e2e::make_test_pair(static_cast<std::uint64_t>(index));
        const alpha_recorder::AlphaIndexEntry &entry = entries[index];

        if (entry.sequence != expected_pair.sequence || entry.pts != expected_pair.pts || entry.record_header_size != alpha_recorder::alpha_record_header_size || entry.uncompressed_size != expected_pair.alpha.bytes.size() || entry.flags != alpha_recorder::alpha_record_flag_lz4_block || entry.reserved != 0U)
        {
            std::cerr << "index entry fields are incorrect\n";
            return 15;
        }

        total_uncompressed_bytes += entry.uncompressed_size;
        total_compressed_bytes += entry.compressed_size;

        sidecar_stream.seekg(static_cast<std::streamoff>(entry.record_offset), std::ios::beg);
        if (!sidecar_stream)
        {
            std::cerr << "failed to seek to record " << index << '\n';
            return 16;
        }

        std::array<char, 8> record_magic{};
        sidecar_stream.read(record_magic.data(), static_cast<std::streamsize>(record_magic.size()));
        if (!sidecar_stream || record_magic != alpha_recorder::alpha_record_magic)
        {
            std::cerr << "record magic mismatch\n";
            return 17;
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
            return 18;
        }

        if (record_version != alpha_recorder::alpha_container_format_version || record_header_size_value != alpha_recorder::alpha_record_header_size || record_sequence != expected_pair.sequence || record_pts != expected_pair.pts || record_uncompressed_size != expected_pair.alpha.bytes.size() || record_compressed_size != entry.compressed_size || record_flags != alpha_recorder::alpha_record_flag_lz4_block || record_reserved != 0U)
        {
            std::cerr << "record header fields are incorrect\n";
            return 19;
        }

        const std::vector<std::uint8_t> compressed_payload = read_payload(sidecar_stream, record_compressed_size);
        if (compressed_payload.size() != record_compressed_size)
        {
            std::cerr << "failed to read the record payload\n";
            return 20;
        }

        const std::vector<std::uint8_t> decoded_payload = decode_lz4_literal_block(compressed_payload);
        if (decoded_payload != expected_pair.alpha.bytes)
        {
            std::cerr << "decoded alpha payload does not match the original bytes\n";
            return 21;
        }
    }

    const std::uint64_t first_sequence = entries.empty() ? 0U : entries.front().sequence;
    const std::uint64_t last_sequence = entries.empty() ? 0U : entries.back().sequence;
    const std::uint64_t first_pts = entries.empty() ? 0U : entries.front().pts;
    const std::uint64_t last_pts = entries.empty() ? 0U : entries.back().pts;

    const std::string manifest_text = read_text_file(manifest_path);
    if (manifest_text.empty())
    {
        std::cerr << "failed to read manifest text\n";
        return 22;
    }

    const std::filesystem::path sidecar_relative = output_root / scenario.alpha_sidecar;
    const std::filesystem::path manifest_relative = output_root / scenario.alpha_manifest;
    const std::uintmax_t sidecar_size = std::filesystem::file_size(sidecar_path);

    ManifestFields manifest_fields;
    std::string manifest_error;
    if (!parse_manifest_json(manifest_text, manifest_fields, manifest_error))
    {
        std::cerr << "invalid manifest JSON: " << manifest_error << '\n';
        return 23;
    }

    const std::string expected_sidecar_path = sidecar_relative.generic_string();
    const std::string expected_manifest_path = manifest_relative.generic_string();

    if (manifest_fields.schema != "alpha_recorder.session_summary.v1" || manifest_fields.container_format_version != alpha_recorder::alpha_container_format_version || manifest_fields.project_name != "alpha_recorder" || manifest_fields.project_version != "0.1.0" || manifest_fields.sidecar_path != expected_sidecar_path || manifest_fields.manifest_path != expected_manifest_path || manifest_fields.pair_count != scenario.expected_pair_count || manifest_fields.record_count != scenario.expected_pair_count || manifest_fields.first_sequence != first_sequence || manifest_fields.last_sequence != last_sequence || manifest_fields.first_pts != first_pts || manifest_fields.last_pts != last_pts || manifest_fields.alpha_uncompressed_bytes != total_uncompressed_bytes || manifest_fields.alpha_compressed_bytes != total_compressed_bytes || manifest_fields.index_offset != index_offset || manifest_fields.index_entry_count != scenario.expected_pair_count || manifest_fields.sidecar_size_bytes != sidecar_size)
    {
        std::cerr << "manifest content is missing expected session metadata\n";
        return 23;
    }

    if (sidecar_size != std::filesystem::file_size(sidecar_path))
    {
        std::cerr << "sidecar size check failed\n";
        return 24;
    }

    std::cout << "e2e test encoder scenario passed: " << scenario.name << '\n';
    std::cout << "  output root: " << output_root.string() << '\n';
    std::cout << "  verified pairs: " << scenario.expected_pair_count << ", dropped pairs: " << scenario.expected_drop_count << '\n';
    return 0;
}