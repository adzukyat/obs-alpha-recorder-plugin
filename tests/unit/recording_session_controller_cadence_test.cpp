#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "recording_session_controller_cadence.hpp"

namespace
{
    bool expect(bool condition, const char *message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            return false;
        }

        return true;
    }

    bool test_repeated_raw_video_frame_is_marked_duplicate()
    {
        alpha_recorder::obs::RawVideoCadenceTracker tracker;
        const std::uint8_t first_frame[] = {1U};
        const std::uint8_t second_frame[] = {2U};

        const auto first = tracker.remember(first_frame, 100U);
        const auto repeated = tracker.remember(first_frame, 133U);
        const auto next = tracker.remember(second_frame, 166U);

        return expect(!first.duplicate_previous, "first raw output frame must not be marked duplicate") &&
               expect(first.timestamp == 100U, "first raw output timestamp changed") &&
               expect(repeated.duplicate_previous, "repeated raw output buffer must be marked duplicate") &&
               expect(repeated.timestamp == 133U, "duplicate raw output timestamp changed") &&
               expect(!next.duplicate_previous, "new raw output buffer must not be marked duplicate");
    }

    bool test_duplicate_output_frame_reuses_last_written_alpha()
    {
        const auto previous_alpha = std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{7U, 8U, 9U});
        const auto newer_pending_alpha =
            std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{100U, 101U, 102U});

        const alpha_recorder::obs::AlphaFrame last_written{100U, previous_alpha};
        const alpha_recorder::obs::AlphaFrame newer_pending{133U, newer_pending_alpha};
        const alpha_recorder::obs::OutputFrameCadence repeated_output{133U, true};
        alpha_recorder::obs::AlphaFrame resolved{};

        if (!expect(alpha_recorder::obs::duplicate_output_uses_previous_alpha(repeated_output, last_written, resolved),
                    "duplicate output frame should resolve from the last written alpha"))
        {
            return false;
        }

        return expect(resolved.timestamp == last_written.timestamp,
                      "duplicate output frame drifted to a newer alpha timestamp") &&
               expect(resolved.alpha == last_written.alpha, "duplicate output frame did not reuse previous alpha bytes") &&
               expect(resolved.alpha != newer_pending.alpha,
                      "duplicate output frame must not consume the newer pending alpha frame");
    }

    bool test_non_duplicate_output_does_not_force_previous_alpha()
    {
        const auto previous_alpha = std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{7U});
        const alpha_recorder::obs::AlphaFrame last_written{100U, previous_alpha};
        const alpha_recorder::obs::OutputFrameCadence fresh_output{133U, false};
        alpha_recorder::obs::AlphaFrame resolved{};

        return expect(!alpha_recorder::obs::duplicate_output_uses_previous_alpha(fresh_output, last_written, resolved),
                      "fresh output frame should not be forced to the previous alpha frame") &&
               expect(resolved.empty(), "fresh output frame unexpectedly resolved alpha");
    }
} // namespace

int main()
{
    if (!test_repeated_raw_video_frame_is_marked_duplicate() ||
        !test_duplicate_output_frame_reuses_last_written_alpha() ||
        !test_non_duplicate_output_does_not_force_previous_alpha())
    {
        return 1;
    }

    std::cout << "recording session cadence test passed\n";
    return 0;
}
