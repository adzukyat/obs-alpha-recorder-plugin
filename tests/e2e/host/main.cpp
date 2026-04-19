#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <windows.h>

#include "alpha_recorder/e2e_module.hpp"
#include "alpha_recorder/e2e_scenario.hpp"

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

    std::filesystem::path resolve_module_path(int argc, char **argv)
    {
        const std::filesystem::path cli_module = read_argument(argc, argv, "--module");
        if (!cli_module.empty())
        {
            return cli_module;
        }

        const std::filesystem::path env_module = read_environment_path("ALPHA_RECORDER_E2E_MODULE");
        if (!env_module.empty())
        {
            return env_module;
        }

        return {};
    }

    struct ModuleHandle
    {
        HMODULE handle = nullptr;

        ~ModuleHandle()
        {
            if (handle != nullptr)
            {
                FreeLibrary(handle);
            }
        }
    };

    bool load_module(const std::filesystem::path &module_path, ModuleHandle &module, std::string &error_message)
    {
        std::wstring module_wide = module_path.wstring();
        module.handle = LoadLibraryW(module_wide.c_str());
        if (module.handle == nullptr)
        {
            error_message.assign("failed to load E2E module: ");
            error_message.append(module_path.generic_string());
            return false;
        }

        return true;
    }

    bool run_module(const std::filesystem::path &module_path,
                    const std::filesystem::path &scenario_path,
                    const std::filesystem::path &artifact_root,
                    alpha_recorder::e2e::E2ERunResult &result,
                    std::string &error_message)
    {
        ModuleHandle module;
        if (!load_module(module_path, module, error_message))
        {
            return false;
        }

        const std::string scenario_path_text = scenario_path.string();
        const std::string artifact_root_text = artifact_root.string();
        std::vector<char> module_error(512U, '\0');

        const alpha_recorder::e2e::E2ERunFunction run_function = reinterpret_cast<alpha_recorder::e2e::E2ERunFunction>(GetProcAddress(module.handle, alpha_recorder::e2e::e2e_run_symbol_name));
        if (run_function == nullptr)
        {
            error_message.assign("failed to resolve E2E module export: ");
            error_message.append(alpha_recorder::e2e::e2e_run_symbol_name);
            return false;
        }

        if (!run_function(scenario_path_text.c_str(), artifact_root_text.c_str(), &result, module_error.data(), module_error.size()))
        {
            error_message.assign(module_error.data());
            if (error_message.empty())
            {
                error_message.assign("E2E module export returned failure without an error message");
            }
            return false;
        }

        return true;
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

    const std::filesystem::path module_path = resolve_module_path(argc, argv);
    if (module_path.empty())
    {
        std::cerr << "missing required --module argument or ALPHA_RECORDER_E2E_MODULE environment variable\n";
        return 4;
    }

    const std::filesystem::path artifact_root = resolve_artifact_root(argc, argv);
    std::error_code directory_error;
    std::filesystem::create_directories(artifact_root, directory_error);
    if (directory_error)
    {
        std::cerr << "failed to create artifact root: " << artifact_root.string() << '\n';
        return 5;
    }

    const std::filesystem::path output_root = alpha_recorder::e2e::resolve_output_root(artifact_root, scenario);
    std::error_code remove_error;
    std::filesystem::remove_all(output_root, remove_error);
    if (remove_error)
    {
        std::cerr << "failed to clear output root: " << output_root.string() << '\n';
        return 6;
    }

    alpha_recorder::e2e::E2ERunResult result;
    std::string module_error;
    if (!run_module(module_path, scenario_path, output_root, result, module_error))
    {
        std::cerr << module_error << '\n';
        return 7;
    }

    const std::uint64_t attempted = alpha_recorder::e2e::attempted_pair_count(scenario);
    if (result.attempted_pairs != attempted || result.accepted_pairs != scenario.expected_pair_count || result.dropped_pairs != scenario.expected_drop_count)
    {
        std::cerr << "module result counts do not match the scenario expectations\n";
        return 8;
    }

    const std::filesystem::path rgb_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.rgb_artifact);
    const std::filesystem::path sidecar_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_sidecar);
    const std::filesystem::path manifest_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_manifest);

    if (!std::filesystem::exists(rgb_path) || !std::filesystem::exists(sidecar_path) || !std::filesystem::exists(manifest_path))
    {
        std::cerr << "host did not find the expected artifacts under " << output_root.string() << '\n';
        return 9;
    }

    std::cout << "e2e host scenario passed: " << scenario.name << "\n";
    std::cout << "  output root: " << output_root.string() << '\n';
    std::cout << "  accepted pairs: " << result.accepted_pairs << ", dropped pairs: " << result.dropped_pairs << '\n';
    return 0;
}