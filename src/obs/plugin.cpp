#include <obs-module.h>

#include "alpha_recorder/plugin.hpp"
#include "alpha_recorder/version.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("alpha_recorder", "en-US")

namespace alpha_recorder::obs
{

    std::string_view module_name() noexcept
    {
        return project_name();
    }

    std::string_view module_description() noexcept
    {
#if defined(ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
        return "OBS test-only scenario output module for alpha_recorder";
#else
        return "OBS recording lifecycle module for alpha_recorder";
#endif
    }

    bool initialize_module() noexcept
    {
#if defined(ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
        return register_output_module();
#else
        return register_runtime_hooks();
#endif
    }

} // namespace alpha_recorder::obs

extern "C" bool obs_module_load(void)
{
    return alpha_recorder::obs::initialize_module();
}

MODULE_EXPORT bool alpha_recorder_sync_runtime_hooks(void)
{
#if defined(ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
    return true;
#else
    return alpha_recorder::obs::register_runtime_hooks();
#endif
}

extern "C" void obs_module_unload(void)
{
#if !defined(ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
    alpha_recorder::obs::unregister_runtime_hooks();
#endif
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return alpha_recorder::obs::module_description().data();
}