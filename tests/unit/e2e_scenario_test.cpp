#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "alpha_recorder/e2e_scenario.hpp"

namespace
{

    void write_text_file(const std::filesystem::path &path, const std::string &text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }

} // namespace

int main()
{
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "alpha_recorder_e2e_scenario_test";
    std::error_code removeError;
    std::filesystem::remove_all(tempRoot, removeError);
    std::filesystem::create_directories(tempRoot);

    const std::filesystem::path validScenarioPath = tempRoot / "valid.scenario";
    write_text_file(validScenarioPath,
                    "name = gui_flow\n"
                    "expected_pair_count = 4\n"
                    "expected_drop_count = 2\n"
                    "output_root = session/output\n"
                    "rgb_artifact = artifacts/rgb.raw\n"
                    "alpha_sidecar = artifacts/alpha.sidecar\n"
                    "alpha_manifest = artifacts/alpha.manifest.json\n");

    alpha_recorder::e2e::E2EScenario scenario;
    std::string errorMessage;
    if (!alpha_recorder::e2e::load_scenario(validScenarioPath, scenario, errorMessage))
    {
        std::cerr << errorMessage << '\n';
        return 1;
    }

    if (scenario.name != "gui_flow" || scenario.expected_pair_count != 4U || scenario.expected_drop_count != 2U)
    {
        std::cerr << "scenario fields were not parsed correctly\n";
        return 2;
    }

    const std::filesystem::path artifactBase = tempRoot / "artifacts";
    const std::filesystem::path resolvedOutputRoot = alpha_recorder::e2e::resolve_output_root(artifactBase, scenario);
    if (resolvedOutputRoot != artifactBase / scenario.output_root)
    {
        std::cerr << "output root resolution did not preserve the scenario-relative path\n";
        return 3;
    }

    if (alpha_recorder::e2e::attempted_pair_count(scenario) != 6U)
    {
        std::cerr << "attempted pair count did not add the expected pair and drop counts\n";
        return 4;
    }

    const std::filesystem::path invalidScenarioPath = tempRoot / "invalid.scenario";
    const std::filesystem::path absoluteOutputRoot = std::filesystem::absolute(tempRoot / "absolute-output");
    write_text_file(invalidScenarioPath,
                    "name = invalid\n"
                    "expected_pair_count = 1\n"
                    "expected_drop_count = 0\n"
                    "output_root = " +
                        absoluteOutputRoot.generic_string() + "\n"
                                                              "rgb_artifact = rgb.raw\n"
                                                              "alpha_sidecar = alpha.sidecar\n"
                                                              "alpha_manifest = alpha.manifest.json\n");

    alpha_recorder::e2e::E2EScenario invalidScenario;
    errorMessage.clear();
    if (alpha_recorder::e2e::load_scenario(invalidScenarioPath, invalidScenario, errorMessage))
    {
        std::cerr << "absolute output_root should be rejected\n";
        return 5;
    }

    if (errorMessage.find("output_root must be relative") == std::string::npos)
    {
        std::cerr << "unexpected validation error message: " << errorMessage << '\n';
        return 6;
    }

    std::cout << "e2e scenario validation test passed\n";
    return 0;
}