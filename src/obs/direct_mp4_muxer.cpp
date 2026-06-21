#include "direct_mp4_muxer.hpp"

#include <filesystem>
#include <system_error>

#include <obs.h>

extern "C"
{
#include <mp4-mux.h>
#include <util/buffered-file-serializer.h>
}

#ifndef ALPHA_RECORDER_HAS_OBS_MP4_MUX_VIDEO_EDIT
#define ALPHA_RECORDER_HAS_OBS_MP4_MUX_VIDEO_EDIT 0
#endif

namespace alpha_recorder::obs
{
    namespace
    {
        void assign_error(std::string *error_message, const char *message)
        {
            if (error_message != nullptr)
            {
                *error_message = message;
            }
        }
    } // namespace

    struct DirectMp4Muxer::Impl
    {
        std::filesystem::path path{};
        serializer mp4_serializer{};
        mp4_mux *mp4_muxer = nullptr;
        bool serializer_open = false;
        bool accepting_packets = false;
        bool first_packet = true;
        bool has_pending_visible_range = false;
        AlphaVisiblePacketRange pending_visible_range{};
        DirectMp4MuxerStats stats{};

        void reset_state() noexcept
        {
            path.clear();
            accepting_packets = false;
            first_packet = true;
            has_pending_visible_range = false;
            pending_visible_range = {};
            stats = {};
        }

        void apply_pending_visible_range() noexcept
        {
#if ALPHA_RECORDER_HAS_OBS_MP4_MUX_VIDEO_EDIT
            if (mp4_muxer != nullptr && has_pending_visible_range)
            {
                mp4_mux_set_video_edit(mp4_muxer, pending_visible_range.media_time,
                                       pending_visible_range.duration);
            }
#endif
        }
    };

    DirectMp4Muxer::DirectMp4Muxer()
        : impl_(std::make_unique<Impl>())
    {
    }

    DirectMp4Muxer::~DirectMp4Muxer() noexcept
    {
        abort();
    }

    bool DirectMp4Muxer::open(const DirectMp4MuxerConfig &config,
                              std::string *error_message)
    {
        abort();
        impl_->reset_state();

        if (config.path.empty())
        {
            assign_error(error_message, "Direct MP4 output path is empty");
            return false;
        }

        const std::filesystem::path parent = config.path.parent_path();
        if (!parent.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                assign_error(error_message, "could not create the Direct MP4 output directory");
                return false;
            }
        }

        if (!buffered_file_serializer_init_defaults(&impl_->mp4_serializer,
                                                    config.path.string().c_str()))
        {
            assign_error(error_message, "could not open the Direct MP4 mux output");
            return false;
        }

        impl_->path = config.path;
        impl_->serializer_open = true;
        return true;
    }

    bool DirectMp4Muxer::begin(obs_output_t *output, std::string *error_message)
    {
        if (!impl_->serializer_open)
        {
            assign_error(error_message, "Direct MP4 output storage is not open");
            return false;
        }
        if (output == nullptr)
        {
            assign_error(error_message, "Direct MP4 output cannot mux without an OBS output");
            return false;
        }
        if (impl_->mp4_muxer != nullptr)
        {
            impl_->accepting_packets = true;
            return true;
        }

        impl_->mp4_muxer =
            mp4_mux_create(output, &impl_->mp4_serializer, MP4_VIDEO_ONLY_TRACKS, FLAVOR_MP4);
        if (impl_->mp4_muxer == nullptr)
        {
            assign_error(error_message, "could not create the Direct MP4 muxer");
            return false;
        }

        impl_->apply_pending_visible_range();
        impl_->accepting_packets = true;
        return true;
    }

    bool DirectMp4Muxer::set_visible_range(const AlphaVisiblePacketRange &range,
                                           std::string *error_message)
    {
        if (range.media_time < 0 || range.duration < 0)
        {
            assign_error(error_message, "Direct MP4 visible range cannot be negative");
            return false;
        }
        if (range.duration > 0 && !supports_visible_range())
        {
            assign_error(error_message, "this OBS mp4_mux build does not support Direct MP4 edit lists");
            return false;
        }

        impl_->pending_visible_range = range;
        impl_->has_pending_visible_range = range.duration > 0;
        impl_->apply_pending_visible_range();
        return true;
    }

    bool DirectMp4Muxer::submit_packet(encoder_packet *packet, std::string *error_message)
    {
        if (!impl_->accepting_packets || packet == nullptr || packet->type != OBS_ENCODER_VIDEO)
        {
            return true;
        }
        if (impl_->mp4_muxer == nullptr)
        {
            assign_error(error_message, "Direct MP4 muxer is not ready");
            return false;
        }

        if (!mp4_mux_submit_packet(impl_->mp4_muxer, packet))
        {
            assign_error(error_message, "could not mux the Direct MP4 packet");
            return false;
        }

        if (impl_->first_packet)
        {
            impl_->stats.first_pts = packet->pts;
            impl_->first_packet = false;
        }
        impl_->stats.last_pts = packet->pts;
        ++impl_->stats.packet_count;
        ++impl_->stats.muxed_packet_count;
        if (packet->keyframe)
        {
            ++impl_->stats.keyframe_count;
        }
        impl_->stats.packet_bytes += static_cast<std::uint64_t>(packet->size);
        return true;
    }

    bool DirectMp4Muxer::finalize(std::string *error_message)
    {
        impl_->accepting_packets = false;
        if (impl_->mp4_muxer == nullptr)
        {
            return true;
        }

        impl_->apply_pending_visible_range();
        impl_->stats.finalized = mp4_mux_finalise(impl_->mp4_muxer);
        if (!impl_->stats.finalized)
        {
            assign_error(error_message, "could not finalize the Direct MP4 mux output");
            return false;
        }
        return true;
    }

    void DirectMp4Muxer::close_storage() noexcept
    {
        impl_->accepting_packets = false;

        if (impl_->mp4_muxer != nullptr)
        {
            mp4_mux_destroy(impl_->mp4_muxer);
            impl_->mp4_muxer = nullptr;
        }
        if (impl_->serializer_open)
        {
            buffered_file_serializer_free(&impl_->mp4_serializer);
            impl_->serializer_open = false;
        }
    }

    void DirectMp4Muxer::abort() noexcept
    {
        close_storage();
    }

    bool DirectMp4Muxer::is_open() const noexcept
    {
        return impl_->serializer_open;
    }

    bool DirectMp4Muxer::is_accepting_packets() const noexcept
    {
        return impl_->accepting_packets;
    }

    bool DirectMp4Muxer::supports_visible_range() const noexcept
    {
        return ALPHA_RECORDER_HAS_OBS_MP4_MUX_VIDEO_EDIT != 0;
    }

    const std::filesystem::path &DirectMp4Muxer::path() const noexcept
    {
        return impl_->path;
    }

    const DirectMp4MuxerStats &DirectMp4Muxer::stats() const noexcept
    {
        return impl_->stats;
    }

} // namespace alpha_recorder::obs
