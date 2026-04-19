#pragma once

#include <cstdint>

#include "alpha_recorder/frame_pair.hpp"

namespace alpha_recorder
{

    struct PairAdmissionCapacity
    {
        std::uint64_t pair_slots = 0;
        std::uint64_t rgb_slots = 0;
        std::uint64_t alpha_slots = 0;
    };

    class PairAdmissionGate
    {
    public:
        PairAdmissionGate() noexcept = default;
        explicit PairAdmissionGate(PairAdmissionCapacity remaining) noexcept;

        void reset(PairAdmissionCapacity remaining = {}) noexcept;
        void set_remaining(PairAdmissionCapacity remaining) noexcept;

        [[nodiscard]] PairAdmissionCapacity remaining() const noexcept;
        [[nodiscard]] bool can_accept(const FramePair &pair) const noexcept;
        [[nodiscard]] bool try_accept(const FramePair &pair) noexcept;

    private:
        PairAdmissionCapacity remaining_{};
    };

    using PairGate = PairAdmissionGate;

} // namespace alpha_recorder