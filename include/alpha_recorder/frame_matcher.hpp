#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace alpha_recorder
{

    struct AlphaFrameMatch
    {
        std::uint64_t frame_cts = 0;
        std::uint64_t packet_pts = 0;
    };

    class AlphaFrameMatcher
    {
    public:
        AlphaFrameMatcher(std::size_t max_frames, std::size_t max_packets, std::size_t max_bytes) noexcept
            : max_frames_(max_frames),
              max_packets_(max_packets),
              max_bytes_(max_bytes)
        {
        }

        [[nodiscard]] bool add_frame(std::uint64_t frame_cts, std::size_t byte_size, AlphaFrameMatch &match) noexcept
        {
            const auto packet_it = find_packet_for_frame(frame_cts);
            if (packet_it != packets_.end())
            {
                match = {frame_cts, packet_it->pts};
                packets_.erase(packet_it);
                return true;
            }

            frames_.push_back({frame_cts, byte_size});
            pending_bytes_ += byte_size;
            return false;
        }

        [[nodiscard]] bool add_packet(std::optional<std::uint64_t> packet_cts, std::uint64_t packet_pts,
                                      AlphaFrameMatch &match) noexcept
        {
            const auto frame_it = find_frame_for_packet(packet_cts);
            if (frame_it != frames_.end())
            {
                match = {frame_it->cts, packet_pts};
                pending_bytes_ -= frame_it->byte_size;
                frames_.erase(frame_it);
                return true;
            }

            packets_.push_back({packet_cts, packet_pts});
            return false;
        }

        [[nodiscard]] bool overflowed() const noexcept
        {
            return frames_.size() > max_frames_ || packets_.size() > max_packets_ || pending_bytes_ > max_bytes_;
        }

        void clear() noexcept
        {
            frames_.clear();
            packets_.clear();
            pending_bytes_ = 0;
        }

        [[nodiscard]] std::size_t pending_frame_count() const noexcept
        {
            return frames_.size();
        }

        [[nodiscard]] std::size_t pending_packet_count() const noexcept
        {
            return packets_.size();
        }

        [[nodiscard]] std::size_t pending_bytes() const noexcept
        {
            return pending_bytes_;
        }

    private:
        struct PendingFrame
        {
            std::uint64_t cts = 0;
            std::size_t byte_size = 0;
        };

        struct PendingPacket
        {
            std::optional<std::uint64_t> cts{};
            std::uint64_t pts = 0;
        };

        [[nodiscard]] std::deque<PendingPacket>::iterator find_packet_for_frame(std::uint64_t frame_cts) noexcept
        {
            for (auto it = packets_.begin(); it != packets_.end(); ++it)
            {
                if (!it->cts.has_value() || *it->cts == frame_cts)
                {
                    return it;
                }
            }

            return packets_.end();
        }

        [[nodiscard]] std::deque<PendingFrame>::iterator find_frame_for_packet(std::optional<std::uint64_t> packet_cts) noexcept
        {
            if (!packet_cts.has_value())
            {
                return frames_.begin();
            }

            for (auto it = frames_.begin(); it != frames_.end(); ++it)
            {
                if (it->cts == *packet_cts)
                {
                    return it;
                }
            }

            return frames_.end();
        }

        std::size_t max_frames_ = 0;
        std::size_t max_packets_ = 0;
        std::size_t max_bytes_ = 0;
        std::size_t pending_bytes_ = 0;
        std::deque<PendingFrame> frames_{};
        std::deque<PendingPacket> packets_{};
    };

} // namespace alpha_recorder
