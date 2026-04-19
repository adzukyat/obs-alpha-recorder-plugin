#include "alpha_recorder/pair_gate.hpp"

namespace alpha_recorder
{

    PairAdmissionGate::PairAdmissionGate(PairAdmissionCapacity remaining) noexcept
        : remaining_(remaining)
    {
    }

    void PairAdmissionGate::reset(PairAdmissionCapacity remaining) noexcept
    {
        remaining_ = remaining;
    }

    void PairAdmissionGate::set_remaining(PairAdmissionCapacity remaining) noexcept
    {
        remaining_ = remaining;
    }

    PairAdmissionCapacity PairAdmissionGate::remaining() const noexcept
    {
        return remaining_;
    }

    bool PairAdmissionGate::can_accept(const FramePair &pair) const noexcept
    {
        return pair.is_complete() && remaining_.pair_slots > 0 && remaining_.rgb_slots > 0 && remaining_.alpha_slots > 0;
    }

    bool PairAdmissionGate::try_accept(const FramePair &pair) noexcept
    {
        if (!can_accept(pair))
        {
            return false;
        }

        --remaining_.pair_slots;
        --remaining_.rgb_slots;
        --remaining_.alpha_slots;
        return true;
    }

} // namespace alpha_recorder