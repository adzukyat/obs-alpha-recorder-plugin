#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace alpha_recorder::obs
{

    [[nodiscard]] inline std::string e2e_test_environment_value(
        const char *name) noexcept
    {
#ifdef _WIN32
        char *value = nullptr;
        std::size_t size = 0U;
        if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        {
            return {};
        }
        std::string result{value};
        std::free(value);
        return result;
#else
        const char *value = std::getenv(name);
        return value == nullptr ? std::string{} : std::string{value};
#endif
    }

    [[nodiscard]] inline bool e2e_test_mode_enabled() noexcept
    {
        return e2e_test_environment_value("ALPHA_RECORDER_E2E_TEST") == "1";
    }

    [[nodiscard]] inline bool e2e_test_fault_enabled(std::string_view fault) noexcept
    {
        if (!e2e_test_mode_enabled())
        {
            return false;
        }
        return e2e_test_environment_value("ALPHA_RECORDER_E2E_FAULT") == fault;
    }

    [[nodiscard]] inline std::uint64_t e2e_test_environment_u64(
        const char *name,
        std::uint64_t fallback) noexcept
    {
        const std::string value = e2e_test_environment_value(name);
        if (value.empty())
        {
            return fallback;
        }
        char *end = nullptr;
        const unsigned long long parsed =
            std::strtoull(value.c_str(), &end, 10);
        return end != value.c_str() && *end == '\0'
                   ? static_cast<std::uint64_t>(parsed)
                   : fallback;
    }

    [[nodiscard]] inline bool e2e_test_fault_applies_to_segment(
        std::uint64_t segment_index) noexcept
    {
        if (!e2e_test_mode_enabled())
        {
            return false;
        }
        const std::string value =
            e2e_test_environment_value("ALPHA_RECORDER_E2E_FAULT_SEGMENT");
        if (value.empty())
        {
            return true;
        }
        char *end = nullptr;
        const unsigned long long requested = std::strtoull(value.c_str(), &end, 10);
        return end != value.c_str() && *end == '\0' &&
               requested == static_cast<unsigned long long>(segment_index);
    }

} // namespace alpha_recorder::obs
