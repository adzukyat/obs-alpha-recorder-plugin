#pragma once

#include <string_view>

namespace alpha_recorder
{

    inline constexpr std::string_view project_name() noexcept
    {
        return "alpha_recorder";
    }

    inline constexpr std::string_view project_version() noexcept
    {
        return "0.1.0";
    }

} // namespace alpha_recorder