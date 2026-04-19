#pragma once

#include "alpha_recorder/sidecar_writer.hpp"

namespace alpha_recorder
{

    class ManifestWriter
    {
    public:
        ManifestWriter() noexcept = default;

        bool write(const AlphaSessionSummary &summary) noexcept;
    };

} // namespace alpha_recorder