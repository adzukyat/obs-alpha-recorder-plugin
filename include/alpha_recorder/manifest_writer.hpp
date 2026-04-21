#pragma once

#include "alpha_recorder/sidecar_writer.hpp"

namespace alpha_recorder
{

    [[nodiscard]] inline constexpr std::string_view manifest_schema_name() noexcept
    {
        return "alpha_recorder.session_summary.v1";
    }

    class ManifestWriter
    {
    public:
        ManifestWriter() noexcept = default;

        bool write(const AlphaSessionSummary &summary) noexcept;
        bool read(const std::filesystem::path &manifest_path, AlphaSessionSummary &summary,
                  std::string *error_message = nullptr) noexcept;
    };

} // namespace alpha_recorder