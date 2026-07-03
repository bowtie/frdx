#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/sources/shoutcast_helpers.hpp"
#include "frdx/sources/shoutcast_metadata.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"
#include "frdx/overlay/overlay_priv.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <optional>
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

// URL-encode a string for use in a query parameter.
std::string url_encode(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Query the iTunes Search API for album artwork by artist + title.
// Returns the 600x600 artwork URL if found, or empty string on miss/error.
std::string itunes_lookup_artwork(const std::string& artist, const std::string& title,
                                  void* winhttp_session) {
    std::string term = url_encode(artist + " " + title);
    std::string path = "/search?term=" + term + "&entity=song&limit=1";

    // Parse the iTunes URL.
    ParsedUrl parsed;
    {
        std::string url = "https://itunes.apple.com" + path;
        auto opt = ParsedUrl::parse(url);
        if (!opt) return {};
        parsed = *opt;
    }

    auto conn = winhttp_connect(winhttp_session, parsed);
    if (!conn) return {};

    HINTERNET hRequest = conn.request.get();
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return {};
    if (!WinHttpReceiveResponse(hRequest, nullptr)) return {};

    std::string body;
    char buf[8192];
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available)) break;
        if (available == 0) break;
        DWORD to_read = (available < sizeof(buf) - 1)
                            ? available
                            : static_cast<DWORD>(sizeof(buf) - 1);
        DWORD bytes_read = 0;
        if (!WinHttpReadData(hRequest, buf, to_read, &bytes_read)) break;
        if (bytes_read == 0) break;
        body.append(buf, bytes_read);
    }

    if (body.empty()) return {};

    try {
        auto msg = json::parse(body);
        const auto& results = msg.value("results", json::array());
        if (!results.is_array() || results.empty()) return {};
        const auto& first = results[0];
        std::string art = first.value("artworkUrl100", "");
        if (art.empty()) return {};
        // Upscale from 100x100 to 600x600.
        auto pos = art.find("100x100bb");
        if (pos != std::string::npos)
            art.replace(pos, 9, "600x600bb");
        return art;
    } catch (...) {
        return {};
    }
}

// Query MusicBrainz + Cover Art Archive for album artwork by artist + title.
// 2nd-tier fallback after iTunes — best for obscure/doujin/Vocaloid tracks
// that iTunes misses.  MusicBrainz rate-limits to 1 req/sec, so this function
// may sleep up to ~2s when checking multiple releases.
// Returns the CAA front-cover URL if found, or empty string on miss/error.
std::string musicbrainz_lookup_artwork(const std::string& artist,
                                       const std::string& title,
                                       void* winhttp_session) {
    // 1. Search MusicBrainz for the recording.
    std::string query = "artist:" + artist + " AND recording:" + title;
    std::string path = "/ws/2/recording/?query=" + url_encode(query) +
                       "&fmt=json&limit=1";

    ParsedUrl parsed;
    {
        std::string url = "https://musicbrainz.org" + path;
        auto opt = ParsedUrl::parse(url);
        if (!opt) return {};
        parsed = *opt;
    }

    auto conn = winhttp_connect(winhttp_session, parsed);
    if (!conn) return {};

    // MusicBrainz asks for a descriptive User-Agent with contact info.
    WinHttpAddRequestHeaders(
        conn.request.get(),
        L"User-Agent: forza-radio-dx/1.0",
        (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    HINTERNET hRequest = conn.request.get();
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return {};
    if (!WinHttpReceiveResponse(hRequest, nullptr)) return {};

    std::string body;
    char buf[8192];
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available)) break;
        if (available == 0) break;
        DWORD to_read = (available < sizeof(buf) - 1)
                            ? available
                            : static_cast<DWORD>(sizeof(buf) - 1);
        DWORD bytes_read = 0;
        if (!WinHttpReadData(hRequest, buf, to_read, &bytes_read)) break;
        if (bytes_read == 0) break;
        body.append(buf, bytes_read);
    }

    if (body.empty()) return {};

    // Extract release MBIDs from the first recording's releases.
    std::vector<std::string> mbids;
    try {
        auto msg = json::parse(body);
        const auto& recordings = msg.value("recordings", json::array());
        if (!recordings.is_array() || recordings.empty()) return {};
        const auto& releases = recordings[0].value("releases", json::array());
        if (!releases.is_array()) return {};
        for (const auto& rel : releases) {
            std::string mbid = rel.value("id", "");
            if (!mbid.empty()) {
                mbids.push_back(mbid);
                if (mbids.size() >= 3) break;
            }
        }
    } catch (...) {
        return {};
    }

    if (mbids.empty()) return {};

    // 2. Check Cover Art Archive for each release (up to 3).
    // CAA /front returns 307 -> image URL (WinHTTP follows redirects),
    // or 404 if no cover exists.
    for (size_t i = 0; i < mbids.size(); ++i) {
        if (i > 0) std::this_thread::sleep_for(std::chrono::seconds(1));

        std::string caa_url =
            "https://coverartarchive.org/release/" + mbids[i] + "/front";
        auto caa_opt = ParsedUrl::parse(caa_url);
        if (!caa_opt) continue;
        auto caa_conn = winhttp_connect(winhttp_session, *caa_opt);
        if (!caa_conn) continue;

        HINTERNET hCaaReq = caa_conn.request.get();
        if (!WinHttpSendRequest(hCaaReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            continue;
        if (!WinHttpReceiveResponse(hCaaReq, nullptr)) continue;

        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        if (WinHttpQueryHeaders(hCaaReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                WINHTTP_HEADER_NAME_BY_INDEX)) {
            if (status_code == 200)
                return caa_url;
        }
    }

    return {};
}

// PCM contract written by ffmpeg: 48000 Hz * 2 ch * 2 bytes.
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

// Resolve the ffmpeg binary path.  Checks the configured path first, then
// a bundled <game>/mods/frdx/ffmpeg/ffmpeg.exe, then falls back to PATH lookup.
std::wstring resolve_ffmpeg_path(const std::filesystem::path& configured) {
    if (!configured.empty()) return configured.wstring();
    // Try bundled ffmpeg.exe next to the game executable.
    wchar_t game_path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, game_path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        auto bundled = std::filesystem::path{game_path}.parent_path() / "mods" / "frdx" / "ffmpeg" /
                       "ffmpeg.exe";
        std::error_code ec;
        if (std::filesystem::exists(bundled, ec)) return bundled.wstring();
    }
    // Fall back to PATH lookup.
    return L"ffmpeg";
}

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
            sp.metadata.prefer_itunes_lookup = m.value("prefer_itunes_lookup", false);
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
                         it != mf["stream_type_value_map"].end(); ++it)
                        sp.metadata.ws_filter.stream_type_value_map[it.key()] =
                            it.value().get<std::string>();
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
                         it != sub["stream_message_map"].end(); ++it)
                        sp.metadata.ws_subscribe.stream_message_map[it.key()] =
                            it.value().get<std::string>();
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

// ---- Pipe ----

struct ShoutcastSource::Pipe {
    HANDLE job         = nullptr;
    HANDLE proc_ff     = nullptr;
    HANDLE read_pipe   = nullptr;
    HANDLE stderr_pipe = nullptr;
    std::string stderr_buf;
    std::uint64_t bytes_written = 0;
    std::uint64_t metadata_epoch = 0;
    bool ended                  = false;
    bool stderr_empty           = false;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (stderr_pipe) CloseHandle(stderr_pipe);
        if (proc_ff) CloseHandle(proc_ff);
        if (job) CloseHandle(job);
    }
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
    stop_pipe_locked();
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
    {
        frdx::ScopedLock lk{mu_};
        stop_pipe_locked();
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
    // Rebuild flat preset cache for cycle_preset().
    flat_presets_.clear();
    for (std::size_t i = 0; i < stations_.size(); ++i) {
        for (std::size_t j = 0; j < stations_[i].streams.size(); ++j)
            flat_presets_.emplace_back(i, j);
    }
    log::info("[shoutcast] {} station(s) available", stations_.size());
}

// ---- ffmpeg subprocess launcher (no mu_ held) ----

std::expected<std::unique_ptr<ShoutcastSource::Pipe>, ShoutcastError>
ShoutcastSource::launch_ffmpeg_pipe(const std::string& url,
                                    const std::filesystem::path& ffmpeg_path,
                                    bool volume_normalization) {
    auto pipe = std::make_unique<Pipe>();
    pipe->job = frdx::subprocess::create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[shoutcast] CreateJobObject failed ({})", GetLastError());
        return std::unexpected{ShoutcastError::pipe_creation_failed};
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;

    auto bail = [&] {
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
        if (err_r) CloseHandle(err_r);
        if (err_w) CloseHandle(err_w);
    };

    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) {
        bail();
        return std::unexpected{ShoutcastError::pipe_creation_failed};
    }
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);

    // Stderr pipe for ICY metadata. ffmpeg prints [icy] StreamTitle='...' lines here.
    if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) {
        bail();
        return std::unexpected{ShoutcastError::pipe_creation_failed};
    }
    SetHandleInformation(err_r, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in = frdx::subprocess::open_nul(GENERIC_READ);

    const auto ff = resolve_ffmpeg_path(ffmpeg_path);

    std::wstring ff_cmd = frdx::subprocess::quote(ff) +
                          L" -hide_banner -nostats -loglevel info"
                          L" -icy 1"
                          L" -user_agent \"WinampMPEG/5.0\""
                          L" -reconnect 1"
                          L" -reconnect_streamed 1"
                          L" -reconnect_delay_max 5"
                          L" -i " +
                          frdx::subprocess::quote(frdx::subprocess::widen(url));
    if (volume_normalization) {
        ff_cmd += L" -af loudnorm=I=-14:TP=-2:LRA=11";
    }
    ff_cmd += L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc_ff     = frdx::subprocess::spawn_in_job(pipe->job, ff_cmd, nul_in, ff_out_w, err_w);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(ff_out_w);
    CloseHandle(err_w);
    ff_out_w = nullptr;
    err_w    = nullptr;
    if (nul_in) CloseHandle(nul_in);

    if (!pipe->proc_ff) {
        log::warn(
            "[shoutcast] failed to launch ffmpeg -- {}",
            frdx::subprocess::describe_launch_failure(std::wstring{ff}, ec_ff, ff != L"ffmpeg"));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (err_r) CloseHandle(err_r);
        return std::unexpected{ShoutcastError::spawn_failed};
    }

    pipe->read_pipe   = ff_out_r;
    pipe->stderr_pipe = err_r;
    return pipe;
}

// ---- select_station ----

void ShoutcastSource::select_station(const std::string& station_id, const std::string& stream_id) {
    // Stop old metadata producers before publishing the new active station.
    // Otherwise a late WS/ICY write can briefly overwrite the freshly selected
    // station metadata.
    metadata_controller_->stop();

    // Phase 1: configure under lock — fast path, no ffmpeg launch.
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
        for (const auto& s : stations_) {
            if (s.id == station_id) {
                station = &s;
                break;
            }
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

        // Stop old pipe (fast — no ffmpeg launch here).
        stop_pipe_locked();
        reset_metadata_sync_locked();
        metadata_controller_->notify_icy_metadata(0);
        metadata_controller_->notify_ffmpeg_icy_metadata(0);
    } // mu_ released — ffmpeg launch happens outside the lock

    // Phase 2: launch ffmpeg OUTSIDE the lock (expensive subprocess creation).
    auto new_pipe_result = launch_url.empty()
                               ? std::expected<std::unique_ptr<Pipe>, ShoutcastError>{nullptr}
                               : launch_ffmpeg_pipe(launch_url, cfg_.ffmpeg_path,
                                                     volume_norm_.load(std::memory_order_acquire));
    auto new_pipe = new_pipe_result ? std::move(*new_pipe_result) : nullptr;

    // Phase 3: install pipe and set metadata under lock.
    bool has_pipe = false;
    {
        frdx::ScopedLock lk{mu_};
        // Only install if we're still on the same station (not changed during launch).
        if (active_station_id_ == station_id && active_stream_id_ == stream_id) {
            const auto metadata_epoch = reset_metadata_sync_locked();
            if (new_pipe) {
                new_pipe->metadata_epoch = metadata_epoch;
                pipe_.store(std::shared_ptr<Pipe>(std::move(new_pipe)),
                            std::memory_order_release);
                has_pipe = true;
            }

            if (has_static) {
                info_ = static_info;
                metadata_epoch_has_track_ = true;
            } else {
                info_        = TrackInfo{};
                info_.title  = station_name;
                info_.artist = stream_name;
            }
            info_.artwork_url = artwork_url;
            info_gen_.fetch_add(1, std::memory_order_release);
            display_name_ = station_name;
            display_name_gen_.fetch_add(1, std::memory_order_release);
            position_ms_.store(0, std::memory_order_release);
            state_.store(PlaybackState::buffering, std::memory_order_release);
            station_selected_ms_ = steady_ms();
        }
    }

    // Start the appropriate new poller.
    restart_metadata_for_current();

    if (has_pipe) {
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

void ShoutcastSource::start_pipe_locked() {
    stop_pipe_locked();
    // Always reset display state so stale metadata doesn't persist when
    // returning to the station list or selecting a new station.
    info_         = TrackInfo{};
    info_.artist  = "Select a station";
    const auto metadata_epoch = reset_metadata_sync_locked();
    info_gen_.fetch_add(1, std::memory_order_release);
    display_name_ = "Online Radio";
    display_name_gen_.fetch_add(1, std::memory_order_release);
    if (target_url_.empty()) return;

    // Delegate the heavy pipe creation to the shared static helper.
    auto pipe_result = launch_ffmpeg_pipe(target_url_, cfg_.ffmpeg_path,
                                           volume_norm_.load(std::memory_order_acquire));
    if (!pipe_result) return;

    (*pipe_result)->metadata_epoch = metadata_epoch;
    pipe_.store(std::shared_ptr<Pipe>(std::move(*pipe_result)),
                std::memory_order_release);

    // Set initial display metadata from the active station before real
    // track info arrives from the background poller / ICY parser.
    restore_station_display_locked();
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);

    log::info("[shoutcast] pipe started for {}", target_url_);
}

void ShoutcastSource::stop_pipe_locked() {
    pipe_.store(nullptr, std::memory_order_release);
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void ShoutcastSource::restore_station_display_locked() {
    if (active_station_id_.empty()) return;
    for (const auto& st : stations_) {
        if (st.id != active_station_id_) continue;
        info_.title   = st.name;
        display_name_ = st.name;
        display_name_gen_.fetch_add(1, std::memory_order_release);
        info_.artist.clear();
        info_.artwork_url.clear();
        for (const auto& str : st.streams) {
            if (str.id == active_stream_id_) {
                info_.artist = str.name;
                info_.artwork_url = str.artwork_url;
                break;
            }
        }
        clear_pending_metadata_locked();
        info_gen_.fetch_add(1, std::memory_order_release);
        return;
    }
}

void ShoutcastSource::play() {
    bool has_pipe = false;
    {
        frdx::ScopedLock lk{mu_};
        drain_pending_ = true;
        if (!pipe_.load(std::memory_order_acquire)) start_pipe_locked();
        restore_station_display_locked();
        has_pipe = (pipe_.load(std::memory_order_acquire) != nullptr);
    } // mu_ released before poller restart to avoid deadlock
    // Restart the metadata poller — pause() stops it, so unpausing needs to
    // bring it back to get live track info again.
    if (has_pipe) {
        restart_metadata_for_current();
        state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

void ShoutcastSource::pause() {
    metadata_controller_->stop();
    {
        frdx::ScopedLock lk{mu_};
        stop_pipe_locked();
        info_         = TrackInfo{};
        info_.artist  = "Nothing playing";
        reset_metadata_sync_locked();
        info_gen_.fetch_add(1, std::memory_order_release);
        if (active_station_id_.empty()) {
            display_name_ = "Online Radio";
            display_name_gen_.fetch_add(1, std::memory_order_release);
        }
    }
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void ShoutcastSource::on_radio_audible(bool audible) {
    frdx::ScopedLock lk{mu_};
    if (audible == audible_) return;
    audible_ = audible;
    if (audible) {
        // Radio became audible again (exited pause menu, etc).
        // Reconnect to the live stream and drain stale buffered audio.
        log::info("[shoutcast] radio audible -- reconnecting to live stream");
        drain_pending_ = true;
        start_pipe_locked();
        restart_metadata_for_current();
    } else {
        // Radio became inaudible (pause menu, rewind, etc).
        // Kill the ffmpeg pipe to stop buffering — otherwise the live
        // stream keeps filling the ring buffer while paused, and when
        // resumed it plays from the stale buffer position, causing the
        // audio to drift behind the live feed and metadata to desync.
        log::info("[shoutcast] radio inaudible -- disconnecting stream");
        stop_pipe_locked();
    }
}

void ShoutcastSource::cycle_and_restart(int direction) {
    metadata_controller_->stop();

    bool has_pipe = false;
    {
        frdx::ScopedLock lk{mu_};
        cycle_preset(direction);
        if (audible_) {
            start_pipe_locked();
        }
        // Always clear pending timed metadata on station swap — the
        // previous station's pending track info is stale and should
        // never be applied to the new station.
        clear_pending_metadata_locked();
        metadata_epoch_has_track_ = false;
        // Handle static metadata from new station + artwork URL.
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                info_.artwork_url.clear();
                for (const auto& str : st.streams) {
                    if (str.id == active_stream_id_) {
                        info_.artwork_url = str.artwork_url;
                        break;
                    }
                }
                if (st.metadata.type == "static") {
                    info_.title =
                        st.metadata.static_title.empty() ? st.name : st.metadata.static_title;
                    info_.artist = st.metadata.static_artist;
                    metadata_epoch_has_track_ = true;
                    info_gen_.fetch_add(1, std::memory_order_release);
                }
                // Station logo fallback for any metadata type.
                if (info_.artwork_url.empty())
                    info_.artwork_url = st.metadata.static_artwork_url;
                break;
            }
        }
        has_pipe = (pipe_.load(std::memory_order_acquire) != nullptr);
        station_selected_ms_ = steady_ms();
    } // mu_ released before poller stop/start
    restart_metadata_for_current();
    if (has_pipe) {
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

    // Station logo fallback: if the poller didn't provide artwork, use
    // the station's cover_url (static_artwork_url) so the texture
    // injector still has something to display.
    //
    // If prefer_itunes_lookup is set, ignore the poller-provided artwork
    // URL and use iTunes/MusicBrainz instead (some stations provide a
    // station logo as artwork rather than per-track art).
    bool prefer_itunes = false;
    if (!active_station_id_.empty()) {
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                prefer_itunes = st.metadata.prefer_itunes_lookup;
                break;
            }
        }
    }
    // Detect sentinel/null artwork URLs that some stations return when
    // there's no cover (e.g. listen.moe returns "https://cdn.listen.moe/covers/null").
    // Treat these as empty so the iTunes/MusicBrainz fallback can kick in.
    if (!info_.artwork_url.empty()) {
        const auto& url = info_.artwork_url;
        // Check for "/null" at end of path, "null" filename, or "null.jpg" etc.
        auto ends_with_null = [](std::string_view u) {
            // Strip query string
            auto qpos = u.find('?');
            if (qpos != std::string_view::npos) u = u.substr(0, qpos);
            // Check last path segment
            auto slash = u.rfind('/');
            std::string_view last = (slash != std::string_view::npos) ? u.substr(slash + 1) : u;
            // Remove extension
            auto dot = last.rfind('.');
            if (dot != std::string_view::npos) last = last.substr(0, dot);
            return last == "null" || last == "none" || last == "default" || last.empty();
        };
        if (ends_with_null(url)) {
            log::info("[shoutcast] ignoring null/sentinel artwork URL: {}", url);
            info_.artwork_url.clear();
        }
    }
    const bool artwork_lookup_on = artwork_lookup_enabled_.load(std::memory_order_acquire);
    bool needs_itunes_lookup = (info_.artwork_url.empty() || prefer_itunes) && artwork_lookup_on;
    if (prefer_itunes && artwork_lookup_on && !info_.artwork_url.empty()) {
        info_.artwork_url.clear();
    }
    if (info_.artwork_url.empty() && !active_station_id_.empty()) {
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                info_.artwork_url = st.metadata.static_artwork_url;
                break;
            }
        }
    }

    info_gen_.fetch_add(1, std::memory_order_release);
    metadata_epoch_has_track_ = true;
    log::info("[shoutcast] applied timed metadata after {}: {} - {}", reason, info_.artist,
              info_.title);

    // If no per-track artwork was provided by the poller, kick off an
    // async artwork lookup.  iTunes Search is tried first (fast, no rate
    // limit); if it misses, MusicBrainz + Cover Art Archive is tried as a
    // 2nd tier (catches obscure/doujin tracks iTunes misses, but rate-
    // limited to 1 req/sec).  The lookup thread checks itunes_lookup_job_
    // to bail out if the track has already changed.
    if (needs_itunes_lookup && !info_.title.empty() && !info_.artist.empty()) {
        const uint64_t job = ++itunes_lookup_job_;
        std::string artist = info_.artist;
        std::string title  = info_.title;
        void* session      = winhttp_session_;
        std::thread([this, job, artist = std::move(artist),
                     title = std::move(title), session]() {
            // Tier 1: iTunes Search API
            std::string art = itunes_lookup_artwork(artist, title, session);
            std::string source = "itunes";
            // Tier 2: MusicBrainz + Cover Art Archive
            if (art.empty()) {
                if (itunes_lookup_job_.load(std::memory_order_acquire) != job) return;
                art = musicbrainz_lookup_artwork(artist, title, session);
                source = "musicbrainz";
            }
            if (art.empty()) return;
            // Stale check: bail if a newer track has been applied.
            if (itunes_lookup_job_.load(std::memory_order_acquire) != job) return;
            frdx::ScopedLock lk{mu_};
            // Bail if the track changed while we were looking up.
            if (itunes_lookup_job_.load(std::memory_order_acquire) != job) return;
            if (info_.artist != artist || info_.title != title) return;
            info_.artwork_url = std::move(art);
            info_gen_.fetch_add(1, std::memory_order_release);
            log::info("[shoutcast] {} artwork found: {} - {}", source, artist, title);
        }).detach();
    }
}

void ShoutcastSource::clear_pending_metadata_locked() {
    pending_info_ = {};
    pending_info_ms_ = 0;
    quiet_started_ms_ = 0;
    pending_info_epoch_ = 0;
    pending_info_valid_ = false;
}

uint64_t ShoutcastSource::reset_metadata_sync_locked() {
    clear_pending_metadata_locked();
    metadata_epoch_has_track_ = false;
    metadata_controller_->notify_icy_metadata(0);
    metadata_controller_->notify_ffmpeg_icy_metadata(0);
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
    constexpr int64_t kQuietHoldMs = 600;       // 600ms of silence → apply
    // Fallback delay: if no quiet gap is found within this window after
    // metadata was queued, apply it anyway.  Covers ffmpeg's buffer latency.
    constexpr int64_t kFallbackDelayMs = 5000;
    constexpr int64_t kMaxPendingMs = 25'000;
    // Grace period after station selection: hold the station logo + name
    // on-screen for at least this long before the first track metadata
    // overwrites it, so the user can see what station they tuned into.
    constexpr int64_t kStationGracePeriodMs = 3500;

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
    for (const auto& st : stations_) {
        if (st.id == active_station_id_) { station = &st; break; }
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

    // Lock-free acquire of the current pipe.  Shared ownership ensures the Pipe
    // stays alive for the entire call, even if a concurrent stop_pipe_locked()
    // replaces pipe_ with nullptr.
    auto p          = pipe_.load(std::memory_order_acquire);
    if (!p) return;
    HANDLE stderr_pipe = p->stderr_pipe;
    HANDLE read_pipe   = p->read_pipe;
    bool ended         = p->ended;

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
        return r;
    };

    // ---- Stderr drain (ffmpeg logs + same-connection ICY metadata) ----
    // ffmpeg emits ICY StreamTitle lines from the exact stream being decoded.
    // Those are better synchronized than our fallback direct ICY connection,
    // which can run ahead of buffered audio on some stations.
    if (stderr_pipe) {
        for (int safety = 0; safety < 3; ++safety) {
            DWORD eavail = 0;
            if (!PeekNamedPipe(stderr_pipe, nullptr, 0, nullptr, &eavail, nullptr)) {
                CloseHandle(stderr_pipe);
                p->stderr_pipe = nullptr;
                break;
            }
            if (eavail == 0) {
                break;
            }
            char ebuf[512];
            DWORD egot = 0;
            if (!ReadFile(stderr_pipe, ebuf, sizeof(ebuf) - 1, &egot, nullptr) || egot == 0) {
                CloseHandle(stderr_pipe);
                p->stderr_pipe = nullptr;
                break;
            }
            p->stderr_buf.append(ebuf, egot);
            if (p->stderr_buf.size() > 65536)
                p->stderr_buf.erase(0, p->stderr_buf.size() - 32768);

            std::size_t consumed = 0;
            for (;;) {
                const auto nl = p->stderr_buf.find('\n', consumed);
                if (nl == std::string::npos) break;

                std::string_view line{p->stderr_buf.data() + consumed, nl - consumed};
                consumed = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.remove_suffix(1);
                if (line.empty()) continue;

                if (auto title = extract_stream_title(line)) {
                    const auto dash = title->find(" - ");
                    std::string logged_artist;
                    std::string logged_title;
                    const auto now_ms = steady_ms();
                    bool accepted = false;
                    {
                        frdx::ScopedLock lk{mu_};
                        TrackInfo queued;
                        if (dash != std::string::npos) {
                            queued.artist = title->substr(0, dash);
                            queued.title  = title->substr(dash + 3);
                        } else {
                            queued.artist = display_name_;
                            queued.title  = std::move(*title);
                        }
                        logged_artist = queued.artist;
                        logged_title  = queued.title;
                        accepted =
                            queue_timed_metadata_locked(std::move(queued), now_ms, p->metadata_epoch);
                    }
                    if (accepted) {
                        metadata_controller_->notify_ffmpeg_icy_metadata(now_ms);
                        metadata_controller_->notify_icy_metadata(now_ms);
                        log::info("[shoutcast] ffmpeg icy metadata queued: {} - {}", logged_artist,
                                  logged_title);
                    }
                } else if (line.find("StreamTitle") == std::string_view::npos) {
                    log::warn("[shoutcast] ffmpeg: {}", line);
                }
            }
            if (consumed > 0) p->stderr_buf.erase(0, consumed);
        }
    }

    // If ffmpeg has exited and the ring is drained, try reconnecting.
    if (ended) {
        if (update_position() == 0) {
            frdx::UniqueLock lk{mu_};
            int attempts = reconnect_attempts_.load(std::memory_order_acquire);
            if (attempts < 3) {
                reconnect_attempts_.store(attempts + 1, std::memory_order_release);
                log::info("[shoutcast] stream disconnected; attempting reconnect {}/3",
                          attempts + 1);
                info_.title  = display_name_;
                info_.artist = "Attempting to reconnect to station";
                clear_pending_metadata_locked();
                info_gen_.fetch_add(1, std::memory_order_release);
            } else {
                reconnect_attempts_.store(0, std::memory_order_release);
                log::warn("[shoutcast] max reconnect attempts reached; cycling presets");
                if (!cfg_.presets.empty() || !stations_.empty()) {
                    info_.title  = display_name_;
                    info_.artist = "Unable to reach this station";
                    clear_pending_metadata_locked();
                    info_gen_.fetch_add(1, std::memory_order_release);
                    cycle_preset();
                }
            }
            // Save before start_pipe_locked resets it to "Online Radio".
            std::string prev_name = display_name_;
            start_pipe_locked();
            bool has_pipe = (pipe_.load(std::memory_order_acquire) != nullptr);
            if (!has_pipe) {
                // Pipe launch failed — restore unreachable status.
                info_.title   = prev_name;
                info_.artist  = "Unable to reach this station";
                clear_pending_metadata_locked();
                info_gen_.fetch_add(1, std::memory_order_release);
            }
            lk.unlock();
            // Deliberately NOT calling restart_metadata_for_current() here —
            // that does a blocking HTTP GET which would stall the audio thread.
            // The sync poll already ran during select_station()/next()/previous().
            // ICY metadata from ffmpeg will fill in track info as the stream plays.
            if (has_pipe) {
                state_.store(PlaybackState::playing, std::memory_order_release);
            }
        }
        return;
    }

    if (!read_pipe) return;

    auto on_eof = [&] {
        if (p->bytes_written == 0) {
            log::warn("[shoutcast] stream produced 0 bytes; stopping");
            frdx::UniqueLock lk{mu_};
            stop_pipe_locked();
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
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
        if (p->stderr_pipe) {
            CloseHandle(p->stderr_pipe);
            p->stderr_pipe = nullptr;
        }
    };

    DWORD avail = 0;
    if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }
    int16_t buf[2048]; // 1024 stereo frames * 2 channels = 4096 bytes
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        DWORD got = 0;
        if (!ReadFile(read_pipe, buf, static_cast<DWORD>(want), &got, nullptr) || got == 0) {
            on_eof();
            return;
        }
        if (got > 0) {
            reconnect_attempts_.store(0, std::memory_order_relaxed);
        }
        ring.write(reinterpret_cast<std::byte*>(buf), got);
        observe_pcm_for_metadata_sync(buf, got / sizeof(int16_t), p->metadata_epoch);
        p->bytes_written += got;
        avail = avail > got ? avail - got : 0;
    }
    if (update_position() > 32 * 1024) {
        auto expected = PlaybackState::buffering;
        (void)state_.compare_exchange_strong(expected, PlaybackState::playing,
                                             std::memory_order_release,
                                             std::memory_order_relaxed);
    }
}

} // namespace frdx::sources
