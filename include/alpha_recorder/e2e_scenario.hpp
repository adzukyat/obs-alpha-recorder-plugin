#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "alpha_recorder/frame_pair.hpp"

namespace alpha_recorder::e2e
{

    struct E2EScenario
    {
        std::string name{};
        std::uint64_t expected_pair_count = 0;
        std::uint64_t expected_drop_count = 0;
        std::uint64_t expected_split_at_sequence = 0;
        std::filesystem::path output_root{};
        std::filesystem::path rgb_artifact{};
        std::filesystem::path alpha_sidecar{};
        std::filesystem::path alpha_manifest{};
    };

    [[nodiscard]] bool load_scenario(const std::filesystem::path &scenario_path, E2EScenario &scenario, std::string &error_message) noexcept;
    [[nodiscard]] std::filesystem::path resolve_output_root(const std::filesystem::path &artifact_root, const E2EScenario &scenario);
    [[nodiscard]] std::filesystem::path resolve_artifact_path(const std::filesystem::path &output_root, const std::filesystem::path &artifact_name);
    [[nodiscard]] std::uint64_t attempted_pair_count(const E2EScenario &scenario) noexcept;
    [[nodiscard]] FramePair make_test_pair(std::uint64_t index) noexcept;

} // namespace alpha_recorder::e2e