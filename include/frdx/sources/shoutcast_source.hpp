#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace frdx::sources {

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

// For APIs that return all streams in one response (e.g. an array of station
// objects). The poller navigates to array_path, finds the entry whose
// match_field equals the stream's resolved name, then applies the mapping.
struct StationMetadataEntrySelector {
    std::string array_path;       // dot-path to the array, e.g. "station"
    std::string match_field;      // field to match inside each entry, e.g. "name"
    std::string match_value_from; // "stream_name" (resolved via stream_name_map)
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
    std::unordered_map<std::string, std::string>
        stream_name_map; // stream_id -> display name for entry_selector matching
    int poll_interval_secs = 20;
    StationMetadataMapping mapping;
    StationMetadataEntrySelector entry_selector;

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
    ~ShoutcastSource() override;

    SourceType type() const noexcept override { return SourceType::shoutcast; }
    uint64_t track_generation() const noexcept override {
        return info_gen_.load(std::memory_order_acquire);
    }

    std::string_view name() const noexcept override { return "shoutcast"; }
    std::string display_name() const override {
        std::scoped_lock lk{mu_};
        return display_name_;
    }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    // Set the stream URL and restart playback.
    void set_target(std::string url);

    // Cycle through preset URLs (simple string presets from config.toml).
    void set_presets(std::vector<std::string> presets);

    // Station preset management.
    void load_stations(const std::filesystem::path& path);
    std::vector<StationPreset> stations() const {
        std::scoped_lock lk{mu_};
        return stations_;
    }
    std::pair<std::string, std::string> active_station_stream() const {
        std::scoped_lock lk{mu_};
        return {active_station_id_, active_stream_id_};
    }
    void select_station(const std::string& station_id, const std::string& stream_id);

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

private:
    struct Pipe;

    void start_pipe_locked();             // mu_ held
    void stop_pipe_locked();              // mu_ held
    void cycle_preset(int direction = 1); // cycle through all streams by one step, mu_ held
    void stop_metadata_poller();          // join bg thread (no-op now, kept for API compat)
    void stop_icy_poller();               // join ICY metadata thread
    void run_icy_poller(const std::string& stream_url,
                        const std::stop_token& stop); // ICY metadata loop body

    // Launches ffmpeg subprocess for the given URL.  Does NOT lock mu_.
    // Returns nullptr on any failure (CreateJob, CreatePipe, CreateProcess).
    static std::unique_ptr<Pipe> launch_ffmpeg_pipe(const std::string& url,
                                                    const std::filesystem::path& ffmpeg_path);

    ShoutcastConfig cfg_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    std::string target_url_;
    std::size_t preset_idx_ = 0;
    TrackInfo info_{};
    std::atomic<uint64_t> position_ms_{0};
    std::atomic<PlaybackState> state_{PlaybackState::stopped};

    // Dynamic display name (includes station name when active).
    std::string display_name_{"Online Radio"};

    // Station presets.
    std::vector<StationPreset> stations_;
    std::string active_station_id_;
    std::string active_stream_id_;
    std::string metadata_url_; // resolved REST metadata URL for current selection

    // Restart metadata poller for the currently active station (releases mu_ first).
    void restart_metadata_for_current();

    // Metadata poller thread (background REST/WS polling).
    std::jthread metadata_thread_;

    // ICY metadata poller — connects directly to the stream URL via WinHTTP,
    // sends Icy-MetaData: 1, parses interleaved StreamTitle blocks.
    std::jthread icy_poller_thread_;

    // WinHTTP WebSocket handle for forced close on stop.
    // void* avoids pulling <windows.h>/<winhttp.h> into this public header.
    mutable std::mutex ws_mu_;
    void* ws_handle_ = nullptr;

    // Persistent WinHTTP session handle shared across REST and WebSocket polling.
    void* winhttp_session_ = nullptr;

    // Retry counter to preserve active station during brief network dropouts.
    int reconnect_attempts_ = 0;

    // Timestamp (steady_clock ms) of the last ICY StreamTitle received in
    // pump(). The REST/WS pollers check this — if ICY is actively delivering
    // metadata, they skip overwriting title/artist.
    std::atomic<int64_t> last_icy_ms_{0};

    // Generation counter bumped on every info_ write so the overlay can
    // avoid calling current_track() (which locks mu_) every frame.
    std::atomic<uint64_t> info_gen_{0};
};

} // namespace frdx::sources
