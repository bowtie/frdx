#include "frdx/sources/shoutcast_source.hpp"
#include "frdx/log.hpp"
#include "frdx/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// The Clang thread-safety analyzer doesn't understand std::jthread boundaries
// and traces lock state from restart_metadata_for_current() (which acquires mu_)
// through the jthread lambda into run_icy_poller(), producing 14 false-positive
// warnings.  Neither [[clang::no_thread_safety_analysis]] nor #pragma clang
// diagnostic wrapping the function definitions suppresses the CFG-based
// analysis when lock state is inherited via inter-procedural inlining.
// File-level suppression is the only reliable mechanism.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wthread-safety"
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
#endif

namespace frdx::sources {

using json = nlohmann::json;

namespace {

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

// Resolve a JSON path to a const json* node (returns nullptr if not found).
// Supports both object traversal and array indexing.
const json* resolve_json_node(const json& root, const std::string& dot_path) {
    if (dot_path.empty()) return &root;
    const json* node = &root;
    std::size_t pos  = 0;
    while (pos < dot_path.size()) {
        auto dot = dot_path.find('.', pos);
        auto key =
            (dot == std::string::npos) ? dot_path.substr(pos) : dot_path.substr(pos, dot - pos);
        if (key.empty()) return nullptr;
        if (node->is_array()) {
            try {
                auto idx = static_cast<std::size_t>(std::stoul(key));
                if (idx >= node->size()) return nullptr;
                node = &(*node)[idx];
            } catch (...) {
                return nullptr;
            }
        } else if (node->is_object()) {
            auto it = node->find(key);
            if (it == node->end()) return nullptr;
            node = &*it;
        } else {
            return nullptr;
        }
        pos = (dot == std::string::npos) ? dot_path.size() : dot + 1;
    }
    return node;
}

// Resolve a dot-notation JSON path (e.g. "data.track_title") to a value in a nlohmann::json tree.
// Returns the value if found, otherwise an empty string.
std::string resolve_json_path(const json& root, const std::string& dot_path) {
    if (dot_path.empty()) return {};
    const json* node = resolve_json_node(root, dot_path);
    if (!node) return {};
    if (node->is_string()) return node->get<std::string>();
    if (node->is_number_integer()) return std::to_string(node->get<int64_t>());
    if (node->is_number_float()) return std::to_string(node->get<double>());
    return node->dump();
}

// Apply enhanced mapping to resolve a single field from JSON.
// Handles: dot-path resolution, artist_array_field (join array of objects),
// and artist_separator.
std::string resolve_mapped_value(const json& root, const std::string& path,
                                 const std::string& array_field, const std::string& separator) {
    if (path.empty()) return {};

    // If array_field is set, resolve to the node and extract array elements.
    if (!array_field.empty()) {
        const json* node = resolve_json_node(root, path);
        if (node && node->is_array()) {
            std::string result;
            for (const auto& elem : *node) {
                std::string val;
                if (elem.is_object()) {
                    auto it = elem.find(array_field);
                    if (it != elem.end() && it->is_string()) val = it->get<std::string>();
                } else if (elem.is_string()) {
                    val = elem.get<std::string>();
                }
                if (!val.empty()) {
                    if (!result.empty()) result += separator;
                    result += val;
                }
            }
            return result;
        }
    }

    // Standard path resolution.
    return resolve_json_path(root, path);
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
            for (const auto& s : j["streams"]) {
                StationStream ss;
                ss.id   = s.value("id", "");
                ss.name = s.value("name", "");
                ss.url  = s.value("url", "");
                // Per-stream mapping override.
                if (s.contains("mapping")) {
                    const auto& mp                         = s["mapping"];
                    ss.mapping_override.title              = mp.value("title", "");
                    ss.mapping_override.artist             = mp.value("artist", "");
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
                sp.metadata.mapping.artist_array_field = mp.value("artist_array_field", "");
                sp.metadata.mapping.artist_separator   = mp.value("artist_separator", ", ");
                sp.metadata.mapping.split_separator    = mp.value("split_separator", "");
            }
            if (m.contains("stream_url_map") && m["stream_url_map"].is_object()) {
                for (auto it = m["stream_url_map"].begin(); it != m["stream_url_map"].end(); ++it)
                    sp.metadata.stream_url_map[it.key()] = it.value().get<std::string>();
            }
            // Static metadata (type == "static").
            sp.metadata.static_title  = m.value("title", "");
            sp.metadata.static_artist = m.value("artist", "");
            // WebSocket metadata (type == "websocket").
            if (m.contains("message_filter")) {
                const auto& mf                   = m["message_filter"];
                sp.metadata.ws_filter.op_field   = mf.value("op_field", "");
                sp.metadata.ws_filter.op_value   = mf.value("op_value", 0);
                sp.metadata.ws_filter.type_field = mf.value("type_field", "");
                sp.metadata.ws_filter.type_value = mf.value("type_value", "");
                sp.metadata.ws_filter.data_field = mf.value("data_field", "");
            }
            if (m.contains("heartbeat")) {
                const auto& hb                          = m["heartbeat"];
                sp.metadata.ws_heartbeat.message        = hb.value("message", "");
                sp.metadata.ws_heartbeat.interval_field = hb.value("interval_field", "");
                sp.metadata.ws_heartbeat.default_interval_ms =
                    hb.value("default_interval_ms", 45000);
            }
        }
        return sp;
    };

    if (root.is_array()) {
        for (const auto& j : root) out.push_back(parse_station(j));
    } else {
        out.push_back(parse_station(root));
    }

    log::info("[shoutcast] loaded {} station(s) from {}", out.size(), path.string());
    return out;
}

// RAII wrapper for HINTERNET handles — auto-closes via WinHttpCloseHandle.
class ScopedWinHttp {
public:
    ScopedWinHttp() = default;
    explicit ScopedWinHttp(HINTERNET h) : h_(h) {}
    ~ScopedWinHttp() { close(); }

    ScopedWinHttp(const ScopedWinHttp&) = delete;
    ScopedWinHttp& operator=(const ScopedWinHttp&) = delete;
    ScopedWinHttp(ScopedWinHttp&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    ScopedWinHttp& operator=(ScopedWinHttp&& o) noexcept {
        if (this != &o) { close(); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }

    void close() {
        if (h_) { WinHttpCloseHandle(h_); h_ = nullptr; }
    }
    HINTERNET get() const { return h_; }
    HINTERNET release() { HINTERNET h = h_; h_ = nullptr; return h; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

// Parsed WinHTTP URL components — handles ws:///wss:// → http/https substitution.
struct ParsedUrl {
    std::wstring wurl;
    URL_COMPONENTS uc{};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    bool is_ssl = false;
    INTERNET_PORT port = 80;

    static std::optional<ParsedUrl> parse(const std::string& url) {
        ParsedUrl p;
        std::string tmp = url;
        auto ws_pos      = tmp.find("://");
        if (ws_pos != std::string::npos) {
            std::string scheme = tmp.substr(0, ws_pos);
            if (scheme == "wss")
                tmp.replace(0, 3, "https");
            else if (scheme == "ws")
                tmp.replace(0, 2, "http");
        }
        p.wurl             = frdx::subprocess::widen(tmp);
        p.uc.dwStructSize  = sizeof(p.uc);
        p.uc.lpszHostName  = p.host;
        p.uc.dwHostNameLength = 255;
        p.uc.lpszUrlPath      = p.path;
        p.uc.dwUrlPathLength  = 2047;
        if (!WinHttpCrackUrl(p.wurl.c_str(), 0, 0, &p.uc)) return std::nullopt;
        p.is_ssl = (p.uc.nScheme == INTERNET_SCHEME_HTTPS);
        p.port   = p.uc.nPort ? static_cast<INTERNET_PORT>(p.uc.nPort)
                              : (p.is_ssl ? 443 : 80);
        return p;
    }
};

// Shared WinHTTP connection setup — RAII-wrapped connect + request.
struct WinHttpConn {
    ScopedWinHttp request;
    ScopedWinHttp connect;
    ScopedWinHttp localSession;
    explicit operator bool() const { return request.get() != nullptr; }
};

WinHttpConn winhttp_connect(void* shared_session, const ParsedUrl& parsed) {
    WinHttpConn conn;
    HINTERNET hSession =
        shared_session ? static_cast<HINTERNET>(shared_session) : nullptr;

    if (!hSession) {
        hSession = WinHttpOpen(L"forza-radio-dx/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            log::warn("[shoutcast] WinHttpOpen failed ({})", GetLastError());
            return conn;
        }
        conn.localSession = ScopedWinHttp(hSession);
        DWORD to_ms = 5000;
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &to_ms, sizeof(to_ms));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &to_ms, sizeof(to_ms));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to_ms, sizeof(to_ms));
    }

    conn.connect = ScopedWinHttp(
        WinHttpConnect(hSession, parsed.host, parsed.port, 0));
    if (!conn.connect) {
        log::warn("[shoutcast] WinHttpConnect failed ({})", GetLastError());
        return conn;
    }

    conn.request = ScopedWinHttp(WinHttpOpenRequest(
        conn.connect.get(), L"GET", parsed.path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.is_ssl ? WINHTTP_FLAG_SECURE : 0));
    if (!conn.request) {
        log::warn("[shoutcast] WinHttpOpenRequest failed ({})", GetLastError());
        return conn;
    }

    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(conn.request.get(), WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));

    return conn;
}

} // namespace

// Replace CJK brackets that don't render in-game with ASCII [].
// U+300C/U+300D (「」) corner brackets, U+300A/U+300B (《》) double angle,
// U+3010/U+3011 (【】) square brackets — each is 3 bytes in UTF-8.
// Corner brackets: \xE3\x80\x8C / \xE3\x80\x8D
// Double angle:    \xE3\x80\x8A / \xE3\x80\x8B
// Square brackets: \xE3\x80\x90 / \xE3\x80\x91
std::string sanitize_brackets(std::string s) {
    // Opening brackets → [, closing brackets → ]
    static constexpr struct {
        const char* bytes;
        char replace;
    } kReplace[] = {
        {"\xE3\x80\x8C", '['}, // 「
        {"\xE3\x80\x8D", ']'}, // 」
        {"\xE3\x80\x8A", '['}, // 《
        {"\xE3\x80\x8B", ']'}, // 》
        {"\xE3\x80\x90", '['}, // 【
        {"\xE3\x80\x91", ']'}, // 】
    };
    for (const auto& rep : kReplace) {
        for (auto pos = s.find(rep.bytes); pos != std::string::npos;
             pos      = s.find(rep.bytes, pos + 1))
            s.replace(pos, 3, 1, rep.replace);
    }
    return s;
}

// ---- Pipe ----

struct ShoutcastSource::Pipe {
    HANDLE job         = nullptr;
    HANDLE proc_ff     = nullptr;
    HANDLE read_pipe   = nullptr;
    HANDLE stderr_pipe = nullptr;
    std::string stderr_buf;
    std::uint64_t bytes_written = 0;
    bool ended                  = false;

    ~Pipe() {
        if (read_pipe) CloseHandle(read_pipe);
        if (stderr_pipe) CloseHandle(stderr_pipe);
        if (proc_ff) CloseHandle(proc_ff);
        if (job) CloseHandle(job);
    }
};

// ---- ShoutcastSource ----

ShoutcastSource::ShoutcastSource(ShoutcastConfig cfg) : cfg_{std::move(cfg)} {
    info_        = TrackInfo{};
    info_.artist = "Select a station";
    info_gen_.fetch_add(1, std::memory_order_release);
}

ShoutcastSource::~ShoutcastSource() {
    stop_icy_poller();
    stop_metadata_poller();
    stop_pipe_locked();
}

bool ShoutcastSource::initialize() {
    if (!cfg_.enabled) return false;
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
    } else {
        log::warn("[shoutcast] failed to initialize shared WinHTTP session; falling back to local "
                  "sessions");
    }

    return true;
}

void ShoutcastSource::shutdown() noexcept {
    stop_icy_poller();
    stop_metadata_poller();
    {
        frdx::ScopedLock lk{mu_};
        stop_pipe_locked();
    }
    if (winhttp_session_) {
        ScopedWinHttp(static_cast<HINTERNET>(winhttp_session_));
        winhttp_session_ = nullptr;
    }
}

void ShoutcastSource::load_stations(const std::filesystem::path& path) {
    auto loaded = parse_stations_json(path);
    frdx::ScopedLock lk{mu_};
    stations_ = std::move(loaded);
    // Rebuild flat preset cache for cycle_preset().
    flat_presets_.clear();
    for (std::size_t i = 0; i < stations_.size(); ++i)
        for (std::size_t j = 0; j < stations_[i].streams.size(); ++j)
            flat_presets_.emplace_back(i, j);
    log::info("[shoutcast] {} station(s) available", stations_.size());
}

// ---- ffmpeg subprocess launcher (no mu_ held) ----

std::expected<std::unique_ptr<ShoutcastSource::Pipe>, ShoutcastError>
ShoutcastSource::launch_ffmpeg_pipe(const std::string& url,
                                    const std::filesystem::path& ffmpeg_path) {
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
                          L" -loglevel error"
                          L" -icy 1"
                          L" -user_agent \"WinampMPEG/5.0\""
                          L" -reconnect 1"
                          L" -reconnect_streamed 1"
                          L" -reconnect_delay_max 5"
                          L" -i " +
                          frdx::subprocess::quote(frdx::subprocess::widen(url)) +
                          L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

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
    // Phase 1: configure under lock — fast path, no ffmpeg launch.
    std::string launch_url;
    std::string md_type;
    std::string station_name;
    std::string stream_name;
    std::string md_url;
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

        // Resolve the metadata URL / type for this stream.
        metadata_url_.clear();
        md_type = station->metadata.type;
        if (md_type == "websocket") {
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

        // Signal the old metadata thread to stop before restarting the
        // pipe, so even if it's about to write a stale poll result to
        // info_ it sees the flag and skips the write.
        // (stop_metadata_poller() will request_stop on the jthread)

        // Stop old pipe (fast — no ffmpeg launch here).
        stop_pipe_locked();
    } // mu_ released — ffmpeg launch happens outside the lock

    // Phase 2: launch ffmpeg OUTSIDE the lock (expensive subprocess creation).
    auto new_pipe_result = launch_url.empty()
                               ? std::expected<std::unique_ptr<Pipe>, ShoutcastError>{nullptr}
                               : launch_ffmpeg_pipe(launch_url, cfg_.ffmpeg_path);
    auto new_pipe = new_pipe_result ? std::move(*new_pipe_result) : nullptr;

    // Phase 3: install pipe and set metadata under lock.
    bool has_pipe = false;
    {
        frdx::ScopedLock lk{mu_};
        // Only install if we're still on the same station (not changed during launch).
        if (active_station_id_ == station_id && active_stream_id_ == stream_id) {
            if (new_pipe) {
                pipe_    = std::move(new_pipe);
                has_pipe = true;
            }

            if (has_static) {
                info_ = static_info;
            } else {
                info_        = TrackInfo{};
                info_.title  = station_name;
                info_.artist = stream_name;
            }
            info_gen_.fetch_add(1, std::memory_order_release);
            display_name_ = station_name;
            position_ms_.store(0, std::memory_order_release);
            state_.store(PlaybackState::buffering, std::memory_order_release);
        }
    }

    // Stop old poller and start the appropriate new one.
    stop_metadata_poller();
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
        if (new_st.metadata.type == "websocket") {
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
    info_gen_.fetch_add(1, std::memory_order_release);
    display_name_ = "Online Radio";
    if (target_url_.empty()) return;

    // Delegate the heavy pipe creation to the shared static helper.
    auto pipe_result = launch_ffmpeg_pipe(target_url_, cfg_.ffmpeg_path);
    if (!pipe_result) return;

    pipe_ = std::move(*pipe_result);

    // Set initial display metadata from the active station before real
    // track info arrives from the background poller / ICY parser.
    if (!active_station_id_.empty()) {
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                info_.title   = st.name;
                display_name_ = st.name;
                for (const auto& str : st.streams) {
                    if (str.id == active_stream_id_) {
                        info_.artist = str.name;
                        break;
                    }
                }
                info_gen_.fetch_add(1, std::memory_order_release);
                break;
            }
        }
    }
    position_ms_.store(0, std::memory_order_release);
    state_.store(PlaybackState::buffering, std::memory_order_release);

    log::info("[shoutcast] pipe started for {}", target_url_);
}

void ShoutcastSource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void ShoutcastSource::play() {
    bool has_pipe = false;
    {
        frdx::ScopedLock lk{mu_};
        if (!pipe_) start_pipe_locked();
        has_pipe = (pipe_ != nullptr);
    } // mu_ released before poller restart to avoid deadlock
    // Restart the metadata poller — pause() stops it, so unpausing needs to
    // bring it back to get live track info again.
    if (has_pipe) {
        restart_metadata_for_current();
        state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

void ShoutcastSource::pause() {
    stop_icy_poller();
    stop_metadata_poller();
    {
        frdx::ScopedLock lk{mu_};
        stop_pipe_locked();
        info_         = TrackInfo{};
        info_.artist  = "Nothing playing";
        info_gen_.fetch_add(1, std::memory_order_release);
        display_name_ = "Online Radio";
    }
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void ShoutcastSource::cycle_and_restart(int direction) {
    bool has_pipe = false;
    {
        frdx::ScopedLock lk{mu_};
        cycle_preset(direction);
        start_pipe_locked();
        // Handle static metadata from new station.
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                if (st.metadata.type == "static") {
                    info_.title =
                        st.metadata.static_title.empty() ? st.name : st.metadata.static_title;
                    info_.artist = st.metadata.static_artist;
                    info_gen_.fetch_add(1, std::memory_order_release);
                }
                break;
            }
        }
        has_pipe = (pipe_ != nullptr);
    } // mu_ released before poller stop/start
    restart_metadata_for_current();
    if (has_pipe) {
        state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

void ShoutcastSource::next() { cycle_and_restart(1); }

void ShoutcastSource::previous() { cycle_and_restart(-1); }

TrackInfo ShoutcastSource::current_track() const {
    frdx::ScopedLock lk{mu_};
    TrackInfo t   = info_;
    t.position_ms = position_ms_.load(std::memory_order_acquire);
    return t;
}

// ---- Metadata poller (dispatcher) ----

void ShoutcastSource::restart_metadata_for_current() {
    // Spawns background metadata threads for the currently active station:
    //   - ICY poller: direct HTTP connection to the stream URL parsing
    //     interleaved StreamTitle blocks (frame-synced). Runs for any
    //     station that isn't WebSocket or static.
    //   - WebSocket poller: persistent connection with live push updates.
    //
    // Called from select_station(), next(), and previous().
    // All captured state is by value — no dangling references.

    // 1. Stop any existing ICY poller before checking state.
    stop_icy_poller();

    frdx::UniqueLock lk{mu_};

    // ---- Legacy preset mode (no station selected, raw URL only) ----
    if (active_station_id_.empty()) {
        std::string legacy_url = target_url_;
        lk.unlock();
        if (!legacy_url.empty()) {
            icy_poller_thread_ = std::jthread(
                [this, url = std::move(legacy_url)](const std::stop_token& stop) {
                    run_icy_poller(url, stop);
                });
        }
        return;
    }

    const StationPreset* station = nullptr;
    for (const auto& st : stations_) {
        if (st.id == active_station_id_) {
            station = &st;
            break;
        }
    }
    if (!station) return;

    const StationStream* stream = nullptr;
    for (const auto& s : station->streams) {
        if (s.id == active_stream_id_) {
            stream = &s;
            break;
        }
    }

    // Copy by value — station/stream pointers dangle after mu_ is released.
    auto mapping = station->metadata.mapping;
    if (stream && (!stream->mapping_override.title.empty() ||
                   !stream->mapping_override.split_separator.empty() ||
                   !stream->mapping_override.artist.empty())) {
        mapping = stream->mapping_override;
        log::info("[shoutcast] using stream-level mapping override for '{}'", stream->id);
    }

    auto url     = metadata_url_;
    auto md_type = station->metadata.type;
    auto ws_filter    = station->metadata.ws_filter;
    auto ws_heartbeat = station->metadata.ws_heartbeat;

    // Capture stream URL for ICY poller.
    std::string stream_url;
    if (stream && !stream->url.empty())
        stream_url = stream->url;
    if (stream_url.empty() && !station->streams.empty())
        stream_url = station->streams[0].url;

    // Reset ICY timestamp so the first WS poller response fills title/artist.
    last_icy_ms_.store(0, std::memory_order_release);

    lk.unlock();

    // 2. Start ICY poller for non-WS, non-static stations.
    if (!stream_url.empty() && md_type != "websocket" && md_type != "static") {
        icy_poller_thread_ = std::jthread(
            [this, url = std::move(stream_url)](const std::stop_token& stop) {
                run_icy_poller(url, stop);
            });
    }

    // 3. Stop any existing background metadata poller before starting a new one.
    stop_metadata_poller();

    // Static or no type -> no background poller needed.
    if (md_type == "static" || md_type.empty()) return;

    // No metadata URL configured -> no WS poller possible.
    if (url.empty()) return;

    // ---- WebSocket metadata poller ----
    if (md_type == "websocket") {
        metadata_thread_ = std::jthread([this, url = std::move(url),
                                         mapping = std::move(mapping),
                                         filter = std::move(ws_filter),
                                         hb = std::move(ws_heartbeat)](
                                            const std::stop_token& stop) {
            run_ws_poller(std::move(url), std::move(mapping), std::move(filter),
                          std::move(hb), stop);
        });
        return;
    }
}

void ShoutcastSource::run_ws_poller(std::string url, StationMetadataMapping mapping,
                                   StationMetadataWebSocketFilter filter,
                                   StationMetadataWebSocketHeartbeat hb,
                                   std::stop_token stop) {
    auto parsed = ParsedUrl::parse(url);
    if (!parsed) {
        log::warn("[shoutcast] ws: failed to parse URL '{}'", url);
        return;
    }
    while (!stop.stop_requested()) {

        auto conn = winhttp_connect(winhttp_session_, *parsed);
        if (!conn) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        HINTERNET hRequest = conn.request.get();

        if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            log::warn("[shoutcast] ws: upgrade failed ({})", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        if (!WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0)) {
            log::warn("[shoutcast] ws: SendRequest failed ({})", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            log::warn("[shoutcast] ws: ReceiveResponse failed ({})", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        if (!hWebSocket) {
            log::warn("[shoutcast] ws: upgrade complete failed ({})", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        // Release the request handle from RAII — it has been upgraded to
        // a WebSocket handle (hWebSocket, closed manually below). The connect
        // handle stays in ScopedWinHttp and auto-closes when conn goes out of
        // scope at the end of this loop iteration.
        (void)conn.request.release();

        {
            frdx::ScopedLock lk{ws_mu_};
            ws_handle_ = hWebSocket;
        }
        log::info("[shoutcast] ws: connected to {}", url);

        // Brief settle so the station name is visible in-game
        // before song metadata overwrites it.
        std::this_thread::sleep_for(std::chrono::seconds(1));

        int heartbeat_ms    = hb.default_interval_ms;
        auto last_heartbeat = std::chrono::steady_clock::now();

        if (!hb.message.empty()) {
            DWORD err = WinHttpWebSocketSend(hWebSocket,
                                             WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                             const_cast<char*>(hb.message.data()),
                                             static_cast<DWORD>(hb.message.size()));
            if (err != ERROR_SUCCESS)
                log::warn("[shoutcast] ws: heartbeat send failed ({})", err);
            last_heartbeat = std::chrono::steady_clock::now();
        }

        // ---- Receive loop ----
        while (!stop.stop_requested()) {
            if (!hb.message.empty()) {
                auto now     = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - last_heartbeat)
                                   .count();
                if (elapsed >= heartbeat_ms) {
                    DWORD err = WinHttpWebSocketSend(
                        hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                        const_cast<char*>(hb.message.data()),
                        static_cast<DWORD>(hb.message.size()));
                    if (err != ERROR_SUCCESS) {
                        log::warn("[shoutcast] ws: heartbeat send failed ({})", err);
                        break;
                    }
                    last_heartbeat = now;
                }
            }

            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;
            char recvBuf[8192];

            DWORD err = WinHttpWebSocketReceive(hWebSocket, recvBuf, sizeof(recvBuf) - 1,
                                                &bytesRead, &bufType);
            if (err != ERROR_SUCCESS) {
                if (err == ERROR_WINHTTP_OPERATION_CANCELLED) break;
                if (err == ERROR_WINHTTP_TIMEOUT) continue;
                log::warn("[shoutcast] ws: receive error ({}), reconnecting...", err);
                break;
            }

            if (bytesRead == 0) continue;
            recvBuf[bytesRead] = '\0';

            json msg;
            try {
                msg = json::parse(recvBuf, recvBuf + bytesRead);
            } catch (...) {
                continue;
            }

            // Update heartbeat interval from any message (including
            // the op:0 welcome message which has d.heartbeat but
            // doesn't pass the track-update filter below). Resolve
            // against raw msg since welcome fields are top-level.
            if (!hb.interval_field.empty()) {
                std::string ival = resolve_json_path(msg, hb.interval_field);
                if (!ival.empty()) {
                    try {
                        int new_interval = std::stoi(ival);
                        if (new_interval > 0) heartbeat_ms = new_interval;
                    } catch (...) {}
                }
            }

            // Apply message filter.
            if (!filter.op_field.empty()) {
                const json* op_node = resolve_json_node(msg, filter.op_field);
                if (!op_node) continue;
                int op_val = op_node->is_number_integer()
                               ? static_cast<int>(op_node->get<int64_t>())
                               : 0;
                if (op_val != filter.op_value) continue;
            }
            if (!filter.type_field.empty() && !filter.type_value.empty()) {
                std::string t = resolve_json_path(msg, filter.type_field);
                if (t != filter.type_value) continue;
            }

            const json* data_root = &msg;
            if (!filter.data_field.empty()) {
                const json* data = resolve_json_node(msg, filter.data_field);
                if (!data) continue;
                data_root = data;
            }

            std::string title = resolve_mapped_value(*data_root, mapping.title, "", "");
            std::string artist =
                resolve_mapped_value(*data_root, mapping.artist, mapping.artist_array_field,
                                     mapping.artist_separator);

            // If ICY metadata has ever been received for this stream
            // (since the last reset in restart_metadata_for_current),
            // only let the WS poller update artwork — ICY provides
            // perfectly-synced title/artist at the audio transition point
            // and never goes stale (the flag is reset on station change).
            auto last_icy   = last_icy_ms_.load(std::memory_order_acquire);
            bool icy_active = (last_icy > 0);

            if (!title.empty() || !artist.empty()) {
                frdx::ScopedLock lk{mu_};
                // Skip write if a new station was selected while
                // we were processing — stop is requested early in
                // select_station() via stop_metadata_poller().
                if (!stop.stop_requested()) {
                    if (!title.empty() && !icy_active)
                        info_.title = sanitize_brackets(std::move(title));
                    if (!artist.empty() && !icy_active)
                        info_.artist = sanitize_brackets(std::move(artist));
                    info_gen_.fetch_add(1, std::memory_order_release);
                    log::info("[shoutcast] ws metadata: {} - {} (icy={})", info_.artist,
                              info_.title, icy_active);
                }
            }
        }

        // Cleanup connection.
        {
            frdx::ScopedLock lk{ws_mu_};
            if (ws_handle_ == hWebSocket) ws_handle_ = nullptr;
        }
        if (!stop.stop_requested()) {
            WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                  nullptr, 0);
        }
        WinHttpCloseHandle(hWebSocket);
        // conn.connect auto-closed by ScopedWinHttp when conn exits scope;
        // conn.localSession likewise auto-closed on scope exit.

        if (stop.stop_requested()) break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void ShoutcastSource::stop_metadata_poller() {
    // Force-close any active WebSocket handle to unblock WinHttpWebSocketReceive.
    // Only send the close frame — do NOT WinHttpCloseHandle here; the thread
    // owns the handle and will close it in its cleanup path.
    {
        frdx::ScopedLock lk{ws_mu_};
        if (ws_handle_) {
            WinHttpWebSocketClose(ws_handle_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            ws_handle_ = nullptr;
        }
    }
    if (metadata_thread_.joinable()) metadata_thread_.request_stop();
    if (metadata_thread_.joinable()) metadata_thread_.join();
}

void ShoutcastSource::stop_icy_poller() {
    if (icy_poller_thread_.joinable()) icy_poller_thread_.request_stop();
    if (icy_poller_thread_.joinable()) icy_poller_thread_.join();
}

// ---- ICY metadata poller ----
//
// Opens a direct HTTP connection to the stream URL with
//   Icy-MetaData: 1
//   User-Agent: WinampMPEG/5.0
//
// After receiving the response, it reads the icy-metaint header from the
// raw response headers, then enters a loop that reads:
//   1. metaint bytes of audio data (discarded)
//   2. 1 byte of metadata length (length = byte * 16, 0 = no metadata)
//   3. metadata bytes (pairs of key='value')
//
// When a StreamTitle is found, info_ is updated under mu_ and last_icy_ms_
// is set so the REST/WS pollers skip overwriting title/artist.
//
// On connection failure or unexpected disconnect, exits after logging;
// the caller (restart_metadata_for_current) will restart the poller on the
// next station change.

void ShoutcastSource::run_icy_poller(const std::string& stream_url,
                                     const std::stop_token& stop) {
    // Brief settle so station/stream names are visible in-game before
    // song metadata overwrites them.
    for (int i = 0; i < 10; ++i) {
        if (stop.stop_requested()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto parsed = ParsedUrl::parse(stream_url);
    if (!parsed) {
        log::warn("[shoutcast] icy: failed to parse URL '{}'", stream_url);
        return;
    }

    while (!stop.stop_requested()) {
        auto conn = winhttp_connect(winhttp_session_, *parsed);
        if (!conn) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        HINTERNET hRequest = conn.request.get();

        // Inject headers via AddRequestHeaders with REPLACE to override any
        // WinHTTP session-level defaults (e.g. User-Agent).
        WinHttpAddRequestHeaders(
            hRequest,
            L"User-Agent: WinampMPEG/5.0\r\nIcy-MetaData: 1\r\n", -1L,
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // ---- Extract icy-metaint via WINHTTP_QUERY_CUSTOM ----
        // WinHTTP's RAW_HEADERS_CRLF parser silently drops non-standard
        // headers like icy-metaint.  Querying by name with _CUSTOM forces
        // WinHTTP to return the exact value.
        wchar_t metaint_buf[64] = {};
        DWORD metaint_len       = sizeof(metaint_buf);
        int metaint             = 0;

        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, L"icy-metaint",
                                metaint_buf, &metaint_len, WINHTTP_NO_HEADER_INDEX)) {
            metaint = static_cast<int>(std::wcstol(metaint_buf, nullptr, 10));
        } else {
            // Fallback: raw header scan for older/broken WinHTTP builds.
            DWORD header_size = 0;
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &header_size,
                                WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && header_size > 0) {
                std::wstring raw_headers(header_size / sizeof(wchar_t) + 1, L'\0');
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX, raw_headers.data(),
                                    &header_size, WINHTTP_NO_HEADER_INDEX);
                std::wstring lowered = raw_headers;
                for (auto& c : lowered)
                    if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
                auto ipos = lowered.find(L"icy-metaint:");
                if (ipos != std::wstring::npos) {
                    auto vs = ipos + 12;
                    while (vs < raw_headers.size() &&
                           (raw_headers[vs] == L' ' || raw_headers[vs] == L'\t'))
                        vs++;
                    auto ve = vs;
                    while (ve < raw_headers.size() && raw_headers[ve] != L'\r' &&
                           raw_headers[ve] != L'\n')
                        ve++;
                    if (ve > vs)
                        metaint = static_cast<int>(
                            std::wcstol(raw_headers.substr(vs, ve - vs).c_str(), nullptr, 10));
                }
            }
        }

        if (metaint <= 0) {
            log::warn("[shoutcast] icy: server did not provide icy-metaint for '{}'",
                      stream_url);
            break; // No ICY support — do not retry
        }

        log::info("[shoutcast] icy: connected, metaint={}", metaint);

        // Buffer: metaint + 1 (metadata length byte) + 4096 (max metadata block)
        static std::vector<char> s_icy_buf;
        std::size_t buf_needed = static_cast<std::size_t>(metaint) + 1 + 4096;
        if (s_icy_buf.size() < buf_needed) s_icy_buf.resize(buf_needed);
        char* buf = s_icy_buf.data();
        bool first_title = true;

        while (!stop.stop_requested()) {
            // Read metaint audio bytes (discard)
            DWORD audio_read = 0;
            bool error       = false;
            while (audio_read < static_cast<DWORD>(metaint) && !stop.stop_requested()) {
                DWORD chunk = 0;
                if (!WinHttpReadData(hRequest, buf + audio_read,
                                     static_cast<DWORD>(metaint) - audio_read, &chunk)) {
                    error = true;
                    break;
                }
                if (chunk == 0) {
                    error = true;
                    break;
                }
                audio_read += chunk;
            }
            if (error || stop.stop_requested()) break;

            // Read metadata length byte
            DWORD meta_len_byte_read = 0;
            if (!WinHttpReadData(hRequest, buf, 1, &meta_len_byte_read) ||
                meta_len_byte_read != 1)
                break;

            int meta_len = static_cast<unsigned char>(buf[0]) * 16;
            if (meta_len <= 0) continue;
            if (meta_len > 4096) meta_len = 4096; // safety clamp

            // Read metadata block
            DWORD meta_read = 0;
            while (meta_read < static_cast<DWORD>(meta_len) && !stop.stop_requested()) {
                DWORD chunk = 0;
                if (!WinHttpReadData(hRequest, buf + meta_read,
                                     static_cast<DWORD>(meta_len) - meta_read, &chunk)) {
                    error = true;
                    break;
                }
                if (chunk == 0) {
                    error = true;
                    break;
                }
                meta_read += chunk;
            }
            if (error) break;

            // Parse StreamTitle from metadata.
            // SHOUTcast format: StreamTitle='value';StreamUrl='url';
            // Embedded apostrophes in the value (e.g. "Pizuya's Cell") are
            // not escaped — search for the ' that is followed by ; to find
            // the real end-of-value delimiter.
            std::string_view meta(buf, meta_read);
            auto st_pos = meta.find("StreamTitle='");
            if (st_pos == std::string_view::npos)
                st_pos = meta.find("StreamTitle=\"");
            if (st_pos == std::string_view::npos) continue;

            auto val_start  = st_pos + 13;
            auto quote_char = meta[st_pos + 12];
            auto val_end    = std::string_view::npos;

            for (auto scan = val_start; scan < meta.size();) {
                auto q = meta.find(quote_char, scan);
                if (q == std::string_view::npos) break;
                auto next = q + 1;
                while (next < meta.size() && meta[next] == ' ') next++;
                if (next < meta.size() && meta[next] == ';') {
                    val_end = q;
                    break;
                }
                scan = q + 1;
            }
            if (val_end == std::string_view::npos || val_end <= val_start) continue;

            std::string val(meta.substr(val_start, val_end - val_start));
            val = sanitize_brackets(std::move(val));

            auto dash = val.find(" - ");
            {
                frdx::ScopedLock lk{mu_};
                if (!stop.stop_requested()) {
                    if (dash != std::string::npos) {
                        info_.artist = val.substr(0, dash);
                        info_.title  = val.substr(dash + 3);
                    } else {
                        info_.artist = display_name_;
                        info_.title  = std::move(val);
                    }
                    info_gen_.fetch_add(1, std::memory_order_release);
                }
            }

            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
            last_icy_ms_.store(now_ms, std::memory_order_release);

            if (first_title) {
                first_title = false;
                log::info("[shoutcast] icy: first StreamTitle received");
            }
        }

        if (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

// ---- spawn_icy_poller ----



// ---- PCM pump ----

void ShoutcastSource::pump(RingBuffer& ring) {
    {
        auto st = state_.load(std::memory_order_acquire);
        if (st != PlaybackState::playing && st != PlaybackState::buffering) return;
    }

    Pipe* p = nullptr;
    HANDLE stderr_pipe = nullptr;
    HANDLE read_pipe = nullptr;
    bool ended = false;
    {
        frdx::UniqueLock lk{mu_};
        p = pipe_.get();
        if (!p) return;
        stderr_pipe = p->stderr_pipe;
        read_pipe = p->read_pipe;
        ended = p->ended;
    }

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };

    // ---- Stderr drain (ffmpeg error/warning logs) ----
    // ICY metadata is now fetched via a direct HTTP connection (run_icy_poller),
    // so we no longer parse [icy] StreamTitle from ffmpeg's stderr.
    // The stderr pipe is kept to capture actual ffmpeg errors.
    if (stderr_pipe) {
        for (int safety = 0; safety < 3; ++safety) {
            DWORD eavail = 0;
            if (!PeekNamedPipe(stderr_pipe, nullptr, 0, nullptr, &eavail, nullptr)) {
                CloseHandle(stderr_pipe);
                {
                    frdx::UniqueLock lk{mu_};
                    if (p && pipe_.get() == p) p->stderr_pipe = nullptr;
                }
                break;
            }
            if (eavail == 0) break;
            char ebuf[512];
            DWORD egot = 0;
            if (!ReadFile(stderr_pipe, ebuf, sizeof(ebuf) - 1, &egot, nullptr) || egot == 0) {
                CloseHandle(stderr_pipe);
                {
                    frdx::UniqueLock lk{mu_};
                    if (p && pipe_.get() == p) p->stderr_pipe = nullptr;
                }
                break;
            }
            {
                frdx::UniqueLock lk{mu_};
                if (p && pipe_.get() == p) p->stderr_buf.append(ebuf, egot);
            }
            std::string local_buf;
            {
                frdx::UniqueLock lk{mu_};
                if (p && pipe_.get() == p) {
                    if (p->stderr_buf.size() > 65536)
                        p->stderr_buf.erase(0, p->stderr_buf.size() - 32768);
                    local_buf = std::move(p->stderr_buf);
                    p->stderr_buf.clear();
                }
            }
            if (!local_buf.empty()) {
                for (;;) {
                    auto nl = local_buf.find('\n');
                    if (nl == std::string::npos) break;
                    std::string line = local_buf.substr(0, nl);
                    local_buf.erase(0, nl + 1);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    if (line.empty()) continue;
                    log::warn("[shoutcast] ffmpeg: {}", line);
                }
            }
        }
    }

    // If ffmpeg has exited and the ring is drained, try reconnecting.
    if (ended) {
        update_position();
        if (ring.readable() == 0) {
            frdx::UniqueLock lk{mu_};
            if (reconnect_attempts_ < 3) {
                reconnect_attempts_++;
                log::info("[shoutcast] stream disconnected; attempting reconnect {}/3",
                          reconnect_attempts_);
                info_.title  = display_name_;
                info_.artist = "Attempting to reconnect to station";
                info_gen_.fetch_add(1, std::memory_order_release);
            } else {
                reconnect_attempts_ = 0;
                log::warn("[shoutcast] max reconnect attempts reached; cycling presets");
                if (!cfg_.presets.empty() || !stations_.empty()) {
                    info_.title  = display_name_;
                    info_.artist = "Unable to reach this station";
                    info_gen_.fetch_add(1, std::memory_order_release);
                    cycle_preset();
                }
            }
            // Caution: p (captured via pipe_.get()) becomes dangling here
            // because start_pipe_locked() calls stop_pipe_locked() → pipe_.reset().
            // p is intentionally not dereferenced after this point.
            // Save before start_pipe_locked resets it to "Online Radio".
            std::string prev_name = display_name_;
            start_pipe_locked();
            bool has_pipe = (pipe_ != nullptr);
            if (!has_pipe) {
                // Pipe launch failed — restore unreachable status.
                info_.title   = prev_name;
                info_.artist  = "Unable to reach this station";
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
        frdx::UniqueLock lk{mu_};
        if (p && pipe_.get() == p) {
            if (p->bytes_written == 0) {
                log::warn("[shoutcast] stream produced 0 bytes; stopping");
                stop_pipe_locked();
                return;
            }
            p->ended = true;
            info_.title = display_name_;
            info_.artist = "Lost connection to station";
            info_gen_.fetch_add(1, std::memory_order_release);
            if (p->read_pipe) {
                CloseHandle(p->read_pipe);
                p->read_pipe = nullptr;
            }
            if (p->stderr_pipe) {
                CloseHandle(p->stderr_pipe);
                p->stderr_pipe = nullptr;
            }
        }
    };

    DWORD avail = 0;
    if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        int16_t buf[2048]; // 1024 stereo frames * 2 channels = 4096 bytes
        DWORD got = 0;
        if (!ReadFile(read_pipe, buf, static_cast<DWORD>(want), &got, nullptr) || got == 0) {
            on_eof();
            return;
        }
        if (got > 0) {
            frdx::UniqueLock lk{mu_};
            if (reconnect_attempts_ > 0) reconnect_attempts_ = 0;
        }
        ring.write(reinterpret_cast<std::byte*>(buf), got);
        {
            frdx::UniqueLock lk{mu_};
            if (p && pipe_.get() == p) p->bytes_written += got;
        }
        update_position();
        avail = avail > got ? avail - got : 0;
        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() > 32 * 1024)
            state_.store(PlaybackState::playing, std::memory_order_release);
    }
    update_position();
}

} // namespace frdx::sources
