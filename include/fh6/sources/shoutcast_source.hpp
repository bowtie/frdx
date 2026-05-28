#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fh6::sources {

// ---- Station preset data structures ----

struct StationMetadataMapping {
    std::string title;       // JSON dot-path, e.g. "data.track_title"
    std::string artist;      // JSON dot-path, e.g. "data.track_artist"
    std::string artwork_url; // JSON dot-path, e.g. "data.artwork_urls.large"
    std::string
        artist_array_field; // if artist resolves to array of objects, extract this field from each
    std::string artist_separator{", "}; // join array elements with this separator
    std::string
        split_separator; // when set, split a combined "artist - title" value from a single field
    std::string artwork_url_template; // "https://cdn.example.com/covers/{value}" — {value} replaced
                                      // by resolved artwork_url
};

struct StationStream {
    std::string id;
    std::string name;
    std::string url;
    std::string icon;
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
    std::string static_cover_url;

    // WebSocket metadata (type == "websocket").
    StationMetadataWebSocketFilter ws_filter;
    StationMetadataWebSocketHeartbeat ws_heartbeat;
};

struct StationPreset {
    std::string id;
    std::string name;
    std::string description;
    std::string icon;
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

    std::string_view name() const noexcept override { return "shoutcast"; }
    std::string_view display_name() const noexcept override {
        // No lock — display_name_ is only written under mu_ (in
        // start_pipe_locked) from the HTTP server thread, and read from
        // the audio thread.  A lock with string_view return creates a
        // TOCTOU window where the pointed-to memory can be freed before
        // the caller finishes copying.
        return display_name_;
    }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
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
    void select_station(const std::string& station_id, const std::string& stream_id);

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override { return auth_; }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

private:
    struct Pipe;

    void start_pipe_locked();             // mu_ held
    void stop_pipe_locked();              // mu_ held
    void cycle_preset(int direction = 1); // cycle through all streams by one step, mu_ held
    void stop_metadata_poller();          // join bg thread (no-op now, kept for API compat)

    ShoutcastConfig cfg_;
    std::unique_ptr<Pipe> pipe_;

    mutable std::mutex mu_;
    std::string target_url_;
    std::size_t preset_idx_ = 0;
    TrackInfo info_{};
    std::atomic<uint64_t> position_ms_{0};
    AuthState auth_ = AuthState::none_required;
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

    // Metadata poller thread (background REST polling).
    std::jthread metadata_thread_;
    std::atomic<bool> metadata_stop_{false};

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
    // metadata, they skip overwriting title/artist and only update artwork.
    std::atomic<int64_t> last_icy_ms_{0};
};

} // namespace fh6::sources
