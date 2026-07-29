#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/sources/shoutcast_helpers.hpp"
#include "frdx/sources/shoutcast_metadata.hpp"
#include "frdx/log.hpp"
#include "frdx/overlay/overlay_priv.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// The Clang thread-safety analyzer doesn't understand std::jthread boundaries
// and traces lock state from queue_timed_metadata_locked() (which requires mu_)
// through MetadataController callbacks, producing false-positive warnings.
// File-level suppression is the only reliable mechanism.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wthread-safety"
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
#endif

namespace frdx::sources {

using json = nlohmann::json;
using namespace helpers;

namespace {

// PCM contract: 48000 Hz * 2 ch * 2 bytes (decoded by mpg123).
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

// Load station presets from a JSON file matching the schema the user provided.
std::vector<StationPreset> parse_stations_json(const std::filesystem::path& path) {
    std::vector<StationPreset> out;

    std::ifstream in{path};
    if (!in) {
        log::warn("[shoutcast] stations file not found at {}", path.string());
        return out;
    }

    json root;
    try {
        root = json::parse(in);
    } catch (const std::exception& e) {
        log::warn("[shoutcast] failed to parse {}: {}", path.string(), e.what());
        return out;
    }

    // Support both array-of-stations and single-station root.
    auto parse_station = [](const json& j) -> StationPreset {
        StationPreset sp;
        sp.id   = j.value("id", "");
        sp.name = j.value("name", "");
        if (j.contains("streams") && j["streams"].is_array()) {
            const auto& streams = j["streams"];
            sp.streams.reserve(streams.size());
            for (const auto& s : streams) {
                StationStream ss;
                ss.id   = s.value("id", "");
                ss.name = s.value("name", "");
                ss.url  = s.value("url", "");
                ss.artwork_url = s.value("artwork_url", "");
                // Per-stream mapping override.
                if (s.contains("mapping")) {
                    const auto& mp                         = s["mapping"];
                    ss.mapping_override.title              = mp.value("title", "");
                    ss.mapping_override.artist             = mp.value("artist", "");
                    ss.mapping_override.title_fallback     = mp.value("title_fallback", "");
                    ss.mapping_override.artwork_url        = mp.value("artwork_url", "");
                    ss.mapping_override.artwork_url_template = mp.value("artwork_url_template", "");
                    ss.mapping_override.artist_array_field = mp.value("artist_array_field", "");
                    ss.mapping_override.artist_separator   = mp.value("artist_separator", ", ");
                    ss.mapping_override.split_separator    = mp.value("split_separator", "");
                }
                if (!ss.url.empty()) sp.streams.push_back(std::move(ss));
            }
        }
        if (j.contains("metadata")) {
            const auto& m                  = j["metadata"];
            sp.metadata.type               = m.value("type", "");
            sp.metadata.url                = m.value("url", "");
            if (m.contains("mapping")) {
                const auto& mp                         = m["mapping"];
                sp.metadata.mapping.title              = mp.value("title", "");
                sp.metadata.mapping.artist             = mp.value("artist", "");
                sp.metadata.mapping.title_fallback     = mp.value("title_fallback", "");
                sp.metadata.mapping.artwork_url        = mp.value("artwork_url", "");
                sp.metadata.mapping.artwork_url_template = mp.value("artwork_url_template", "");
                sp.metadata.mapping.artist_array_field = mp.value("artist_array_field", "");
                sp.metadata.mapping.artist_separator   = mp.value("artist_separator", ", ");
                sp.metadata.mapping.split_separator    = mp.value("split_separator", "");
            }
            sp.metadata.poll_interval_secs = m.value("poll_interval_secs", 0);
            if (m.contains("stream_url_map") && m["stream_url_map"].is_object()) {
                for (auto it = m["stream_url_map"].begin(); it != m["stream_url_map"].end(); ++it)
                    sp.metadata.stream_url_map[it.key()] = it.value().get<std::string>();
            }
            // Static metadata (type == "static").
            sp.metadata.static_title  = m.value("title", "");
            sp.metadata.static_artist = m.value("artist", "");
            sp.metadata.static_artwork_url = m.value("cover_url", "");
            sp.metadata.station_logo_only = m.value("station_logo_only", false);
            // WebSocket metadata (type == "websocket").
            if (m.contains("message_filter")) {
                const auto& mf                   = m["message_filter"];
                sp.metadata.ws_filter.op_field   = mf.value("op_field", "");
                sp.metadata.ws_filter.op_value   = mf.value("op_value", 0);
                sp.metadata.ws_filter.type_field = mf.value("type_field", "");
                sp.metadata.ws_filter.type_value = mf.value("type_value", "");
                sp.metadata.ws_filter.data_field = mf.value("data_field", "");
                if (mf.contains("stream_type_value_map") && mf["stream_type_value_map"].is_object()) {
                    for (auto it = mf["stream_type_value_map"].begin();
                         it != mf["stream_type_value_map"].end(); ++it) {
                        sp.metadata.ws_filter.stream_type_value_map[it.key()] =
                            it.value().get<std::string>();
                    }
                }
            }
            if (m.contains("heartbeat")) {
                const auto& hb                          = m["heartbeat"];
                sp.metadata.ws_heartbeat.message        = hb.value("message", "");
                sp.metadata.ws_heartbeat.interval_field = hb.value("interval_field", "");
                sp.metadata.ws_heartbeat.default_interval_ms =
                    hb.value("default_interval_ms", 45000);
            }
            if (m.contains("subscribe")) {
                const auto& sub                        = m["subscribe"];
                sp.metadata.ws_subscribe.message       = sub.value("message", "");
                if (sub.contains("stream_message_map") && sub["stream_message_map"].is_object()) {
                    for (auto it = sub["stream_message_map"].begin();
                         it != sub["stream_message_map"].end(); ++it) {
                        sp.metadata.ws_subscribe.stream_message_map[it.key()] =
                            it.value().get<std::string>();
                    }
                }
            }
        }
        return sp;
    };

    if (root.is_array()) {
        out.reserve(root.size());
        for (const auto& j : root) out.push_back(parse_station(j));
    } else {
        out.push_back(parse_station(root));
    }

    log::info("[shoutcast] loaded {} station(s) from {}", out.size(), path.string());
    return out;
}

} // namespace

// ---- Stream ----

struct ShoutcastSource::Stream {
    std::unique_ptr<Mpg123Stream> stream;
    std::uint64_t bytes_written = 0;
    std::uint64_t metadata_epoch = 0;
    bool ended                  = false;

    ~Stream() = default;
};

// ---- ShoutcastSource ----

ShoutcastSource::ShoutcastSource(ShoutcastConfig cfg) : cfg_{std::move(cfg)} {
    metadata_controller_ = std::make_unique<MetadataController>(
        [this](TrackInfo info, int64_t now_ms, uint64_t epoch) -> bool {
            frdx::ScopedLock lk{mu_};
            return queue_timed_metadata_locked(std::move(info), now_ms, epoch);
        }
    );
    info_        = TrackInfo{};
    info_.artist = "Select a station";
    clear_pending_metadata_locked();
    info_gen_.fetch_add(1, std::memory_order_release);
}

ShoutcastSource::~ShoutcastSource() {
    reconnect_in_progress_.store(false, std::memory_order_release);
    if (reconnect_thread_.joinable()) reconnect_thread_.join();
    stop_stream_locked();
}

bool ShoutcastSource::initialize() {
    if (!cfg_.default_url.empty()) {
        frdx::ScopedLock lk{mu_};
        target_url_ = cfg_.default_url;
    }

    // Create a persistent shared WinHTTP session with default timeouts.
    ScopedWinHttp session(WinHttpOpen(L"forza-radio-dx/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session) {
        DWORD to = 3000; // 3 seconds timeout
        WinHttpSetOption(session.get(), WINHTTP_OPTION_CONNECT_TIMEOUT,
                         &to, sizeof(to));
        WinHttpSetOption(session.get(), WINHTTP_OPTION_SEND_TIMEOUT, &to,
                         sizeof(to));
        WinHttpSetOption(session.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT,
                         &to, sizeof(to));
        winhttp_session_ = session.release();
        metadata_controller_->set_winhttp_session(winhttp_session_);
    } else {
        log::warn("[shoutcast] failed to initialize shared WinHTTP session; falling back to local "
                  "sessions");
    }

    return true;
}

void ShoutcastSource::shutdown() noexcept {
    metadata_controller_->stop();
    reconnect_in_progress_.store(false, std::memory_order_release);
    if (reconnect_thread_.joinable()) reconnect_thread_.join();
    {
        frdx::ScopedLock lk{mu_};
        stop_stream_locked();
    }
    if (winhttp_session_) {
        ScopedWinHttp give_me_a_name(static_cast<HINTERNET>(winhttp_session_));
        winhttp_session_ = nullptr;
    }
}

void ShoutcastSource::load_stations(const std::filesystem::path& path) {
    auto loaded = parse_stations_json(path);
    frdx::ScopedLock lk{mu_};
    stations_ = std::move(loaded);
    // Rebuild station index for O(1) lookup by ID.
    station_index_.clear();
    station_index_.reserve(stations_.size());
    // Rebuild flat preset cache for cycle_preset().
    flat_presets_.clear();
    for (std::size_t i = 0; i < stations_.size(); ++i) {
        station_index_[stations_[i].id] = i;
        for (std::size_t j = 0; j < stations_[i].streams.size(); ++j)
            flat_presets_.emplace_back(i, j);
    }
    log::info("[shoutcast] {} station(s) available", stations_.size());
}

// ---- mpg123 in-process stream launcher (no mu_ held) ----

std::expected<std::unique_ptr<ShoutcastSource::Stream>, ShoutcastError>
ShoutcastSource::launch_stream(const std::string& url, void* winhttp_session) {
    auto s = std::make_unique<Stream>();

    auto result = Mpg123Stream::create(url, winhttp_session);
    if (!result) {
        log::warn("[shoutcast] mpg123 stream creation failed for {}", url);
        return std::unexpected{ShoutcastError::spawn_failed};
    }

    s->stream = std::move(*result);
    return s;
}

// ---- select_station ----

void ShoutcastSource::select_station(const std::string& station_id, const std::string& stream_id) {
    // Stop old metadata producers before publishing the new active station.
    // Otherwise a late WS/ICY write can briefly overwrite the freshly selected
    // station metadata.
    metadata_controller_->stop();

    // Phase 1: configure under lock — fast path, no stream launch.
    std::string launch_url;
    std::string md_type;
    std::string station_name;
    std::string stream_name;
    std::string md_url;
    std::string artwork_url;
    TrackInfo static_info;
    bool has_static = false;
    {
        frdx::ScopedLock lk{mu_};

        const StationPreset* station = nullptr;
        const StationStream* stream  = nullptr;
        auto sit = station_index_.find(station_id);
        if (sit != station_index_.end()) {
            station = &stations_[sit->second];
        }
        if (!station) {
            log::warn("[shoutcast] station '{}' not found", station_id);
            return;
        }

        for (const auto& s : station->streams) {
            if (s.id == stream_id) {
                stream = &s;
                break;
            }
        }
        if (!stream) {
            log::warn("[shoutcast] stream '{}' not found in station '{}'", stream_id, station_id);
            return;
        }

        target_url_        = stream->url;
        active_station_id_ = station_id;
        active_stream_id_  = stream_id;
        launch_url         = target_url_;
        station_name       = station->name;
        stream_name        = stream->name;
        artwork_url        = stream->artwork_url;

        // Resolve the metadata URL / type for this stream.
        metadata_url_.clear();
        md_type = station->metadata.type;
        if (md_type == "websocket" || md_type == "rest" || md_type == "sse" ||
            md_type == "icecast-json") {
            auto mit      = station->metadata.stream_url_map.find(stream_id);
            md_url        = (mit != station->metadata.stream_url_map.end()) ? mit->second
                                                                            : station->metadata.url;
            metadata_url_ = md_url;
        }

        // Handle static metadata: set info_ directly, no poller needed.
        if (md_type == "static") {
            has_static         = true;
            static_info.title  = sanitize_brackets(station->metadata.static_title.empty()
                                                       ? station->name
                                                       : station->metadata.static_title);
            static_info.artist = sanitize_brackets(station->metadata.static_artist);
        }
        // Station logo fallback for any metadata type.
        if (static_info.artwork_url.empty())
            static_info.artwork_url = station->metadata.static_artwork_url;

        // Stop old stream (fast — no HTTP reconnect here).
        stop_stream_locked();
        reset_metadata_sync_locked();
        metadata_controller_->notify_icy_metadata(0);
    } // mu_ released — stream launch happens outside the lock

    // Phase 2: launch mpg123 stream OUTSIDE the lock (HTTP handshake).
    // Only launch when the radio is audible AND the user has actually
    // requested playback (playing/buffering). Without the state_ check,
    // switching stations while paused or before FMOD is hooked would
    // launch the stream into a void — audible_ defaults to true and is
    // only flipped by the control loop once FMOD is installed, so it
    // can't gate the pre-FMOD case on its own. The overlay's FMOD-ready
    // auto-play hook calls play() to start streaming when ready.
    bool should_launch = false;
    {
        frdx::ScopedLock lk{mu_};
        const auto st = state_.load(std::memory_order_relaxed);
        const bool user_playing = (st == PlaybackState::playing ||
                                   st == PlaybackState::buffering);
        should_launch = audible_ && user_playing && !launch_url.empty();
    }
    auto new_stream_result = should_launch
                               ? launch_stream(launch_url, winhttp_session_)
                               : std::expected<std::unique_ptr<Stream>, ShoutcastError>{nullptr};
    auto new_stream = new_stream_result ? std::move(*new_stream_result) : nullptr;

    // Phase 3: install stream and set metadata under lock.
    bool has_stream = false;
    {
        frdx::ScopedLock lk{mu_};
        // Only install if we're still on the same station (not changed during launch).
        if (active_station_id_ == station_id && active_stream_id_ == stream_id) {
            const auto metadata_epoch = reset_metadata_sync_locked();
            if (new_stream) {
                new_stream->metadata_epoch = metadata_epoch;
                stream_.store(std::shared_ptr<Stream>(std::move(new_stream)),
                            std::memory_order_release);
                has_stream = true;
            }

            if (has_static) {
                info_ = static_info;
                metadata_epoch_has_track_ = true;
            } else {
                info_        = TrackInfo{};
                info_.title  = station_name;
                info_.artist = stream_name;
                info_.is_placeholder = true;
                auto msit = station_index_.find(station_id);
                if (msit != station_index_.end()) {
                    info_.multi_stream = stations_[msit->second].streams.size() > 1;
                }
            }
            info_.artwork_url = artwork_url;
            info_gen_.fetch_add(1, std::memory_order_release);
            display_name_ = station_name;
            display_name_gen_.fetch_add(1, std::memory_order_release);
            position_ms_.store(0, std::memory_order_release);
            // Don't set buffering/playing if we didn't launch a stream.
            if (has_stream) {
                state_.store(PlaybackState::buffering, std::memory_order_release);
            } else {
                state_.store(PlaybackState::stopped, std::memory_order_release);
            }
            station_selected_ms_ = steady_ms();
        }
    }

    // Start the appropriate new poller.
    restart_metadata_for_current();

    if (has_stream) {
        state_.store(PlaybackState::playing, std::memory_order_release);
    }

    log::info("[shoutcast] selected station '{}' stream '{}' ({}) metadata={} type={}", station_id,
              stream_id, launch_url, md_url.empty() ? "none" : md_url,
              md_type.empty() ? "icy" : md_type);
}

void ShoutcastSource::cycle_preset(int direction) {
    if (direction >= 0) {
        direction = 1;
    } else {
        direction = -1;
    }

    if (!flat_presets_.empty()) {
        // Find the current (station_id, stream_id) in the cached flat list.
        std::size_t cur = 0;
        for (; cur < flat_presets_.size(); ++cur) {
            const auto& st  = stations_[flat_presets_[cur].first];
            const auto& str = st.streams[flat_presets_[cur].second];
            if (st.id == active_station_id_ && str.id == active_stream_id_) break;
        }
        // If not found (e.g. no station selected yet), start at index 0
        // for forward, last for backward.
        if (cur >= flat_presets_.size()) cur = (direction > 0) ? flat_presets_.size() - 1 : 0;

        // Advance/retreat by 1, wrapping around.
        std::size_t next    = (cur + flat_presets_.size() + direction) % flat_presets_.size();
        const auto& new_st  = stations_[flat_presets_[next].first];
        const auto& new_str = new_st.streams[flat_presets_[next].second];

        target_url_        = new_str.url;
        active_station_id_ = new_st.id;
        active_stream_id_  = new_str.id;
        metadata_url_.clear();
        if (new_st.metadata.type == "websocket" || new_st.metadata.type == "rest" ||
            new_st.metadata.type == "sse" || new_st.metadata.type == "icecast-json") {
            auto mit = new_st.metadata.stream_url_map.find(new_str.id);
            metadata_url_ =
                (mit != new_st.metadata.stream_url_map.end()) ? mit->second : new_st.metadata.url;
        }
        log::info("[shoutcast] cycled to station '{}' stream '{}'", new_st.id, new_str.id);
        return;
    }

    // Legacy preset mode: cycle through string URLs.
    if (cfg_.presets.empty()) return;
    preset_idx_ = (preset_idx_ + cfg_.presets.size() + direction) % cfg_.presets.size();
    target_url_ = cfg_.presets[preset_idx_];
}

std::string ShoutcastSource::start_stream_locked() {
    // Phase 1: pre-launch setup only.  The HTTP/MP3 handshake (launch_stream)
    // happens OUTSIDE mu_ in the caller; install_stream_locked() is Phase 3.
    stop_stream_locked();
    last_data_ms_.store(0, std::memory_order_release);
    reset_metadata_sync_locked();
    display_name_ = "Online Radio";
    display_name_gen_.fetch_add(1, std::memory_order_release);

    // Default display state until restore_station_display_locked() finds a station.
    info_ = TrackInfo{};
    info_.artist = "Select a station";

    if (target_url_.empty()) {
        // No stream to play — publish the cleared state.
        info_gen_.fetch_add(1, std::memory_order_release);
        return {};
    }
    return target_url_;
}

void ShoutcastSource::install_stream_locked(
    const std::string& launched_url,
    std::expected<std::unique_ptr<Stream>, ShoutcastError> stream_result) {
    // Phase 3: install the stream that was launched outside mu_.
    // Guard against stale launches — if target_url_ changed during the HTTP
    // handshake (another select_station/cycle_and_restart ran concurrently),
    // discard this result; the new selection launches its own stream.
    if (target_url_ != launched_url) {
        log::info("[shoutcast] stream launch stale (target changed during handshake), discarding");
        return;
    }

    if (!stream_result) {
        info_gen_.fetch_add(1, std::memory_order_release);
        return;
    }

    // Compute a fresh metadata epoch at install time (matching select_station's
    // Phase 3).  start_stream_locked() already reset metadata sync, but a
    // concurrent change during the handshake warrants a new epoch here.
    const auto metadata_epoch = reset_metadata_sync_locked();
    (*stream_result)->metadata_epoch = metadata_epoch;
    stream_.store(std::shared_ptr<Stream>(std::move(*stream_result)),
                std::memory_order_release);

    // restore_station_display_locked() overwrites info_ and bumps info_gen_
    // if it finds the active station.  If it doesn't (e.g. legacy preset mode),
    // the default "Select a station" remains and we bump here.
    restore_station_display_locked();
    if (info_.artist == "Select a station" && info_.title.empty()) {
        info_gen_.fetch_add(1, std::memory_order_release);
    }
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);

    log::info("[shoutcast] stream started for {}", launched_url);
}

void ShoutcastSource::stop_stream_locked() {
    stream_.store(nullptr, std::memory_order_release);
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void ShoutcastSource::restore_station_display_locked() {
    if (active_station_id_.empty()) return;
    auto sit = station_index_.find(active_station_id_);
    if (sit == station_index_.end()) return;
    const auto& st = stations_[sit->second];
    info_.title   = st.name;
    display_name_ = st.name;
    display_name_gen_.fetch_add(1, std::memory_order_release);
    info_.artist.clear();
    info_.artwork_url.clear();
    info_.is_placeholder = true;
    info_.multi_stream = st.streams.size() > 1;
    for (const auto& str : st.streams) {
        if (str.id == active_stream_id_) {
            info_.artist = str.name;
            info_.artwork_url = str.artwork_url;
            break;
        }
    }
    if (st.metadata.type == "static") {
        info_.title =
            st.metadata.static_title.empty() ? st.name : st.metadata.static_title;
        info_.artist = st.metadata.static_artist;
        metadata_epoch_has_track_ = true;
    }
    // Station logo fallback for any metadata type.
    if (info_.artwork_url.empty())
        info_.artwork_url = st.metadata.static_artwork_url;
    clear_pending_metadata_locked();
    info_gen_.fetch_add(1, std::memory_order_release);
}

void ShoutcastSource::play() {
    bool started_stream = false;
    bool has_stream = false;
    std::string launch_url;
    {
        frdx::ScopedLock lk{mu_};
        drain_pending_ = true;
        // Flip the UI button to "pause" immediately, even before the HTTP
        // handshake that happens below.  If the user pauses while we are
        // handshaking, the post-handshake check will abort.
        state_.store(PlaybackState::buffering, std::memory_order_release);
        if (!stream_.load(std::memory_order_acquire)) {
            launch_url = start_stream_locked();
            started_stream = true;
            // start_stream_locked() calls stop_stream_locked(), which resets
            // state_ to stopped. Re-assert buffering so the UI stays in the
            // "buffering" state for the handshake and the post-handshake
            // abort check has a stable baseline to compare against.
            state_.store(PlaybackState::buffering, std::memory_order_release);
        }
        // If start_stream_locked() didn't run (stream already existed) or failed to
        // set a station display (no active station), make sure the display is
        // refreshed.  Avoid double-bumping info_gen_ when start_stream_locked()
        // already called restore_station_display_locked().
        if (!started_stream) {
            restore_station_display_locked();
        }
        has_stream = (stream_.load(std::memory_order_acquire) != nullptr);
    } // mu_ released — HTTP handshake happens outside the lock

    // Phase 2: launch the stream OUTSIDE mu_ (the WinHTTP/MP3 handshake can
    // block for several seconds).  Mirrors select_station()'s three-phase
    // pattern and prevents stalling the calling (UI/action) thread.
    if (started_stream && !launch_url.empty()) {
        auto stream_result = launch_stream(launch_url, winhttp_session_);
        {
            frdx::ScopedLock lk{mu_};
            // If the user hit pause while we were handshaking, pause() set
            // state_ to paused.  Don't install a stale stream or flip the UI
            // back to "playing".  (We compare against paused, not != buffering,
            // because start_stream_locked()'s internal stop_stream_locked()
            // transiently resets state_ to stopped — that's an internal reset,
            // not a user pause, and must not abort the launch.)
            if (state_.load(std::memory_order_relaxed) == PlaybackState::paused) {
                return;
            }
            install_stream_locked(launch_url, std::move(stream_result));
            has_stream = (stream_.load(std::memory_order_acquire) != nullptr);
            if (has_stream) {
                state_.store(PlaybackState::playing, std::memory_order_release);
            } else {
                state_.store(PlaybackState::stopped, std::memory_order_release);
            }
        }
    } else if (has_stream) {
        // Resuming an existing stream — no HTTP handshake, so keep it under lock.
        frdx::ScopedLock lk{mu_};
        state_.store(PlaybackState::playing, std::memory_order_release);
    }

    // Restart the metadata poller — pause() no longer stops it synchronously,
    // so unpausing needs to restart it to get live track info again.
    if (has_stream) {
        restart_metadata_for_current();
    }
}

void ShoutcastSource::pause() {
    // NOTE: we intentionally do NOT call metadata_controller_->stop() here.
    // stop() joins the poller threads, which can block the UI thread for the
    // network receive timeout (up to several seconds).  Pausing only needs to
    // stop the audio stream and reset the display; the metadata callbacks will
    // be ignored while state_ is not playing/buffering.  restart_metadata_for_current()
    // is called on resume (action worker) and on station change.
    {
        frdx::ScopedLock lk{mu_};
        stop_stream_locked();
        reset_metadata_sync_locked();
        // Reset to a station placeholder so the HUD shows the station
        // logo + name instead of the stale last track.  For multi-stream
        // stations this includes the stream name (e.g. "Chillwave").
        if (!active_station_id_.empty()) {
            restore_station_display_locked();
        } else {
            info_        = TrackInfo{};
            info_.artist = "Nothing playing";
            display_name_ = "Online Radio";
            display_name_gen_.fetch_add(1, std::memory_order_release);
            info_gen_.fetch_add(1, std::memory_order_release);
        }
    }
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void ShoutcastSource::on_radio_audible(bool audible) {
    bool should_restart_metadata = false;
    std::string launch_url;
    {
        frdx::ScopedLock lk{mu_};
        if (audible == audible_) return;
        audible_ = audible;
        if (audible) {
            // Radio became audible again (exited pause menu, etc).
            // Reconnect to the live stream and drain stale buffered audio.
            log::info("[shoutcast] radio audible -- reconnecting to live stream");
            drain_pending_ = true;
            launch_url = start_stream_locked();
            should_restart_metadata = true;
        } else {
            // Radio became inaudible (pause menu, rewind, etc).
            // Kill the stream to stop buffering — otherwise the live
            // stream keeps filling the ring buffer while paused, and when
            // resumed it plays from the stale buffer position, causing the
            // audio to drift behind the live feed and metadata to desync.
            log::info("[shoutcast] radio inaudible -- disconnecting stream");
            stop_stream_locked();
            // Reset the track info to a station placeholder so the HUD
            // shows the station logo + name instead of the stale last
            // track.  restore_station_display_locked() sets up the
            // placeholder with the station name, stream name (for
            // multi-stream stations), and station logo URL.
            restore_station_display_locked();
        }
    } // mu_ released — HTTP handshake happens outside the lock

    // Phase 2: launch the stream OUTSIDE mu_ (the WinHTTP/MP3 handshake can
    // block for several seconds).  Mirrors select_station()'s three-phase
    // pattern and prevents stalling the control loop thread.
    if (should_restart_metadata && !launch_url.empty()) {
        auto stream_result = launch_stream(launch_url, winhttp_session_);
        {
            frdx::ScopedLock lk{mu_};
            install_stream_locked(launch_url, std::move(stream_result));
        }
    }

    // restart_metadata_for_current() requires mu_ to be free (FRDX_EXCLUDES).
    if (should_restart_metadata) {
        restart_metadata_for_current();
    }
}

void ShoutcastSource::cycle_and_restart(int direction) {
    metadata_controller_->stop();

    bool has_stream = false;
    std::string launch_url;
    {
        frdx::ScopedLock lk{mu_};
        cycle_preset(direction);
        // Only (re)start the stream when the radio is audible AND the user
        // has requested playback — mirrors select_station()'s gate. Without
        // the state_ check, cycling presets while paused or before FMOD is
        // hooked would launch the stream into a void.
        const auto st = state_.load(std::memory_order_relaxed);
        const bool user_playing = (st == PlaybackState::playing ||
                                   st == PlaybackState::buffering);
        if (audible_ && user_playing) {
            launch_url = start_stream_locked();
        } else {
            // Not audible / not playing: update the display info so the
            // station name is ready when the UI comes back, but don't start
            // the stream yet.
            restore_station_display_locked();
        }
        // Always clear pending timed metadata on station swap — the
        // previous station's pending track info is stale and should
        // never be applied to the new station.
        clear_pending_metadata_locked();
        metadata_epoch_has_track_ = false;
        has_stream = (stream_.load(std::memory_order_acquire) != nullptr);
        station_selected_ms_ = steady_ms();
    } // mu_ released — HTTP handshake happens outside the lock

    // Phase 2: launch the stream OUTSIDE mu_ (the WinHTTP/MP3 handshake can
    // block for several seconds).  Mirrors select_station()'s three-phase
    // pattern and prevents stalling the action-worker thread.
    if (!launch_url.empty()) {
        auto stream_result = launch_stream(launch_url, winhttp_session_);
        {
            frdx::ScopedLock lk{mu_};
            install_stream_locked(launch_url, std::move(stream_result));
            has_stream = (stream_.load(std::memory_order_acquire) != nullptr);
        }
    }

    restart_metadata_for_current();
    if (has_stream) {
        state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

void ShoutcastSource::next() { cycle_and_restart(1); }

void ShoutcastSource::previous() { cycle_and_restart(-1); }

TrackInfo ShoutcastSource::current_track() const {
    TrackInfo t;
    {
        frdx::ScopedLock lk{mu_};
        t = info_;
    }
    t.position_ms = position_ms_.load(std::memory_order_acquire);
    return t;
}

bool ShoutcastSource::queue_timed_metadata_locked(TrackInfo info, int64_t now_ms,
                                                  uint64_t metadata_epoch) {
    if (metadata_epoch != metadata_epoch_.load(std::memory_order_acquire)) return false;
    // Ignore metadata updates while paused/stopped — pause() no longer stops
    // the poller synchronously, so callbacks can still arrive.
    const auto st = state_.load(std::memory_order_relaxed);
    if (st != PlaybackState::playing && st != PlaybackState::buffering) return false;
    if (info.title.empty() && info.artist.empty()) return false;
    if (info.title == info_.title && info.artist == info_.artist) {
        clear_pending_metadata_locked();
        return false;
    }
    if (pending_info_valid_ && pending_info_epoch_ == metadata_epoch &&
        info.title == pending_info_.title &&
        info.artist == pending_info_.artist) {
        return false;
    }
    pending_info_        = std::move(info);
    pending_info_ms_     = now_ms;
    quiet_started_ms_    = 0;
    pending_info_epoch_  = metadata_epoch;
    pending_info_valid_  = true;
    has_pending_metadata_.store(true, std::memory_order_release);
    // Don't apply immediately even if there's no track yet — the station
    // logo grace period in observe_pcm_for_metadata_sync() will hold it
    // for a few seconds so the user can see what station they tuned into.
    return true;
}

void ShoutcastSource::apply_pending_metadata_locked(const char* reason) {
    if (!pending_info_valid_) return;
    if (pending_info_epoch_ != metadata_epoch_.load(std::memory_order_acquire)) {
        clear_pending_metadata_locked();
        return;
    }
    info_ = std::move(pending_info_);
    pending_info_ = {};
    pending_info_ms_ = 0;
    quiet_started_ms_ = 0;
    pending_info_epoch_ = 0;
    pending_info_valid_ = false;
    has_pending_metadata_.store(false, std::memory_order_release);
    // the station's cover_url (static_artwork_url) so the texture
    // injector still has something to display.
    //
    // station_logo_only: always use the station logo, ignore poller art.
    bool logo_only = false;
    if (!active_station_id_.empty()) {
        auto sit = station_index_.find(active_station_id_);
        if (sit != station_index_.end()) {
            logo_only = stations_[sit->second].metadata.station_logo_only;
        }
    }
    if (logo_only) {
        info_.artwork_url.clear();
        info_.artwork_stretch = true;
    }
    // When artwork lookup is disabled, ignore the poller-provided
    // artwork_url too — only use the station logo (static_artwork_url).
    const bool artwork_lookup_on = artwork_lookup_enabled_.load(std::memory_order_acquire);
    if (!artwork_lookup_on && !logo_only) {
        info_.artwork_url.clear();
    }
    // Detect sentinel/null artwork URLs that some stations return when
    // there's no cover (e.g. listen.moe returns "https://cdn.listen.moe/covers/null").
    if (!info_.artwork_url.empty()) {
        const auto& url = info_.artwork_url;
        auto ends_with_null = [](std::string_view u) {
            auto qpos = u.find('?');
            if (qpos != std::string_view::npos) u = u.substr(0, qpos);
            auto slash = u.rfind('/');
            std::string_view last = (slash != std::string_view::npos) ? u.substr(slash + 1) : u;
            auto dot = last.rfind('.');
            if (dot != std::string_view::npos) last = last.substr(0, dot);
            return last == "null" || last == "none" || last == "default" || last.empty();
        };
        if (ends_with_null(url)) {
            log::info("[shoutcast] ignoring null/sentinel artwork URL: {}", url);
            info_.artwork_url.clear();
        }
    }
    if (info_.artwork_url.empty() && !active_station_id_.empty()) {
        auto sit = station_index_.find(active_station_id_);
        if (sit != station_index_.end()) {
            info_.artwork_url = stations_[sit->second].metadata.static_artwork_url;
        }
    }

    info_gen_.fetch_add(1, std::memory_order_release);
    metadata_epoch_has_track_ = true;
    log::info("[shoutcast] applied timed metadata after {}: {} - {}", reason, info_.artist,
              info_.title);
}

void ShoutcastSource::clear_pending_metadata_locked() {
    pending_info_ = {};
    pending_info_ms_ = 0;
    quiet_started_ms_ = 0;
    pending_info_epoch_ = 0;
    pending_info_valid_ = false;
    has_pending_metadata_.store(false, std::memory_order_release);
}

uint64_t ShoutcastSource::reset_metadata_sync_locked() {
    clear_pending_metadata_locked();
    metadata_epoch_has_track_ = false;
    metadata_controller_->notify_icy_metadata(0);
    return metadata_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void ShoutcastSource::observe_pcm_for_metadata_sync(const int16_t* samples,
                                                    std::size_t sample_count,
                                                    uint64_t metadata_epoch) {
    // Metadata sync: when a metadata poller reports a new track, we queue
    // it as pending and wait for the right moment to apply it.
    //
    // Primary: quiet-gap detection — between tracks, streams typically drop
    // to near-silence.  We detect this by counting the fraction of "quiet"
    // samples (|sample| below a threshold) with hysteresis to avoid
    // toggling.  This syncs metadata with the actual audio transition.
    //
    // Fallback: if no quiet gap is detected within a window (from when the
    // metadata was queued), apply via fixed delay.  This handles streams
    // with crossfading, no inter-track silence, or encoder dither.
    // Quiet-gap detection: between tracks, streams typically drop to near-
    // digital silence.  We detect this by checking the peak amplitude of
    // each PCM buffer — if the peak is below a very low threshold, the
    // audio is effectively silent (inter-track gap or buffer underrun).
    //
    // Using peak amplitude instead of quiet-ratio is more robust because
    // normal music has brief quiet moments (drum pauses, quiet sections)
    // that can trigger ratio-based detection even though the track is
    // still playing.  Peak amplitude only fires on actual silence.
    constexpr int kSilencePeakThreshold = 200;  // ~0.6% of full scale = -44dB
    constexpr int64_t kQuietHoldMs = 1500;      // 1.5s of silence → apply
    // Fallback delay: if no quiet gap is found within this window after
    // metadata was queued, apply it anyway.  WebSocket metadata can arrive
    // 10-20s before the audio stream actually switches tracks (the WS
    // notifies on the server side, but the HLS/ICE stream has buffer
    // latency).  15s gives the audio time to catch up without being so
    // long that metadata feels stale on gapless streams.
    constexpr int64_t kFallbackDelayMs = 15'000;
    constexpr int64_t kMaxPendingMs = 30'000;
    // Grace period after station selection: hold the station logo + name
    // on-screen for at least this long before the first track metadata
    // overwrites it, so the user can see what station they tuned into.
    const int64_t kStationGracePeriodMs = grace_period_ms_.load(std::memory_order_acquire);

    // Fast path: if no metadata is pending (the common case — music playing,
    // no track change queued), skip the peak-amplitude scan entirely.
    // has_pending_metadata_ is a lock-free mirror of pending_info_valid_
    // that is written under mu_ in the metadata queue/clear/apply paths.
    if (!has_pending_metadata_.load(std::memory_order_acquire)) {
        // Still need to reset quiet_started_ms_ if the epoch changed, but
        // only if it was non-zero (rare).  Avoid taking the lock in the
        // overwhelmingly common case by just returning.
        return;
    }

    // Compute peak amplitude across the buffer.  If the peak is below the
    // silence threshold, the audio is effectively silent.
    int peak = 0;
    for (std::size_t i = 0; i < sample_count; ++i) {
        const int sample = samples[i] == INT16_MIN ? 32768 : std::abs(static_cast<int>(samples[i]));
        if (sample > peak) peak = sample;
    }
    const bool is_quiet = peak < kSilencePeakThreshold;

    const auto now_ms = steady_ms();
    frdx::ScopedLock lk{mu_};
    if (metadata_epoch != metadata_epoch_.load(std::memory_order_acquire) ||
        !pending_info_valid_ || pending_info_epoch_ != metadata_epoch) {
        quiet_started_ms_ = 0;
        return;
    }

    // Don't treat silence as an inter-track gap while the stream is
    // buffering — the silence here is the gap between reconnection and
    // the first audio data arriving, not a gap between tracks.  Applying
    // metadata during this silence would show the next track's info
    // before the current track has finished playing.
    const auto playback_st = state_.load(std::memory_order_acquire);
    if (playback_st != PlaybackState::playing) {
        quiet_started_ms_ = 0;
        return;
    }

    if (pending_info_ms_ > 0 && now_ms - pending_info_ms_ >= kMaxPendingMs) {
        apply_pending_metadata_locked("timeout");
        return;
    }

    // Station logo grace period: if this is the first track metadata after
    // a station change, hold the station logo for a few seconds before
    // allowing the track metadata to take over.
    if (!metadata_epoch_has_track_ && station_selected_ms_ > 0) {
        if (now_ms - station_selected_ms_ < kStationGracePeriodMs) {
            // Still in grace period — keep the quiet gap timer running so
            // we can transition smoothly when the grace period ends.
            if (is_quiet) {
                if (quiet_started_ms_ == 0) quiet_started_ms_ = now_ms;
            } else {
                quiet_started_ms_ = 0;
            }
            return;
        }
        apply_pending_metadata_locked("grace period ended");
        return;
    }

    // Fallback: if no quiet gap was found within the fallback window,
    // apply the metadata anyway.
    if (pending_info_ms_ > 0 && now_ms - pending_info_ms_ >= kFallbackDelayMs) {
        apply_pending_metadata_locked("fallback delay");
        return;
    }

    // Primary: quiet-gap detection.
    if (is_quiet) {
        if (quiet_started_ms_ == 0) quiet_started_ms_ = now_ms;
        if (now_ms - quiet_started_ms_ >= kQuietHoldMs) {
            apply_pending_metadata_locked("quiet gap");
        }
    } else {
        quiet_started_ms_ = 0;
    }
}

// ---- Metadata poller (dispatcher) ----

void ShoutcastSource::restart_metadata_for_current() {
    // Gather station metadata config under mu_, then delegate to
    // metadata_controller_ for the actual polling threads.
    metadata_controller_->stop();

    frdx::UniqueLock lk{mu_};

    // Legacy preset mode (no station selected, raw URL only).
    if (active_station_id_.empty()) {
        std::string legacy_url = target_url_;
        const auto md_epoch = metadata_epoch_.load(std::memory_order_acquire);
        lk.unlock();
        if (!legacy_url.empty()) {
            metadata_controller_->start(legacy_url, "", "", {}, {}, {}, {}, 0, md_epoch);
        }
        return;
    }

    const StationPreset* station = nullptr;
    auto sit = station_index_.find(active_station_id_);
    if (sit != station_index_.end()) {
        station = &stations_[sit->second];
    }
    if (!station) return;

    const StationStream* stream = nullptr;
    for (const auto& s : station->streams) {
        if (s.id == active_stream_id_) { stream = &s; break; }
    }

    auto mapping = station->metadata.mapping;
    if (stream && (!stream->mapping_override.title.empty() ||
                   !stream->mapping_override.split_separator.empty() ||
                   !stream->mapping_override.artist.empty())) {
        mapping = stream->mapping_override;
        log::info("[shoutcast] using stream-level mapping override for '{}'", stream->id);
    }

    auto md_url     = metadata_url_;
    auto md_type    = station->metadata.type;
    auto md_poll    = station->metadata.poll_interval_secs;
    auto ws_filter  = station->metadata.ws_filter;
    // Per-stream type_value override (SSE array filter, e.g. Nightride).
    if (stream && !ws_filter.stream_type_value_map.empty()) {
        auto it = ws_filter.stream_type_value_map.find(stream->id);
        if (it != ws_filter.stream_type_value_map.end())
            ws_filter.type_value = it->second;
    }
    auto ws_hb      = station->metadata.ws_heartbeat;
    auto ws_sub     = station->metadata.ws_subscribe;
    // Per-stream subscribe message override.
    if (stream && !ws_sub.stream_message_map.empty()) {
        auto it = ws_sub.stream_message_map.find(stream->id);
        if (it != ws_sub.stream_message_map.end())
            ws_sub.message = it->second;
    }
    const auto md_epoch = metadata_epoch_.load(std::memory_order_acquire);

    std::string stream_url;
    if (stream && !stream->url.empty())
        stream_url = stream->url;
    if (stream_url.empty() && !station->streams.empty())
        stream_url = station->streams[0].url;

    lk.unlock();

    metadata_controller_->start(stream_url, md_url, md_type, mapping,
                                ws_filter, ws_hb, ws_sub, md_poll, md_epoch);
}

// ---- PCM pump ----

void ShoutcastSource::pump(RingBuffer& ring) {
    // Drain stale buffered audio before processing new data.  This is set
    // by on_radio_audible(true) or play() to discard the old contents of
    // the ring buffer after reconnecting to a live stream, so playback
    // resumes from the current live position instead of behind it.
    {
        frdx::ScopedLock lk{mu_};
        if (drain_pending_) {
            ring.drain();
            drain_pending_ = false;
        }
    }

    {
        auto st = state_.load(std::memory_order_acquire);
        if (st != PlaybackState::playing && st != PlaybackState::buffering) return;
    }

    // Lock-free acquire of the current stream.  Shared ownership ensures the Stream
    // stays alive for the entire call, even if a concurrent stop_stream_locked()
    // replaces stream_ with nullptr.
    auto p = stream_.load(std::memory_order_acquire);
    if (!p) return;
    bool ended = p->ended;

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
        return r;
    };

    // If the stream has ended, kick off a background reconnect so the
    // remaining ring-buffer audio keeps playing without interruption.
    if (ended) {
        if (reconnect_in_progress_.load(std::memory_order_acquire)) {
            // Reconnect already in flight — just update position.
            update_position();
            return;
        }
        // Launch reconnect on a background thread.  The HTTP handshake
        // blocks there, not in pump(), so the control loop keeps ticking
        // and the DSP callback keeps reading from the ring buffer.
        reconnect_in_progress_.store(true, std::memory_order_release);
        int attempts = reconnect_attempts_.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Snapshot the URL and session under lock so the background thread
        // doesn't need mu_ for the handshake.
        std::string url;
        void* session = nullptr;
        {
            frdx::ScopedLock lk{mu_};
            url = target_url_;
            session = winhttp_session_;
            if (attempts <= 3) {
                info_.title  = display_name_;
                info_.artist = "Attempting to reconnect to station";
                clear_pending_metadata_locked();
                info_gen_.fetch_add(1, std::memory_order_release);
            } else {
                reconnect_attempts_.store(0, std::memory_order_release);
                info_.title  = display_name_;
                info_.artist = "Unable to reach this station";
                clear_pending_metadata_locked();
                info_gen_.fetch_add(1, std::memory_order_release);
            }
        }
        // The previous reconnect thread has finished its work (reconnect_in_progress_
        // was false when we entered this branch, so its lambda already returned).
        // Detach instead of joining — we're on the audio render thread and even a
        // brief join is undesirable.  The destructor/shutdown() join the final thread.
        if (reconnect_thread_.joinable()) reconnect_thread_.detach();
        reconnect_thread_ = std::thread([this, url, session, attempts] {
            auto stream_result = launch_stream(url, session);
            if (stream_result && *stream_result) {
                auto new_stream = std::shared_ptr<Stream>(std::move(*stream_result));
                {
                    frdx::ScopedLock lk{mu_};
                    const auto epoch = reset_metadata_sync_locked();
                    new_stream->metadata_epoch = epoch;
                    stream_.store(new_stream, std::memory_order_release);
                    reconnect_attempts_.store(0, std::memory_order_release);
                }
                state_.store(PlaybackState::buffering, std::memory_order_release);
                restart_metadata_for_current();
                log::info("[shoutcast] background reconnect succeeded (attempt {})", attempts);
            } else {
                log::warn("[shoutcast] background reconnect failed (attempt {})", attempts);
                // After 3 failures, cycle to next preset and try it.
                if (attempts >= 3) {
                    {
                        frdx::ScopedLock lk{mu_};
                        if (!cfg_.presets.empty() || !stations_.empty()) {
                            cycle_preset();
                        }
                    }
                    std::string next_url;
                    {
                        frdx::ScopedLock lk{mu_};
                        next_url = target_url_;
                    }
                    auto retry = launch_stream(next_url, session);
                    if (retry && *retry) {
                        auto new_stream = std::shared_ptr<Stream>(std::move(*retry));
                        frdx::ScopedLock lk{mu_};
                        const auto epoch = reset_metadata_sync_locked();
                        new_stream->metadata_epoch = epoch;
                        stream_.store(new_stream, std::memory_order_release);
                        reconnect_attempts_.store(0, std::memory_order_release);
                        state_.store(PlaybackState::buffering, std::memory_order_release);
                    }
                    restart_metadata_for_current();
                }
            }
            reconnect_in_progress_.store(false, std::memory_order_release);
        });
        update_position();
        return;
    }

    if (!p->stream || !p->stream->is_alive()) return;

    // Throttle writing into the ring buffer: only feed the ring when it's
    // below the target level.  200ms of buffered audio (38400 bytes at
    // 48kHz S16 stereo) is enough to absorb timing jitter and short FMOD
    // callback freezes without adding much latency.
    constexpr std::size_t kRingTargetBytes = 38400; // ~200ms
    const std::size_t ring_readable = ring.readable();
    if (ring_readable >= kRingTargetBytes) {
        update_position();
        return;  // ring has enough — let the DSP drain it before adding more
    }

    // Read decoded PCM from mpg123 and write to the ring buffer.
    // We read in chunks until the ring is full enough or no more data
    // is available from the decoder right now.
    bool stream_ended = false;
    std::size_t total_read_this_tick = 0;

    while (ring_readable + total_read_this_tick < kRingTargetBytes) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;

        int16_t buf[2048]; // 1024 stereo frames * 2 channels = 4096 bytes
        std::size_t want = std::min<std::size_t>(writable, sizeof(buf));

        bool ended_flag = false;
        std::size_t got = p->stream->read_pcm(
            std::span<std::byte>(reinterpret_cast<std::byte*>(buf), want), ended_flag);

        if (ended_flag) {
            stream_ended = true;
            break;
        }

        if (got == 0) {
            // No data available right now — check for stall.
            break;
        }

        reconnect_attempts_.store(0, std::memory_order_relaxed);
        last_data_ms_.store(steady_ms(), std::memory_order_release);

        ring.write(reinterpret_cast<std::byte*>(buf), got);
        observe_pcm_for_metadata_sync(buf, got / sizeof(int16_t), p->metadata_epoch);
        p->bytes_written += got;
        total_read_this_tick += got;
    }

    // ── Stall detection ──
    // If the stream is alive but hasn't produced data for >10s, reconnect.
    if (total_read_this_tick == 0 && !stream_ended) {
        const auto now_ms = steady_ms();
        int64_t last = last_data_ms_.load(std::memory_order_acquire);
        if (last == 0) {
            last_data_ms_.store(now_ms, std::memory_order_release);
        } else if (now_ms - last > 10'000) {
            log::warn("[shoutcast] stream stalled (no data for {}ms); reconnecting",
                      now_ms - last);
            stream_ended = true;
            last_data_ms_.store(0, std::memory_order_release);
        }
    }

    if (stream_ended) {
        if (p->bytes_written == 0) {
            log::warn("[shoutcast] stream produced 0 bytes; stopping");
            frdx::UniqueLock lk{mu_};
            stop_stream_locked();
            return;
        }
        p->ended = true;
        {
            frdx::UniqueLock lk{mu_};
            info_.title  = display_name_;
            info_.artist = "Lost connection to station";
            clear_pending_metadata_locked();
            info_gen_.fetch_add(1, std::memory_order_release);
        }
    }

    if (update_position() > 8 * 1024) {
        auto expected = PlaybackState::buffering;
        if (state_.compare_exchange_strong(expected, PlaybackState::playing,
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
            log::info("[shoutcast] buffering complete, transitioning to playing");
        }
    }
}

} // namespace frdx::sources
