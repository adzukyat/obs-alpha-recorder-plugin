#include "gpu_texture_alpha_output_sink.hpp"

namespace alpha_recorder::obs
{
    GpuTextureAlphaOutputSink::~GpuTextureAlphaOutputSink() noexcept = default;

    bool GpuTextureAlphaOutputSink::open(const GpuTextureAlphaOutputSinkConfig &config,
                                         std::string *error_message)
    {
        return muxer_.open(DirectMp4MuxerConfig{config.path}, error_message);
    }

    bool GpuTextureAlphaOutputSink::begin_mux(obs_output_t *output, std::string *error_message)
    {
        return muxer_.begin(output, error_message);
    }

    bool GpuTextureAlphaOutputSink::set_visible_range(const AlphaVisiblePacketRange &range,
                                                      std::string *error_message)
    {
        return muxer_.set_visible_range(range, error_message);
    }

    bool GpuTextureAlphaOutputSink::submit_packet(encoder_packet *packet, std::string *error_message)
    {
        return muxer_.submit_packet(packet, error_message);
    }

    bool GpuTextureAlphaOutputSink::finalize(std::string *error_message)
    {
        return muxer_.finalize(error_message);
    }

    void GpuTextureAlphaOutputSink::close_storage() noexcept
    {
        muxer_.close_storage();
    }

    void GpuTextureAlphaOutputSink::abort() noexcept
    {
        muxer_.abort();
    }

    bool GpuTextureAlphaOutputSink::is_open() const noexcept
    {
        return muxer_.is_open();
    }

    bool GpuTextureAlphaOutputSink::is_accepting_packets() const noexcept
    {
        return muxer_.is_accepting_packets();
    }

    const std::filesystem::path &GpuTextureAlphaOutputSink::path() const noexcept
    {
        return muxer_.path();
    }

    const GpuTextureAlphaOutputSinkStats &GpuTextureAlphaOutputSink::stats() const noexcept
    {
        return muxer_.stats();
    }

} // namespace alpha_recorder::obs
