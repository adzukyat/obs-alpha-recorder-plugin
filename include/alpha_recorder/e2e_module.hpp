#pragma once

#include <cstddef>
#include <cstdint>

namespace alpha_recorder::e2e
{

    struct E2ERunResult
    {
        std::uint64_t attempted_pairs = 0;
        std::uint64_t accepted_pairs = 0;
        std::uint64_t dropped_pairs = 0;
    };

    using E2ERunFunction = bool (*)(const char *scenario_path, const char *output_root, E2ERunResult *result, char *error_message, std::size_t error_message_size) noexcept;

    inline constexpr const char *e2e_run_symbol_name = "alpha_recorder_run_e2e";

} // namespace alpha_recorder::e2e