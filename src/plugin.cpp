#include <obs-module.h>
#include <obs-frontend-api.h>

#include "alpha_recorder/plugin.hpp"
#include "gpu_texture_recording_output.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("alpha_recorder", "en-US")

namespace alpha_recorder::obs
{

    std::string_view module_description() noexcept
    {
        return "OBS recording lifecycle module for alpha_recorder";
    }

    bool initialize_module() noexcept
    {
        return register_gpu_texture_recording_output() && register_runtime_hooks();
    }

} // namespace alpha_recorder::obs

namespace
{
    void alpha_recorder_recording_frontend_event(enum obs_frontend_event event, void *) noexcept
    {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
        {
            alpha_recorder::obs::register_websocket_vendor_api();
        }
    }
} // namespace

extern "C" bool obs_module_load(void)
{
    return alpha_recorder::obs::initialize_module();
}

extern "C" void obs_module_post_load(void)
{
    alpha_recorder::obs::register_websocket_vendor_api();
    obs_frontend_add_event_callback(alpha_recorder_recording_frontend_event, nullptr);
    alpha_recorder::obs::register_settings_ui();
}

extern "C" void obs_module_unload(void)
{
    alpha_recorder::obs::unregister_settings_ui();
    obs_frontend_remove_event_callback(alpha_recorder_recording_frontend_event, nullptr);
    alpha_recorder::obs::unregister_websocket_vendor_api();
    alpha_recorder::obs::unregister_runtime_hooks();
    alpha_recorder::obs::unregister_gpu_texture_recording_output();
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return alpha_recorder::obs::module_description().data();
}
