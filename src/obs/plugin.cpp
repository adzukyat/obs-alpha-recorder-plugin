#include <obs-module.h>

#include "alpha_recorder/plugin.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("alpha_recorder", "en-US")

extern "C" bool obs_module_load(void)
{
    return alpha_recorder::obs::initialize_module();
}

extern "C" const char *obs_module_description(void)
{
    return alpha_recorder::obs::module_description().data();
}