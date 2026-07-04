#pragma once

#include "frdx/audio_source.hpp"
#include "frdx/sources/shoutcast_source.hpp"  // StationMetadataMapping, StationMetadataWebSocketFilter, StationMetadataWebSocketHeartbeat

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace frdx::sources {

// ── MetadataController ─────────────────────────────────────────────────
//
// Owns the background metadata polling threads for a ShoutcastSource:
//   - ICY (Icecast) poller: direct HTTP to the stream URL, parses
//     interleaved StreamTitle blocks from the audio stream.
//   - WebSocket poller: persistent connection with live push updates
//     from the station's metadata server.
//
// The controller is designed to be owned by ShoutcastSource. When metadata
// is ready, it invokes a callback (locked under ShoutcastSource::mu_) that
// queues the track info for timed application via the PCM silence-gap
// detector in pump().
//
// Threading:  on_metadata is called from the ICY/WS background threads.
// The callee (ShoutcastSource) must acquire its own internal lock when
// handling the callback.  notify_ffmpeg_icy_metadata() is called from
// ShoutcastSource::pump() (the DSP mixer thread) and is lock-free.
class MetadataController {
public:
    // Called when a background poller has new track metadata ready.
    //   - info:  title/artist to queue
    //   - now_ms:  steady_clock timestamp
    //   - epoch:  metadata epoch (caller discards if mismatched)
    // Returns true if the metadata was accepted/queued.
    using OnMetadata = std::function<bool(TrackInfo info, int64_t now_ms, uint64_t epoch)>;

    // winhttp_session may be nullptr (each poller creates a local session).
    explicit MetadataController(OnMetadata on_metadata, void* winhttp_session = nullptr);
    ~MetadataController();

    // Non-copyable, non-movable — holds jthreads and atomics whose
    // addresses must remain stable for the background loops.
    MetadataController(const MetadataController&)            = delete;
    MetadataController& operator=(const MetadataController&) = delete;
    MetadataController(MetadataController&&)                 = delete;
    MetadataController& operator=(MetadataController&&)      = delete;

    // ── Polling control ──────────────────────────────────────────────

    // Start the appropriate metadata poller(s) for the given station config.
    // Stops any existing pollers first.  Safe to call from any thread (the
    // caller typically holds ShoutcastSource::mu_ during the dispatch phase
    // and releases it before the actual HTTP/WS connections begin).
    void start(const std::string& stream_url,
               const std::string& metadata_url,
               const std::string& metadata_type,
               const StationMetadataMapping& mapping,
               const StationMetadataWebSocketFilter& ws_filter,
               const StationMetadataWebSocketHeartbeat& ws_heartbeat,
               const StationMetadataWebSocketSubscribe& ws_subscribe,
               int poll_interval_secs,
               uint64_t metadata_epoch);

    // Stop all background poller threads.  Blocks until they've joined.
    // Public wrapper is thread-safe; internal stop_unlocked() does the work.
    void stop();

    // ── Configuration ───────────────────────────────────────────────

    void set_winhttp_session(void* session) noexcept { winhttp_session_ = session; }

    // ── Cross-thread notifications ───────────────────────────────────

    // Called by ShoutcastSource::pump() when ffmpeg stderr has produced
    // an ICY StreamTitle.  The WS poller uses this to suppress its own
    // metadata when ffmpeg's same-connection ICY is actively delivering.
    void notify_ffmpeg_icy_metadata(int64_t now_ms) noexcept;

    // Called by ShoutcastSource::pump() when the separate direct-ICY
    // connection has delivered metadata — same suppression logic as above.
    void notify_icy_metadata(int64_t now_ms) noexcept;

private:
    // Internal stop without locking — caller must hold ctrl_mu_.
    void stop_unlocked();

    // ── Poller thread bodies ─────────────────────────────────────────
    void run_icy_poller(const std::string& stream_url, uint64_t metadata_epoch,
                        const std::stop_token& stop);
    void run_ws_poller(std::string url,
                       const StationMetadataMapping& mapping,
                       const StationMetadataWebSocketFilter& filter,
                       StationMetadataWebSocketHeartbeat hb,
                       StationMetadataWebSocketSubscribe sub,
                       uint64_t metadata_epoch,
                       const std::stop_token& stop);
    void run_sse_poller(std::string url,
                        const StationMetadataMapping& mapping,
                        const StationMetadataWebSocketFilter& filter,
                        uint64_t metadata_epoch,
                        const std::stop_token& stop);
    void run_rest_poller(std::string url,
                         const StationMetadataMapping& mapping,
                         int poll_interval_secs,
                         uint64_t metadata_epoch,
                         const std::stop_token& stop);
    void process_sse_event(const std::string& event_data,
                           const StationMetadataMapping& mapping,
                           const StationMetadataWebSocketFilter& filter,
                           uint64_t metadata_epoch,
                           const std::stop_token& stop);

    // ── State ────────────────────────────────────────────────────────
    OnMetadata on_metadata_;
    void* winhttp_session_;

    // Serializes public start() / stop() calls.  Without this, two threads
    // (e.g. overlay station switch + control-loop on_radio_audible) can
    // race on the jthread members and double-join or corrupt state.
    std::mutex ctrl_mu_;

    mutable std::mutex ws_mu_;
    void* ws_handle_ = nullptr;  // active WebSocket handle for forced close
    void* sse_handle_ = nullptr; // active SSE HTTP request handle for forced close

    std::jthread metadata_thread_;   // WS/REST poller
    std::jthread icy_poller_thread_; // ICY poller

    // Timestamps of the last ICY metadata delivery — used by both pollers
    // to skip overwriting when a fresher source is active.
    std::atomic<int64_t> last_icy_ms_{0};
    std::atomic<int64_t> last_ffmpeg_icy_ms_{0};

    // ICY read buffer — reused across run_icy_poller() invocations.
    std::vector<char> icy_buf_;
};

} // namespace frdx::sources
