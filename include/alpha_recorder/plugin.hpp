#pragma once

#include <string_view>

namespace alpha_recorder::obs
{

    [[nodiscard]] std::string_view module_name() noexcept;
    [[nodiscard]] std::string_view module_description() noexcept;
    bool register_output_bridge() noexcept;
    bool initialize_module() noexcept;

} // namespace alpha_recorder::obs