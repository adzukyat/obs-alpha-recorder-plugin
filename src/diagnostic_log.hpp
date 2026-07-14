#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace alpha_recorder::obs
{

    [[nodiscard]] std::filesystem::path diagnostic_log_path() noexcept;
    [[nodiscard]] bool ensure_diagnostic_log_file(std::string *error_message = nullptr) noexcept;
    void append_diagnostic_log_line(std::string_view line) noexcept;

} // namespace alpha_recorder::obs
