#include "gpu_texture_alpha_output_sink.hpp"

#include "matroska_hevc_muxer.hpp"

namespace alpha_recorder::obs
{
    class GpuTextureAlphaOutputSink::Muxer
    {
    public:
        virtual ~Muxer() noexcept = default;

        [[nodiscard]] virtual bool open(const GpuTextureAlphaOutputSinkConfig &config,
                                        std::string *error_message) = 0;
        [[nodiscard]] virtual bool begin_mux(obs_output_t *output, std::string *error_message) = 0;
        [[nodiscard]] virtual bool set_visible_range(const AlphaVisiblePacketRange &range,
                                                     std::string *error_message) = 0;
        [[nodiscard]] virtual bool submit_packet(encoder_packet *packet, std::string *error_message) = 0;
        [[nodiscard]] virtual bool finalize(std::string *error_message) = 0;

        virtual void close_storage() noexcept = 0;
        virtual void abort() noexcept = 0;

        [[nodiscard]] virtual bool is_open() const noexcept = 0;
        [[nodiscard]] virtual bool is_accepting_packets() const noexcept = 0;
        [[nodiscard]] virtual const std::filesystem::path &path() const noexcept = 0;
        [[nodiscard]] virtual const AlphaMovieMuxerStats &stats() const noexcept = 0;
    };

    namespace
    {
        class IsoBmffMuxer final : public GpuTextureAlphaOutputSink::Muxer
        {
        public:
            [[nodiscard]] bool open(const GpuTextureAlphaOutputSinkConfig &config,
                                    std::string *error_message) override
            {
                return muxer_.open(DirectMp4MuxerConfig{config.path, config.container == AlphaMovieContainer::Mov},
                                   error_message);
            }

            [[nodiscard]] bool begin_mux(obs_output_t *output, std::string *error_message) override
            {
                return muxer_.begin(output, error_message);
            }

            [[nodiscard]] bool set_visible_range(const AlphaVisiblePacketRange &range,
                                                 std::string *error_message) override
            {
                return muxer_.set_visible_range(range, error_message);
            }

            [[nodiscard]] bool submit_packet(encoder_packet *packet, std::string *error_message) override
            {
                return muxer_.submit_packet(packet, error_message);
            }

            [[nodiscard]] bool finalize(std::string *error_message) override
            {
                return muxer_.finalize(error_message);
            }

            void close_storage() noexcept override
            {
                muxer_.close_storage();
            }

            void abort() noexcept override
            {
                muxer_.abort();
            }

            [[nodiscard]] bool is_open() const noexcept override
            {
                return muxer_.is_open();
            }

            [[nodiscard]] bool is_accepting_packets() const noexcept override
            {
                return muxer_.is_accepting_packets();
            }

            [[nodiscard]] const std::filesystem::path &path() const noexcept override
            {
                return muxer_.path();
            }

            [[nodiscard]] const AlphaMovieMuxerStats &stats() const noexcept override
            {
                return muxer_.stats();
            }

        private:
            DirectMp4Muxer muxer_{};
        };

        class MkvMuxer final : public GpuTextureAlphaOutputSink::Muxer
        {
        public:
            [[nodiscard]] bool open(const GpuTextureAlphaOutputSinkConfig &config,
                                    std::string *error_message) override
            {
                return muxer_.open(
                    MatroskaHevcMuxerConfig{config.path, config.tail_packet_buffer_size},
                    error_message);
            }

            [[nodiscard]] bool begin_mux(obs_output_t *output, std::string *error_message) override
            {
                return muxer_.begin(output, error_message);
            }

            [[nodiscard]] bool set_visible_range(const AlphaVisiblePacketRange &range,
                                                 std::string *error_message) override
            {
                return muxer_.set_visible_range(range, error_message);
            }

            [[nodiscard]] bool submit_packet(encoder_packet *packet, std::string *error_message) override
            {
                return muxer_.submit_packet(packet, error_message);
            }

            [[nodiscard]] bool finalize(std::string *error_message) override
            {
                return muxer_.finalize(error_message);
            }

            void close_storage() noexcept override
            {
                muxer_.close_storage();
            }

            void abort() noexcept override
            {
                muxer_.abort();
            }

            [[nodiscard]] bool is_open() const noexcept override
            {
                return muxer_.is_open();
            }

            [[nodiscard]] bool is_accepting_packets() const noexcept override
            {
                return muxer_.is_accepting_packets();
            }

            [[nodiscard]] const std::filesystem::path &path() const noexcept override
            {
                return muxer_.path();
            }

            [[nodiscard]] const AlphaMovieMuxerStats &stats() const noexcept override
            {
                return muxer_.stats();
            }

        private:
            MatroskaHevcMuxer muxer_{};
        };
    } // namespace

    GpuTextureAlphaOutputSink::GpuTextureAlphaOutputSink() = default;

    GpuTextureAlphaOutputSink::~GpuTextureAlphaOutputSink() noexcept = default;

    bool GpuTextureAlphaOutputSink::open(const GpuTextureAlphaOutputSinkConfig &config,
                                         std::string *error_message)
    {
        if (muxer_ != nullptr)
        {
            muxer_->abort();
            muxer_.reset();
        }

        if (config.container == AlphaMovieContainer::Mkv)
        {
            muxer_ = std::make_unique<MkvMuxer>();
        }
        else
        {
            muxer_ = std::make_unique<IsoBmffMuxer>();
        }
        return muxer_->open(config, error_message);
    }

    bool GpuTextureAlphaOutputSink::begin_mux(obs_output_t *output, std::string *error_message)
    {
        if (muxer_ == nullptr)
        {
            if (error_message != nullptr)
            {
                *error_message = "Alpha Recorder alpha movie muxer is not open.";
            }
            return false;
        }
        return muxer_->begin_mux(output, error_message);
    }

    bool GpuTextureAlphaOutputSink::set_visible_range(const AlphaVisiblePacketRange &range,
                                                      std::string *error_message)
    {
        if (muxer_ == nullptr)
        {
            if (error_message != nullptr)
            {
                *error_message = "Alpha Recorder alpha movie muxer is not open.";
            }
            return false;
        }
        return muxer_->set_visible_range(range, error_message);
    }

    bool GpuTextureAlphaOutputSink::submit_packet(encoder_packet *packet, std::string *error_message)
    {
        if (muxer_ == nullptr)
        {
            if (error_message != nullptr)
            {
                *error_message = "Alpha Recorder alpha movie muxer is not open.";
            }
            return false;
        }
        return muxer_->submit_packet(packet, error_message);
    }

    bool GpuTextureAlphaOutputSink::finalize(std::string *error_message)
    {
        if (muxer_ == nullptr)
        {
            if (error_message != nullptr)
            {
                *error_message = "Alpha Recorder alpha movie muxer is not open.";
            }
            return false;
        }
        return muxer_->finalize(error_message);
    }

    void GpuTextureAlphaOutputSink::close_storage() noexcept
    {
        if (muxer_ != nullptr)
        {
            muxer_->close_storage();
        }
    }

    void GpuTextureAlphaOutputSink::abort() noexcept
    {
        if (muxer_ != nullptr)
        {
            muxer_->abort();
            muxer_.reset();
        }
    }

    bool GpuTextureAlphaOutputSink::is_open() const noexcept
    {
        return muxer_ != nullptr && muxer_->is_open();
    }

    bool GpuTextureAlphaOutputSink::is_accepting_packets() const noexcept
    {
        return muxer_ != nullptr && muxer_->is_accepting_packets();
    }

    const std::filesystem::path &GpuTextureAlphaOutputSink::path() const noexcept
    {
        static const std::filesystem::path empty_path{};
        return muxer_ == nullptr ? empty_path : muxer_->path();
    }

    const GpuTextureAlphaOutputSinkStats &GpuTextureAlphaOutputSink::stats() const noexcept
    {
        return muxer_ == nullptr ? empty_stats_ : muxer_->stats();
    }

} // namespace alpha_recorder::obs
