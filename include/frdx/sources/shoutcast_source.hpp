#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "frdx/detail/thread_annotations.hpp"

namespace frdx::sources {

class MetadataController;

enum class ShoutcastError {
    pipe_creation_failed,
    spawn_failed,
};

// ---- Station preset data structures ----

struct StationMetadataMapping {
    std::string title;  // JSON dot-path, e.g. "data.track_title"
    std::string artist; // JSON dot-path, e.g. "data.track_artist"
    std::string
        artist_array_field; // if artist resolves to array of objects, extract this field from each
    std::string artist_separator{", "}; // join array elements with this separator
    std::string
        split_separator; // when set, split a combined "artist - title" value from a single field
};

struct StationStream {
    std::string id;
    std::string name;
    std::string url;
    // Per-stream mapping override — when non-empty, takes precedence over
    // the station-level mapping for metadata resolution. This allows fixing
    // individual streams whose API returns data in a different format (e.g.
    // a combined "artist - title" metadata field instead of separate fields).
    StationMetadataMapping mapping_override;
};

// WebSocket message filter — only process messages matching this criteria.
struct StationMetadataWebSocketFilter {
    std::string op_field;   // JSON path to the op code field, e.g. "op"
    int op_value = 0;       // expected op value, e.g. 0 or 1
    std::string type_field; // JSON path to the type field, e.g. "t"
    std::string type_value; // expected type string, e.g. "TRACK_UPDATE"
    std::string data_field; // JSON path to the data object, e.g. "d"
};

// WebSocket heartbeat / keep-alive configuration.
struct StationMetadataWebSocketHeartbeat {
    std::string message; // JSON message to send, e.g. "{\"op\":9}"
    std::string
        interval_field; // dot-path in server response that gives interval, e.g. "d.heartbeat"
    int default_interval_ms = 45000; // fallback if server doesn't provide interval
};

struct StationMetadata {
    std::string type; // "rest", "static", "websocket", or empty
    std::string url;
    std::unordered_map<std::string, std::string> stream_url_map; // stream_id -> metadata URL
    StationMetadataMapping mapping;

    // Static metadata (type == "static").
    std::string static_title;
    std::string static_artist;

    // WebSocket metadata (type == "websocket").
    StationMetadataWebSocketFilter ws_filter;
    StationMetadataWebSocketHeartbeat ws_heartbeat;
};

struct StationPreset {
    std::string id;
    std::string name;
    std::vector<StationStream> streams;
    StationMetadata metadata;
};

// ---- ShoutcastSource ----
//
// Streams audio from a Shoutcast / Icecast URL via ffmpeg.
// Supports station presets (JSON catalog) with REST metadata polling
// for live track info, plus ICY metadata from the stream stderr.
class ShoutcastSource final : public IAudioSource, public IAudioMetadataProvider {
public:
    explicit ShoutcastSource(ShoutcastConfig cfg);
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    ~ShoutcastSource() override;

    // Non-copyable and non-movable — holds a mutex (whose address must
    // remain stable for GUARDED_BY annotations), atomics accessed from
    // multiple threads, and WinHTTP handles that must not be duplicated.
    ShoutcastSource(const ShoutcastSource&)            = delete;
    ShoutcastSource& operator=(const ShoutcastSource&) = delete;
    ShoutcastSource(ShoutcastSource&&)                 = delete;
    ShoutcastSource& operator=(ShoutcastSource&&)      = delete;

    SourceType type() const noexcept override { return SourceType::shoutcast; }

    // IAudioMetadataProvider
    IAudioMetadataProvider* metadata_provider() noexcept override { return this; }
    uint64_t track_generation() const noexcept override {
        return info_gen_.load(std::memory_order_acquire);
    }

    std::string_view name() const noexcept override { return "shoutcast"; }
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    std::string display_name() const override FRDX_EXCLUDES(mu_) {
        // Snapshot display_name_ under mu_ so all threads see the same
        // value for a given generation.  Uses an atomic load of the gen
        // counter to avoid locking on repeated calls within the same
        // generation — only the control-loop metadata path (push_metadata)
        // calls this at low frequency (4 Hz), so the lock is cheap.
        //
        // No thread_local cache — the previous thread_local approach could
        // go stale when the station changed on a background thread without
        // the calling thread ever re-locking.
        const uint64_t gen = display_name_gen_.load(std::memory_order_acquire);
        {
            frdx::ScopedLock lk{mu_};
            // Re-check gen under lock in case it changed between the
            // acquire-load above and the lock acquisition.
            if (display_name_gen_.load(std::memory_order_relaxed) == gen) {
                return display_name_;
            }
        }
        // Gen changed after the load — retry once with lock held.
        frdx::ScopedLock lk{mu_};
        return display_name_;
    }

    bool initialize() override FRDX_EXCLUDES(mu_);
    void shutdown() noexcept override FRDX_EXCLUDES(mu_);

    void play() override FRDX_EXCLUDES(mu_);
    void pause() override FRDX_EXCLUDES(mu_);
    void next() override FRDX_EXCLUDES(mu_);
    void previous() override FRDX_EXCLUDES(mu_);
    void pump(RingBuffer& ring) override FRDX_EXCLUDES(mu_);

    void set_volume_normalization(bool on) noexcept { volume_norm_.store(on, std::memory_order_release); }

    // Update the ffmpeg binary path. Takes effect on the next stream
    // (re)connect. Used by DependencyManager after auto-downloading ffmpeg.
    void update_ffmpeg_path(const std::filesystem::path& path) noexcept {
        std::lock_guard lk{mu_};
        cfg_.ffmpeg_path = path;
    }

    // Set the stream URL and restart playback.
    // Station preset management.
    void load_stations(const std::filesystem::path& path) FRDX_EXCLUDES(mu_);
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    std::vector<StationPreset> stations() const FRDX_EXCLUDES(mu_) {
        frdx::ScopedLock lk{mu_};
        return stations_;
    }
    // Lock-free const-ref access for the render thread, where no concurrent
    // station mutations occur (all mutations happen on the action/bridge thread).
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    const std::vector<StationPreset>& stations_nolock() const noexcept { return stations_; }
    std::pair<std::string, std::string> active_station_stream() const FRDX_EXCLUDES(mu_) {
        frdx::ScopedLock lk{mu_};
        return {active_station_id_, active_stream_id_};
    }
    void select_station(const std::string& station_id, const std::string& stream_id) FRDX_EXCLUDES(mu_);

    TrackInfo current_track() const override FRDX_EXCLUDES(mu_);
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

private:
    struct Pipe;

    void start_pipe_locked() FRDX_REQUIRES(mu_);
    void stop_pipe_locked() FRDX_REQUIRES(mu_);
    void restore_station_display_locked() FRDX_REQUIRES(mu_);
    void cycle_preset(int direction = 1) FRDX_REQUIRES(mu_);
    void cycle_and_restart(int direction) FRDX_EXCLUDES(mu_);
    bool queue_timed_metadata_locked(TrackInfo info, int64_t now_ms, uint64_t metadata_epoch)
        FRDX_REQUIRES(mu_);
    void apply_pending_metadata_locked(const char* reason) FRDX_REQUIRES(mu_);
    void clear_pending_metadata_locked() FRDX_REQUIRES(mu_);
    uint64_t reset_metadata_sync_locked() FRDX_REQUIRES(mu_);
    void observe_pcm_for_metadata_sync(const int16_t* samples, std::size_t sample_count,
                                       uint64_t metadata_epoch)
        FRDX_EXCLUDES(mu_);

    // Launches ffmpeg subprocess for the given URL.  Does NOT lock mu_.
    static std::expected<std::unique_ptr<Pipe>, ShoutcastError>
    launch_ffmpeg_pipe(const std::string& url,
                       const std::filesystem::path& ffmpeg_path,
                       bool volume_normalization);

    ShoutcastConfig cfg_;
    std::atomic<bool> volume_norm_{false};
    // Lock-free audio path: pump() loads via acquire, start_pipe_locked() stores
    // via release.  Shared ownership ensures the Pipe stays alive until pump()
    // releases its local reference, even if stop_pipe_locked() replaces pipe_.
    std::atomic<std::shared_ptr<Pipe>> pipe_{};

    mutable frdx::Mutex mu_;
    std::string target_url_          FRDX_GUARDED_BY(mu_);
    std::size_t preset_idx_          FRDX_GUARDED_BY(mu_) = 0;
    TrackInfo info_                  FRDX_GUARDED_BY(mu_){};
    TrackInfo pending_info_          FRDX_GUARDED_BY(mu_){};
    int64_t pending_info_ms_         FRDX_GUARDED_BY(mu_) = 0;
    int64_t quiet_started_ms_        FRDX_GUARDED_BY(mu_) = 0;
    uint64_t pending_info_epoch_     FRDX_GUARDED_BY(mu_) = 0;
    bool pending_info_valid_         FRDX_GUARDED_BY(mu_) = false;
    bool metadata_epoch_has_track_   FRDX_GUARDED_BY(mu_) = false;
    std::atomic<uint64_t> position_ms_{0};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};

    // Dynamic display name (includes station name when active).
    std::string display_name_ FRDX_GUARDED_BY(mu_){"Online Radio"};
    // Atomic generation counter bumped whenever display_name_ changes under mu_.
    // display_name() uses this + a thread_local cache to avoid locking mu_ on every call.
    mutable std::atomic<uint64_t> display_name_gen_{1};

    // Station presets.
    std::vector<StationPreset> stations_ FRDX_GUARDED_BY(mu_);
    // Cached flat (station_idx, stream_idx) pairs for O(1) cycle_preset(),
    // rebuilt in load_stations() when stations_ changes.
    std::vector<std::pair<std::size_t, std::size_t>> flat_presets_ FRDX_GUARDED_BY(mu_);
    std::string active_station_id_      FRDX_GUARDED_BY(mu_);
    std::string active_stream_id_       FRDX_GUARDED_BY(mu_);
    std::string metadata_url_           FRDX_GUARDED_BY(mu_); // resolved REST metadata URL for current selection

    // Restarts polling for the current station. Delegates to metadata_controller_.
    void restart_metadata_for_current() FRDX_EXCLUDES(mu_);

    // Persistent WinHTTP session handle shared across polling connections.
    void* winhttp_session_ = nullptr;

    // Retry counter to preserve active station during brief network dropouts.
    std::atomic<int> reconnect_attempts_{0};

    // Background metadata poller (ICY + WebSocket). Allocated on heap so
    // shoutcast_source.hpp only needs a forward declaration (avoids circular
    // include with shoutcast_metadata.hpp).
    std::unique_ptr<MetadataController> metadata_controller_;

    // Generation counter bumped on every info_ write so the overlay can
    // avoid calling current_track() (which locks mu_) every frame.
    std::atomic<uint64_t> info_gen_{0};
    std::atomic<uint64_t> metadata_epoch_{1};
};

} // namespace frdx::sources
