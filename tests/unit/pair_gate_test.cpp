#include <cstdint>
#include <iostream>

#include "alpha_recorder/pair_gate.hpp"
#include "alpha_recorder/version.hpp"

namespace
{

    alpha_recorder::FramePair make_complete_pair(std::uint64_t sequence, std::uint64_t pts)
    {
        alpha_recorder::FramePair pair;
        pair.sequence = sequence;
        pair.pts = pts;
        pair.rgb.width = 2;
        pair.rgb.height = 1;
        pair.rgb.stride = 6;
        pair.rgb.bytes = {0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U};
        pair.alpha.width = 2;
        pair.alpha.height = 1;
        pair.alpha.stride = 2;
        pair.alpha.bytes = {0xA0U, 0xA1U};
        return pair;
    }

} // namespace

int main()
{
    alpha_recorder::PairAdmissionGate gate;
    const auto initial_capacity = gate.remaining();
    if (initial_capacity.pair_slots != 0 || initial_capacity.rgb_slots != 0 || initial_capacity.alpha_slots != 0)
    {
        std::cerr << "fresh gate should start empty\n";
        return 1;
    }

    const alpha_recorder::FramePair complete_pair = make_complete_pair(7U, 101U);
    if (gate.can_accept(complete_pair))
    {
        std::cerr << "empty gate should not accept a pair\n";
        return 2;
    }

    gate.reset({1U, 1U, 1U});
    if (!gate.can_accept(complete_pair))
    {
        std::cerr << "gate should accept when every capacity is available\n";
        return 3;
    }

    if (!gate.try_accept(complete_pair))
    {
        std::cerr << "gate rejected a complete pair even though capacity was available\n";
        return 4;
    }

    const auto after_accept = gate.remaining();
    if (after_accept.pair_slots != 0 || after_accept.rgb_slots != 0 || after_accept.alpha_slots != 0)
    {
        std::cerr << "gate did not consume every capacity for the accepted pair\n";
        return 5;
    }

    if (gate.try_accept(complete_pair))
    {
        std::cerr << "gate accepted a pair after its capacity was exhausted\n";
        return 6;
    }

    gate.reset({1U, 1U, 0U});
    const auto before_reject = gate.remaining();
    if (gate.can_accept(complete_pair))
    {
        std::cerr << "gate should reject when alpha capacity is missing\n";
        return 7;
    }

    if (gate.try_accept(complete_pair))
    {
        std::cerr << "gate accepted a pair with missing alpha capacity\n";
        return 8;
    }

    const auto after_reject = gate.remaining();
    if (after_reject.pair_slots != before_reject.pair_slots || after_reject.rgb_slots != before_reject.rgb_slots || after_reject.alpha_slots != before_reject.alpha_slots)
    {
        std::cerr << "gate partially consumed capacity on rejection\n";
        return 9;
    }

    alpha_recorder::FramePair incomplete_pair = complete_pair;
    incomplete_pair.alpha.bytes.clear();

    gate.reset({1U, 1U, 1U});
    const auto before_incomplete = gate.remaining();
    if (gate.can_accept(incomplete_pair))
    {
        std::cerr << "gate should reject an incomplete pair\n";
        return 10;
    }

    if (gate.try_accept(incomplete_pair))
    {
        std::cerr << "gate accepted an incomplete pair\n";
        return 11;
    }

    const auto after_incomplete = gate.remaining();
    if (after_incomplete.pair_slots != before_incomplete.pair_slots || after_incomplete.rgb_slots != before_incomplete.rgb_slots || after_incomplete.alpha_slots != before_incomplete.alpha_slots)
    {
        std::cerr << "gate mutated capacity while rejecting an incomplete pair\n";
        return 12;
    }

    if (alpha_recorder::project_name().empty() || alpha_recorder::project_version().empty())
    {
        std::cerr << "project metadata is missing\n";
        return 13;
    }

    std::cout << "pair admission gate test passed\n";
    return 0;
}