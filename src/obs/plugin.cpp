#include <obs-module.h>
#if !defined(ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
#include <obs-frontend-api.h>
#endif

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
        return register_output_module() && register_e2e_sources();
#else
        return register_runtime_hooks();
#endif
    }

} // namespace alpha_recorder::obs

namespace
{
#if !defined(ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
    void alpha_recorder_frontend_event(enum obs_frontend_event event, void *) noexcept
    {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
        {
            alpha_recorder::obs::register_websocket_vendor_api();
        }
    }
#endif
} // namespace

extern "C" bool obs_module_load(void)
{
    return alpha_recorder::obs::initialize_module();
}

extern "C" void obs_module_post_load(void)
{
#if !defined(ALPHA_RECORDER_ENABLE_E2E_OUTPUT_MODULE)
    alpha_recorder::obs::register_websocket_vendor_api();
    obs_frontend_add_event_callback(alpha_recorder_frontend_event, nullptr);
#endif
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
    obs_frontend_remove_event_callback(alpha_recorder_frontend_event, nullptr);
    alpha_recorder::obs::unregister_websocket_vendor_api();
    alpha_recorder::obs::unregister_runtime_hooks();
#endif
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return alpha_recorder::obs::module_description().data();
}
