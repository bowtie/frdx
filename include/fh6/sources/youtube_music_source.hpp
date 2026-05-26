#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace fh6::sources {

// Streams audio via `yt-dlp | ffmpeg -f s16le -ar 44100 -ac 2`. The pcm pipe
// is drained into the ring buffer by pump().
class YouTubeMusicSource final : public IAudioSource {
public:
    explicit YouTubeMusicSource(YouTubeMusicConfig cfg);
    ~YouTubeMusicSource() override;

    std::string_view name() const noexcept override { return "youtube_music"; }
    std::string_view display_name() const noexcept override { return "YouTube Music"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void pump(RingBuffer& ring) override;

    // URL / playlist / search query to play next.
    void set_target(std::string url);

    AudioFormat format() const noexcept override { return {}; }
    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return auth_; }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, false, true}; }

private:
    struct Pipe;
    void start_pipe();
    void stop_pipe();

    YouTubeMusicConfig cfg_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    std::string target_url_;
    TrackInfo info_{};
    AuthState auth_ = AuthState::none_required;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
};

} // namespace fh6::sources
