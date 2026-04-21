#pragma once

#include <filesystem>
#include <string>

#include "alpha_recorder/plugin.hpp"

namespace alpha_recorder::obs
{

    struct FinalizationExportRequest
    {
        std::filesystem::path recording_path{};
        std::filesystem::path sidecar_path{};
        std::filesystem::path manifest_path{};
        FinalizationFormat finalization_format = FinalizationFormat::ProRes4444;
    };

    bool export_completed_recording(const FinalizationExportRequest &request, std::string *error_message = nullptr) noexcept;

} // namespace alpha_recorder::obs
