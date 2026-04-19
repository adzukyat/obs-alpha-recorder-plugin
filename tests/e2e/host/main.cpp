#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <obs.h>

#include "alpha_recorder/e2e_scenario.hpp"

namespace
{

#ifdef _WIN32
    LRESULT CALLBACK hidden_window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    HWND create_hidden_window()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSW window_class = {};
        window_class.lpszClassName = L"alpha_recorder_e2e";
        window_class.hInstance = instance;
        window_class.lpfnWndProc = hidden_window_proc;

        if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return nullptr;
        }

        return CreateWindowExW(0, window_class.lpszClassName, L"alpha_recorder_e2e", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, instance, nullptr);
    }
#endif

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

    std::filesystem::path resolve_artifact_root(int argc, char **argv)
    {
        const std::filesystem::path cli_root = read_argument(argc, argv, "--artifact-root");
        if (!cli_root.empty())
        {
            return cli_root;
        }

        const char *artifact_root_env = std::getenv("ALPHA_RECORDER_E2E_ARTIFACT_ROOT");
        const std::filesystem::path env_root = (artifact_root_env != nullptr && *artifact_root_env != '\0')
                                                   ? std::filesystem::path{artifact_root_env}
                                                   : std::filesystem::path{};
        if (!env_root.empty())
        {
            return env_root;
        }

        const char *stage_root_env = std::getenv("ALPHA_RECORDER_STAGE_DIR");
        const std::filesystem::path stage_root = (stage_root_env != nullptr && *stage_root_env != '\0')
                                                     ? std::filesystem::path{stage_root_env}
                                                     : std::filesystem::path{};
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

    std::filesystem::path resolve_stage_dir(int argc, char **argv)
    {
        const std::filesystem::path cli_stage_dir = read_argument(argc, argv, "--stage-dir");
        if (!cli_stage_dir.empty())
        {
            return cli_stage_dir;
        }

        const char *stage_dir_env = std::getenv("ALPHA_RECORDER_STAGE_DIR");
        if (stage_dir_env != nullptr && *stage_dir_env != '\0')
        {
            return std::filesystem::path{stage_dir_env};
        }

        return {};
    }

    bool initialize_obs(const std::filesystem::path &stage_dir, std::string &error_message)
    {
        const std::filesystem::path bin_dir = stage_dir / "bin" / "64bit";
        const std::filesystem::path plugin_dir = stage_dir / "obs-plugins" / "64bit";
        const std::filesystem::path data_dir = stage_dir / "data";

        if (!std::filesystem::exists(bin_dir))
        {
            error_message.assign("stage dir is missing the OBS runtime bin directory: ");
            error_message.append(bin_dir.generic_string());
            return false;
        }

        if (!std::filesystem::exists(plugin_dir))
        {
            error_message.assign("stage dir is missing the OBS plugin directory: ");
            error_message.append(plugin_dir.generic_string());
            return false;
        }

        if (!std::filesystem::exists(data_dir))
        {
            error_message.assign("stage dir is missing the OBS data directory: ");
            error_message.append(data_dir.generic_string());
            return false;
        }

        const std::filesystem::path module_config_path = stage_dir / "module-config";
        std::error_code directory_error;
        std::filesystem::create_directories(module_config_path, directory_error);
        if (directory_error)
        {
            error_message.assign("failed to create the OBS module config directory: ");
            error_message.append(module_config_path.generic_string());
            return false;
        }

        const std::string module_config_text = module_config_path.generic_string();
        if (!obs_startup("en-US", module_config_text.c_str(), nullptr))
        {
            error_message.assign("failed to initialize OBS");
            return false;
        }

#ifdef _WIN32
        const std::wstring bin_dir_text = bin_dir.wstring();
        if (SetCurrentDirectoryW(bin_dir_text.c_str()) == 0)
        {
            error_message.assign("failed to set the working directory to the staged OBS bin directory");
            return false;
        }

        if (SetDllDirectoryW(bin_dir_text.c_str()) == 0)
        {
            error_message.assign("failed to add the OBS runtime bin directory to the DLL search path");
            obs_shutdown();
            return false;
        }
#endif

        obs_video_info video_info = {};
        video_info.adapter = 0;
        video_info.base_width = 1280;
        video_info.base_height = 720;
        video_info.output_width = 1280;
        video_info.output_height = 720;
        video_info.fps_num = 30000;
        video_info.fps_den = 1001;
        video_info.graphics_module = "libobs-opengl.dll";
        video_info.output_format = VIDEO_FORMAT_RGBA;

        if (obs_reset_video(&video_info) != OBS_VIDEO_SUCCESS)
        {
            error_message.assign("failed to initialize the OBS video subsystem");
            obs_shutdown();
            return false;
        }

        obs_audio_info audio_info = {};
        audio_info.samples_per_sec = 48000;
        audio_info.speakers = SPEAKERS_STEREO;

        if (!obs_reset_audio(&audio_info))
        {
            error_message.assign("failed to initialize the OBS audio subsystem");
            obs_shutdown();
            return false;
        }

        const std::filesystem::path plugin_path = plugin_dir / "alpha_recorder.dll";
        if (!std::filesystem::exists(plugin_path))
        {
            error_message.assign("stage dir is missing the alpha_recorder plugin: ");
            error_message.append(plugin_path.generic_string());
            obs_shutdown();
            return false;
        }

        obs_module_t *module = nullptr;
        const std::string plugin_path_text = plugin_path.generic_string();
        const std::string data_path_text = (data_dir / "obs-plugins" / "%module%").generic_string();

        const int open_code = obs_open_module(&module, plugin_path_text.c_str(), data_path_text.c_str());
        if (open_code != MODULE_SUCCESS)
        {
            error_message.assign("failed to open the alpha_recorder module: ");
            error_message.append(plugin_path.generic_string());
            obs_shutdown();
            return false;
        }

        if (!obs_init_module(module))
        {
            error_message.assign("failed to initialize the alpha_recorder module");
            obs_shutdown();
            return false;
        }

        obs_post_load_modules();
        return true;
    }

    bool start_output(const std::filesystem::path &scenario_path,
                      const std::filesystem::path &artifact_root,
                      std::string &error_message)
    {
        obs_data_t *settings = obs_data_create();
        obs_data_set_string(settings, "scenario_path", scenario_path.generic_string().c_str());
        obs_data_set_string(settings, "artifact_root", artifact_root.generic_string().c_str());

        obs_output_t *output = obs_output_create("alpha_recorder_output", "alpha_recorder_e2e", settings, nullptr);
        obs_data_release(settings);

        if (output == nullptr)
        {
            error_message.assign("failed to create the alpha_recorder output");
            return false;
        }

        const bool started = obs_output_start(output);
        if (!started)
        {
            const char *last_error = obs_output_get_last_error(output);
            if (last_error != nullptr && *last_error != '\0')
            {
                error_message.assign(last_error);
            }
            else
            {
                error_message.assign("alpha_recorder output failed to start");
            }

            obs_output_release(output);
            return false;
        }

        obs_output_stop(output);
        obs_output_release(output);
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

#ifdef _WIN32
    const HWND hidden_window = create_hidden_window();
    if (hidden_window == nullptr)
    {
        std::cerr << "failed to create hidden OBS test window\n";
        return 2;
    }
#endif

    alpha_recorder::e2e::E2EScenario scenario;
    std::string scenario_error;
    if (!alpha_recorder::e2e::load_scenario(scenario_path, scenario, scenario_error))
    {
        std::cerr << scenario_error << '\n';
        return 3;
    }

    const std::filesystem::path stage_dir = resolve_stage_dir(argc, argv);
    if (stage_dir.empty())
    {
        std::cerr << "missing required --stage-dir argument or ALPHA_RECORDER_STAGE_DIR environment variable\n";
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

    std::string obs_error;
    if (!initialize_obs(stage_dir, obs_error))
    {
        std::cerr << obs_error << '\n';
        return 5;
    }

    const std::filesystem::path output_root = alpha_recorder::e2e::resolve_output_root(artifact_root, scenario);
    std::error_code remove_error;
    std::filesystem::remove_all(output_root, remove_error);
    if (remove_error)
    {
        std::cerr << "failed to clear output root: " << output_root.string() << '\n';
        obs_shutdown();
        return 6;
    }

    if (!start_output(scenario_path, output_root, obs_error))
    {
        std::cerr << obs_error << '\n';
        obs_shutdown();
#ifdef _WIN32
        DestroyWindow(hidden_window);
#endif
        return 7;
    }

    const std::filesystem::path rgb_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.rgb_artifact);
    const std::filesystem::path sidecar_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_sidecar);
    const std::filesystem::path manifest_path = alpha_recorder::e2e::resolve_artifact_path(output_root, scenario.alpha_manifest);

    if (!std::filesystem::exists(rgb_path) || !std::filesystem::exists(sidecar_path) || !std::filesystem::exists(manifest_path))
    {
        std::cerr << "host did not find the expected artifacts under " << output_root.string() << '\n';
        obs_shutdown();
#ifdef _WIN32
        DestroyWindow(hidden_window);
#endif
        return 8;
    }

    obs_shutdown();

    std::cout << "e2e host scenario passed: " << scenario.name << "\n";
    std::cout << "  output root: " << output_root.string() << '\n';
    return 0;
}