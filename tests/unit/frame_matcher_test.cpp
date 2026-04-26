#include "alpha_recorder/frame_matcher.hpp"

#include <cstdint>
#include <iostream>

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

    bool test_exact_frame_then_packet()
    {
        alpha_recorder::AlphaFrameMatcher matcher{4, 4, 1024};
        alpha_recorder::AlphaFrameMatch match{};

        if (!expect(!matcher.add_frame(100, 16, match), "frame should wait for a matching packet"))
        {
            return false;
        }

        if (!expect(matcher.add_packet(100, 9000, match), "packet should match the pending frame by CTS"))
        {
            return false;
        }

        return expect(match.frame_cts == 100 && match.packet_pts == 9000, "match should preserve frame CTS and packet PTS") &&
               expect(matcher.pending_frame_count() == 0 && matcher.pending_packet_count() == 0, "matched queues should be empty");
    }

    bool test_exact_packet_then_frame()
    {
        alpha_recorder::AlphaFrameMatcher matcher{4, 4, 1024};
        alpha_recorder::AlphaFrameMatch match{};

        if (!expect(!matcher.add_packet(200, 12000, match), "packet should wait for a later raw frame"))
        {
            return false;
        }

        if (!expect(matcher.add_frame(200, 16, match), "frame should match a pending packet by CTS"))
        {
            return false;
        }

        return expect(match.frame_cts == 200 && match.packet_pts == 12000, "packet-before-frame match returned wrong values");
    }

    bool test_dropped_raw_frame_is_not_matched()
    {
        alpha_recorder::AlphaFrameMatcher matcher{4, 4, 1024};
        alpha_recorder::AlphaFrameMatch match{};

        if (!expect(!matcher.add_frame(300, 16, match), "raw frame should remain pending"))
        {
            return false;
        }

        if (!expect(!matcher.add_packet(301, 13000, match), "packet with different CTS should not match the raw frame"))
        {
            return false;
        }

        return expect(matcher.pending_frame_count() == 1 && matcher.pending_packet_count() == 1, "unmatched frame and packet should remain pending");
    }

    bool test_missing_packet_timing_uses_fifo()
    {
        alpha_recorder::AlphaFrameMatcher matcher{4, 4, 1024};
        alpha_recorder::AlphaFrameMatch match{};

        if (!expect(!matcher.add_frame(400, 16, match), "first raw frame should remain pending"))
        {
            return false;
        }

        if (!expect(!matcher.add_frame(401, 16, match), "second raw frame should remain pending"))
        {
            return false;
        }

        if (!expect(matcher.add_packet({}, 14000, match), "packet without CTS should match the oldest pending frame"))
        {
            return false;
        }

        return expect(match.frame_cts == 400 && match.packet_pts == 14000, "FIFO fallback should use the oldest frame");
    }

    bool test_overflow_detection()
    {
        alpha_recorder::AlphaFrameMatcher matcher{1, 1, 16};
        alpha_recorder::AlphaFrameMatch match{};

        if (!expect(!matcher.add_frame(500, 16, match), "first frame should fit the queue"))
        {
            return false;
        }

        if (!expect(!matcher.overflowed(), "queue should not overflow at the configured limit"))
        {
            return false;
        }

        if (!expect(!matcher.add_frame(501, 1, match), "second unmatched frame should be queued before overflow handling"))
        {
            return false;
        }

        return expect(matcher.overflowed(), "queue should report overflow after exceeding frame and byte limits");
    }

} // namespace

int main()
{
    if (!test_exact_frame_then_packet() ||
        !test_exact_packet_then_frame() ||
        !test_dropped_raw_frame_is_not_matched() ||
        !test_missing_packet_timing_uses_fifo() ||
        !test_overflow_detection())
    {
        return 1;
    }

    std::cout << "frame matcher test passed\n";
    return 0;
}
