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

enum class ShoutcastError {
    not_enabled,
    ffmpeg_not_found,
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
class ShoutcastSource final : public IAudioSource {
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
    uint64_t track_generation() const noexcept override {
        return info_gen_.load(std::memory_order_acquire);
    }

    std::string_view name() const noexcept override { return "shoutcast"; }
    std::string display_name() const override FRDX_EXCLUDES(mu_) {
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

    // Set the stream URL and restart playback.
    // Station preset management.
    void load_stations(const std::filesystem::path& path) FRDX_EXCLUDES(mu_);
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
    void cycle_preset(int direction = 1) FRDX_REQUIRES(mu_);
    void cycle_and_restart(int direction) FRDX_EXCLUDES(mu_);
    void stop_metadata_poller() FRDX_EXCLUDES(ws_mu_);          // join bg thread (no-op now, kept for API compat)
    void stop_icy_poller();               // join ICY metadata thread
    void run_icy_poller(const std::string& stream_url,
                        const std::stop_token& stop); // ICY metadata loop body
    void run_ws_poller(std::string url, StationMetadataMapping mapping,
                       StationMetadataWebSocketFilter filter,
                       StationMetadataWebSocketHeartbeat hb,
                       std::stop_token stop); // WebSocket metadata loop body

    // Launches ffmpeg subprocess for the given URL.  Does NOT lock mu_.
    static std::expected<std::unique_ptr<Pipe>, ShoutcastError>
    launch_ffmpeg_pipe(const std::string& url,
                       const std::filesystem::path& ffmpeg_path);

    ShoutcastConfig cfg_;
    std::unique_ptr<Pipe> pipe_ FRDX_GUARDED_BY(mu_);

    mutable frdx::Mutex mu_;
    std::string target_url_          FRDX_GUARDED_BY(mu_);
    std::size_t preset_idx_          FRDX_GUARDED_BY(mu_) = 0;
    TrackInfo info_                  FRDX_GUARDED_BY(mu_){};
    std::atomic<uint64_t> position_ms_{0};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};

    // Dynamic display name (includes station name when active).
    std::string display_name_ FRDX_GUARDED_BY(mu_){"Online Radio"};

    // Station presets.
    std::vector<StationPreset> stations_ FRDX_GUARDED_BY(mu_);
    // Cached flat (station_idx, stream_idx) pairs for O(1) cycle_preset(),
    // rebuilt in load_stations() when stations_ changes.
    std::vector<std::pair<std::size_t, std::size_t>> flat_presets_ FRDX_GUARDED_BY(mu_);
    std::string active_station_id_      FRDX_GUARDED_BY(mu_);
    std::string active_stream_id_       FRDX_GUARDED_BY(mu_);
    std::string metadata_url_           FRDX_GUARDED_BY(mu_); // resolved REST metadata URL for current selection

    // Restart metadata poller for the currently active station (releases mu_ first).
    // FRDX_NO_THREAD_SAFETY_ANALYSIS: the Clang analyzer doesn't understand
    // std::jthread boundaries and traces lock state from the internal mu_
    // acquisition through the spawned lambdas, producing false positives.
    FRDX_NO_THREAD_SAFETY_ANALYSIS
    void restart_metadata_for_current() FRDX_EXCLUDES(mu_);

    // Metadata poller thread (background REST/WS polling).
    std::jthread metadata_thread_;

    // ICY metadata poller — connects directly to the stream URL via WinHTTP,
    // sends Icy-MetaData: 1, parses interleaved StreamTitle blocks.
    std::jthread icy_poller_thread_;

    // WinHTTP WebSocket handle for forced close on stop.
    // void* avoids pulling <windows.h>/<winhttp.h> into this public header.
    mutable frdx::Mutex ws_mu_;
    void* ws_handle_ FRDX_GUARDED_BY(ws_mu_) = nullptr;

    // Persistent WinHTTP session handle shared across REST and WebSocket polling.
    void* winhttp_session_ = nullptr;

    // Retry counter to preserve active station during brief network dropouts.
    int reconnect_attempts_ FRDX_GUARDED_BY(mu_) = 0;

    // Timestamp (steady_clock ms) of the last ICY StreamTitle received in
    // pump(). The REST/WS pollers check this — if ICY is actively delivering
    // metadata, they skip overwriting title/artist.
    std::atomic<int64_t> last_icy_ms_{0};

    // Generation counter bumped on every info_ write so the overlay can
    // avoid calling current_track() (which locks mu_) every frame.
    std::atomic<uint64_t> info_gen_{0};
};

} // namespace frdx::sources
