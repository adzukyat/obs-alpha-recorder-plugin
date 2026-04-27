#include <iostream>

#include "recording_session_controller_gate.hpp"

int main()
{
    if (!alpha_recorder::obs::recording_input_is_allowed(true, false, true, false))
    {
        std::cerr << "unpaused recording intake should be allowed\n";
        return 1;
    }

    if (alpha_recorder::obs::recording_input_is_allowed(true, false, true, true))
    {
        std::cerr << "paused recording intake should be blocked\n";
        return 2;
    }

    if (alpha_recorder::obs::recording_input_is_allowed(false, false, true, false))
    {
        std::cerr << "inactive sessions should not accept intake\n";
        return 3;
    }

    if (alpha_recorder::obs::recording_input_is_allowed(true, true, true, false))
    {
        std::cerr << "aborted sessions should not accept intake\n";
        return 4;
    }

    std::cout << "recording pause gate test passed\n";
    return 0;
}