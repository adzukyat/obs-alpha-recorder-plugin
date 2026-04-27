#pragma once

namespace alpha_recorder::obs
{

    [[nodiscard]] inline constexpr bool recording_input_is_allowed(bool session_active,
                                                                   bool session_aborted,
                                                                   bool writer_open,
                                                                   bool recording_paused) noexcept
    {
        return session_active && !session_aborted && writer_open && !recording_paused;
    }

} // namespace alpha_recorder::obs