#include "fh6/sources/shoutcast_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace fh6::sources {

using json = nlohmann::json;

namespace {

// PCM contract written by ffmpeg: 48000 Hz * 2 ch * 2 bytes.
constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;

// Synchronous WinHTTP GET for one-shot metadata polling.  Returns the
// response body as a string, or empty on any failure (timeout, non-200, etc.).
// Uses system WinHTTP rather than cpp-httplib to avoid static-linking issues
// in the game mod DLL context.
std::string winhttp_get(void* session_handle, const std::string& url, int timeout_secs = 3) {
    std::wstring wurl = fh6::subprocess::widen(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256]   = {};
    wchar_t path[2048]  = {};
    uc.lpszHostName     = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath      = path;
    uc.dwUrlPathLength  = 2047;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {};

    bool is_ssl        = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort ? uc.nPort : (is_ssl ? 443 : 80);

    HINTERNET hSession = session_handle ? static_cast<HINTERNET>(session_handle) : nullptr;
    bool local_session = false;
    if (!hSession) {
        hSession = WinHttpOpen(L"fh6-radio/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return {};
        local_session = true;
    }

    if (local_session) {
        DWORD to = static_cast<DWORD>(timeout_secs) * 1000;
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &to, sizeof(to));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) {
        if (local_session) WinHttpCloseHandle(hSession);
        return {};
    }

    HINTERNET hRequest =
        WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, is_ssl ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        if (local_session) WinHttpCloseHandle(hSession);
        return {};
    }

    // Ignore SSL certificate errors for public metadata JSON APIs.
    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        if (local_session) WinHttpCloseHandle(hSession);
        return {};
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        if (local_session) WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD status    = 0;
    DWORD status_sz = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &status_sz, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        if (local_session) WinHttpCloseHandle(hSession);
        return {};
    }
    if (status != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        if (local_session) WinHttpCloseHandle(hSession);
        return {};
    }

    // Read response body.
    std::string body;
    char buf[4096];
    DWORD got = 0;
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &got) && got > 0) {
        body.append(buf, got);
        got = 0;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    if (local_session) WinHttpCloseHandle(hSession);
    return body;
}

// Resolve a dot-notation JSON path (e.g. "data.track_title") to a value in a nlohmann::json tree.
// Returns the value if found, otherwise an empty string.
std::string resolve_json_path(const json& root, const std::string& dot_path) {
    if (dot_path.empty()) return {};
    const json* node = &root;
    std::size_t pos  = 0;
    while (pos < dot_path.size()) {
        auto dot = dot_path.find('.', pos);
        auto key =
            (dot == std::string::npos) ? dot_path.substr(pos) : dot_path.substr(pos, dot - pos);
        if (key.empty()) return {};
        // Support array index: "results.0.title" -> key is "0" inside array
        if (node->is_array()) {
            try {
                auto idx = static_cast<std::size_t>(std::stoul(key));
                if (idx >= node->size()) return {};
                node = &(*node)[idx];
            } catch (...) {
                return {};
            }
        } else if (node->is_object()) {
            auto it = node->find(key);
            if (it == node->end()) return {};
            node = &*it;
        } else {
            return {};
        }
        pos = (dot == std::string::npos) ? dot_path.size() : dot + 1;
    }
    if (node->is_string()) return node->get<std::string>();
    if (node->is_number_integer()) return std::to_string(node->get<int64_t>());
    if (node->is_number_float()) return std::to_string(node->get<double>());
    return node->dump();
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

// Apply enhanced mapping to resolve a single field from JSON.
// Handles: dot-path resolution, artist_array_field (join array of objects),
// artist_separator, and artwork_url_template ({value} substitution).
std::string resolve_mapped_value(const json& root, const std::string& path,
                                 const std::string& array_field, const std::string& separator,
                                 const std::string& url_template) {
    if (path.empty()) return {};

    // If array_field is set, resolve to the node and extract array elements.
    if (!array_field.empty()) {
        const json* node = resolve_json_node(root, path);
        if (node && node->is_array()) {
            std::string result;
            for (std::size_t i = 0; i < node->size(); ++i) {
                const json& elem = (*node)[i];
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
    std::string raw = resolve_json_path(root, path);
    if (raw.empty()) return {};

    // Apply URL template if present.
    if (!url_template.empty()) {
        auto pos = url_template.find("{value}");
        if (pos != std::string::npos) {
            std::string out = url_template;
            out.replace(pos, 7, raw);
            return out;
        }
    }

    return raw;
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
        sp.id          = j.value("id", "");
        sp.name        = j.value("name", "");
        sp.description = j.value("description", "");
        sp.icon        = j.value("icon", "fa-solid fa-radio");
        if (j.contains("streams") && j["streams"].is_array()) {
            for (const auto& s : j["streams"]) {
                StationStream ss;
                ss.id   = s.value("id", "");
                ss.name = s.value("name", "");
                ss.url  = s.value("url", "");
                ss.icon = s.value("icon", "fa-solid fa-play");
                // Per-stream mapping override.
                if (s.contains("mapping")) {
                    const auto& mp                           = s["mapping"];
                    ss.mapping_override.title                = mp.value("title", "");
                    ss.mapping_override.artist               = mp.value("artist", "");
                    ss.mapping_override.artwork_url          = mp.value("artwork_url", "");
                    ss.mapping_override.artist_array_field   = mp.value("artist_array_field", "");
                    ss.mapping_override.artist_separator     = mp.value("artist_separator", ", ");
                    ss.mapping_override.split_separator      = mp.value("split_separator", "");
                    ss.mapping_override.artwork_url_template = mp.value("artwork_url_template", "");
                }
                if (!ss.url.empty()) sp.streams.push_back(std::move(ss));
            }
        }
        if (j.contains("metadata")) {
            const auto& m                  = j["metadata"];
            sp.metadata.type               = m.value("type", "");
            sp.metadata.url                = m.value("url", "");
            sp.metadata.poll_interval_secs = m.value("poll_interval_secs", 20);
            if (m.contains("mapping")) {
                const auto& mp                           = m["mapping"];
                sp.metadata.mapping.title                = mp.value("title", "");
                sp.metadata.mapping.artist               = mp.value("artist", "");
                sp.metadata.mapping.artwork_url          = mp.value("artwork_url", "");
                sp.metadata.mapping.artist_array_field   = mp.value("artist_array_field", "");
                sp.metadata.mapping.artist_separator     = mp.value("artist_separator", ", ");
                sp.metadata.mapping.split_separator      = mp.value("split_separator", "");
                sp.metadata.mapping.artwork_url_template = mp.value("artwork_url_template", "");
            }
            if (m.contains("stream_url_map") && m["stream_url_map"].is_object()) {
                for (auto it = m["stream_url_map"].begin(); it != m["stream_url_map"].end(); ++it)
                    sp.metadata.stream_url_map[it.key()] = it.value().get<std::string>();
            }
            if (m.contains("stream_name_map") && m["stream_name_map"].is_object()) {
                for (auto it = m["stream_name_map"].begin(); it != m["stream_name_map"].end(); ++it)
                    sp.metadata.stream_name_map[it.key()] = it.value().get<std::string>();
            }
            if (m.contains("entry_selector")) {
                const auto& es                              = m["entry_selector"];
                sp.metadata.entry_selector.array_path       = es.value("array_path", "");
                sp.metadata.entry_selector.match_field      = es.value("match_field", "");
                sp.metadata.entry_selector.match_value_from = es.value("match_value_from", "");
            }
            // Static metadata (type == "static").
            sp.metadata.static_title     = m.value("title", "");
            sp.metadata.static_artist    = m.value("artist", "");
            sp.metadata.static_cover_url = m.value("cover_url", "");
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

// Apply entry_selector + mapping to a parsed JSON body.
// Returns {title, artist, artwork_url} — any may be empty.
std::tuple<std::string, std::string, std::string>
resolve_mapping(const json& body, const StationMetadataMapping& mp,
                const StationMetadataEntrySelector& sel, const std::string& match_name) {
    const json* root = &body;
    json matched;
    if (!sel.array_path.empty() && !match_name.empty()) {
        auto it = body.find(sel.array_path);
        if (it != body.end() && it->is_array()) {
            for (const auto& entry : *it) {
                auto f = entry.find(sel.match_field);
                if (f != entry.end() && f->is_string() && f->get<std::string>() == match_name) {
                    matched = entry;
                    root    = &matched;
                    break;
                }
            }
        }
    }
    auto title = resolve_mapped_value(*root, mp.title, "", "", "");
    auto artist =
        resolve_mapped_value(*root, mp.artist, mp.artist_array_field, mp.artist_separator, "");

    // If split_separator is set and no separate artist mapping resolved,
    // try splitting the title field on the separator (e.g. "artist - title").
    if (!mp.split_separator.empty() && artist.empty() && !title.empty()) {
        auto pos = title.find(mp.split_separator);
        if (pos != std::string::npos) {
            artist = title.substr(0, pos);
            title  = title.substr(pos + mp.split_separator.size());
        }
    }

    return {std::move(title), std::move(artist),
            resolve_mapped_value(*root, mp.artwork_url, "", "", mp.artwork_url_template)};
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
    for (auto& rep : kReplace) {
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
        if (job) CloseHandle(job);
        if (proc_ff) CloseHandle(proc_ff);
    }
};

// ---- ShoutcastSource ----

ShoutcastSource::ShoutcastSource(ShoutcastConfig cfg) : cfg_{std::move(cfg)} {
    info_        = TrackInfo{};
    info_.artist = "Select a station";
}

ShoutcastSource::~ShoutcastSource() {
    stop_metadata_poller();
    stop_pipe_locked();
    if (winhttp_session_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(winhttp_session_));
        winhttp_session_ = nullptr;
    }
}

bool ShoutcastSource::initialize() {
    if (!cfg_.enabled) return false;
    if (!cfg_.default_url.empty()) {
        std::scoped_lock lk{mu_};
        target_url_ = cfg_.default_url;
    }
    auth_ = AuthState::none_required;

    // Create a persistent shared WinHTTP session with default timeouts.
    winhttp_session_ = WinHttpOpen(L"fh6-radio/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (winhttp_session_) {
        DWORD to = 3000; // 3 seconds timeout
        WinHttpSetOption(static_cast<HINTERNET>(winhttp_session_), WINHTTP_OPTION_CONNECT_TIMEOUT,
                         &to, sizeof(to));
        WinHttpSetOption(static_cast<HINTERNET>(winhttp_session_), WINHTTP_OPTION_SEND_TIMEOUT, &to,
                         sizeof(to));
        WinHttpSetOption(static_cast<HINTERNET>(winhttp_session_), WINHTTP_OPTION_RECEIVE_TIMEOUT,
                         &to, sizeof(to));
    } else {
        log::warn("[shoutcast] failed to initialize shared WinHTTP session; falling back to local "
                  "sessions");
    }

    return true;
}

void ShoutcastSource::shutdown() noexcept {
    stop_metadata_poller();
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
    if (winhttp_session_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(winhttp_session_));
        winhttp_session_ = nullptr;
    }
}

void ShoutcastSource::set_target(std::string url) {
    std::scoped_lock lk{mu_};
    target_url_ = std::move(url);
    active_station_id_.clear();
    active_stream_id_.clear();
}

void ShoutcastSource::set_presets(std::vector<std::string> presets) {
    std::scoped_lock lk{mu_};
    cfg_.presets = std::move(presets);
    preset_idx_  = 0;
}

void ShoutcastSource::load_stations(const std::filesystem::path& path) {
    auto loaded = parse_stations_json(path);
    std::scoped_lock lk{mu_};
    stations_ = std::move(loaded);
    log::info("[shoutcast] {} station(s) available", stations_.size());
}

// ---- ffmpeg subprocess launcher (no mu_ held) ----

std::unique_ptr<ShoutcastSource::Pipe>
ShoutcastSource::launch_ffmpeg_pipe(const std::string& url,
                                    const std::filesystem::path& ffmpeg_path) {
    auto pipe = std::make_unique<Pipe>();
    pipe->job = fh6::subprocess::create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[shoutcast] CreateJobObject failed ({})", GetLastError());
        return nullptr;
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
        return nullptr;
    }
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);

    // Stderr pipe for ICY metadata. ffmpeg prints [icy] StreamTitle='...' lines here.
    if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) {
        bail();
        return nullptr;
    }
    SetHandleInformation(err_r, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in = fh6::subprocess::open_nul(GENERIC_READ);

    const auto ff = ffmpeg_path.empty() ? L"ffmpeg" : ffmpeg_path.wstring();

    std::wstring ff_cmd = fh6::subprocess::quote(ff) +
                          L" -loglevel error"
                          L" -icy 1"
                          L" -reconnect 1"
                          L" -reconnect_streamed 1"
                          L" -reconnect_delay_max 5"
                          L" -i " +
                          fh6::subprocess::quote(fh6::subprocess::widen(url)) +
                          L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc_ff     = fh6::subprocess::spawn_in_job(pipe->job, ff_cmd, nul_in, ff_out_w, err_w);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(ff_out_w);
    CloseHandle(err_w);
    ff_out_w = nullptr;
    err_w    = nullptr;
    if (nul_in) CloseHandle(nul_in);

    if (!pipe->proc_ff) {
        log::warn("[shoutcast] failed to launch ffmpeg -- {}",
                  fh6::subprocess::describe_launch_failure(std::wstring{ff}, ec_ff,
                                                           !ffmpeg_path.empty()));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (err_r) CloseHandle(err_r);
        return nullptr;
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
        std::scoped_lock lk{mu_};

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
        if (md_type == "rest" || md_type == "websocket") {
            auto mit      = station->metadata.stream_url_map.find(stream_id);
            md_url        = (mit != station->metadata.stream_url_map.end()) ? mit->second
                                                                            : station->metadata.url;
            metadata_url_ = md_url;
        }

        // Handle static metadata: set info_ directly, no poller needed.
        if (md_type == "static") {
            has_static              = true;
            static_info.title       = sanitize_brackets(station->metadata.static_title.empty()
                                                            ? station->name
                                                            : station->metadata.static_title);
            static_info.artist      = sanitize_brackets(station->metadata.static_artist);
            static_info.artwork_url = station->metadata.static_cover_url;
        }

        // Signal the old metadata thread to stop before restarting the
        // pipe, so even if it's about to write a stale poll result to
        // info_ it sees the flag and skips the write.
        metadata_stop_.store(true, std::memory_order_release);

        // Stop old pipe (fast — no ffmpeg launch here).
        stop_pipe_locked();
    } // mu_ released — ffmpeg launch happens outside the lock

    // Phase 2: launch ffmpeg OUTSIDE the lock (expensive subprocess creation).
    auto new_pipe = launch_url.empty() ? nullptr : launch_ffmpeg_pipe(launch_url, cfg_.ffmpeg_path);

    // Phase 3: install pipe and set metadata under lock.
    bool has_pipe = false;
    {
        std::scoped_lock lk{mu_};
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
            display_name_     = station_name;
            info_.duration_ms = 0;
            position_ms_.store(0, std::memory_order_release);
            state_.store(PlaybackState::buffering, std::memory_order_release);
        }
    }

    // Stop old poller and start the appropriate new one.
    stop_metadata_poller();
    restart_metadata_for_current();

    if (has_pipe) state_.store(PlaybackState::playing, std::memory_order_release);

    log::info("[shoutcast] selected station '{}' stream '{}' ({}) metadata={} type={}", station_id,
              stream_id, launch_url, md_url.empty() ? "none" : md_url,
              md_type.empty() ? "icy" : md_type);
}

void ShoutcastSource::cycle_preset(int direction) {
    if (direction >= 0)
        direction = 1;
    else
        direction = -1;

    if (!stations_.empty()) {
        // Build a flat list of (station_idx, stream_idx) pairs for all streams
        // across all stations, so next/back cycles through every stream.
        struct Entry {
            std::size_t st_idx;
            std::size_t str_idx;
        };
        std::vector<Entry> flat;
        for (std::size_t i = 0; i < stations_.size(); ++i)
            for (std::size_t j = 0; j < stations_[i].streams.size(); ++j) flat.push_back({i, j});

        if (flat.empty()) return;

        // Find the current (station_id, stream_id) in the flat list.
        std::size_t cur = 0;
        for (; cur < flat.size(); ++cur) {
            const auto& st  = stations_[flat[cur].st_idx];
            const auto& str = st.streams[flat[cur].str_idx];
            if (st.id == active_station_id_ && str.id == active_stream_id_) break;
        }
        // If not found (e.g. no station selected yet), start at index 0
        // for forward, last for backward.
        if (cur >= flat.size()) cur = (direction > 0) ? flat.size() - 1 : 0;

        // Advance/retreat by 1, wrapping around.
        std::size_t next    = (cur + flat.size() + direction) % flat.size();
        const auto& new_st  = stations_[flat[next].st_idx];
        const auto& new_str = new_st.streams[flat[next].str_idx];

        target_url_        = new_str.url;
        active_station_id_ = new_st.id;
        active_stream_id_  = new_str.id;
        metadata_url_.clear();
        if (new_st.metadata.type == "rest") {
            auto mit = new_st.metadata.stream_url_map.find(new_str.id);
            metadata_url_ =
                (mit != new_st.metadata.stream_url_map.end()) ? mit->second : new_st.metadata.url;
        } else if (new_st.metadata.type == "websocket") {
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
    display_name_ = "Online Radio";
    if (target_url_.empty()) return;

    auto pipe = std::make_unique<Pipe>();
    pipe->job = fh6::subprocess::create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[shoutcast] CreateJobObject failed ({})", GetLastError());
        return;
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
        return;
    }
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT);

    // Stderr pipe for ICY metadata. ffmpeg prints [icy] StreamTitle='...' lines here.
    if (!CreatePipe(&err_r, &err_w, &sa, 1 << 16)) {
        bail();
        return;
    }
    SetHandleInformation(err_r, 0, HANDLE_FLAG_INHERIT);

    HANDLE nul_in = fh6::subprocess::open_nul(GENERIC_READ);

    const auto ff = cfg_.ffmpeg_path.empty() ? L"ffmpeg" : cfg_.ffmpeg_path.wstring();

    std::wstring ff_cmd = fh6::subprocess::quote(ff) +
                          L" -loglevel error"
                          L" -icy 1"
                          L" -reconnect 1"
                          L" -reconnect_streamed 1"
                          L" -reconnect_delay_max 5"
                          L" -i " +
                          fh6::subprocess::quote(fh6::subprocess::widen(target_url_)) +
                          L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc_ff     = fh6::subprocess::spawn_in_job(pipe->job, ff_cmd, nul_in, ff_out_w, err_w);
    const DWORD ec_ff = pipe->proc_ff ? 0u : GetLastError();
    CloseHandle(ff_out_w);
    CloseHandle(err_w);
    ff_out_w = nullptr;
    err_w    = nullptr;
    if (nul_in) CloseHandle(nul_in);

    if (!pipe->proc_ff) {
        log::warn("[shoutcast] failed to launch ffmpeg -- {}",
                  fh6::subprocess::describe_launch_failure(std::wstring{ff}, ec_ff,
                                                           !cfg_.ffmpeg_path.empty()));
        if (ff_out_r) CloseHandle(ff_out_r);
        if (err_r) CloseHandle(err_r);
        return;
    }

    pipe->read_pipe   = ff_out_r;
    pipe->stderr_pipe = err_r;
    pipe_             = std::move(pipe);

    info_         = TrackInfo{};
    info_.artist  = "Select a station";
    display_name_ = "Online Radio";
    if (!active_station_id_.empty()) {
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                // Station name goes in title (top row), stream name goes
                // in artist (bottom row) so both rows show meaningful
                // content immediately. Real metadata overwrites both
                // when the background poller / ICY parser delivers it.
                info_.title   = st.name;
                display_name_ = st.name;
                for (const auto& str : st.streams) {
                    if (str.id == active_stream_id_) {
                        info_.artist = str.name;
                        break;
                    }
                }
                break;
            }
        }
    }
    // Set artist to the station name too so the in-game HUD always shows
    // meaningful content on both rows. When the background poller fetches
    // real track metadata, it overwrites both fields.
    info_.duration_ms = 0;
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
        std::scoped_lock lk{mu_};
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
    stop_metadata_poller();
    {
        std::scoped_lock lk{mu_};
        stop_pipe_locked();
        info_         = TrackInfo{};
        info_.artist  = "Nothing playing";
        display_name_ = "Online Radio";
    }
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void ShoutcastSource::stop() {
    stop_metadata_poller();
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void ShoutcastSource::next() {
    bool has_pipe = false;
    {
        std::scoped_lock lk{mu_};
        cycle_preset(1);
        start_pipe_locked();
        // Handle static metadata from new station.
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                if (st.metadata.type == "static") {
                    info_.title =
                        st.metadata.static_title.empty() ? st.name : st.metadata.static_title;
                    info_.artist      = st.metadata.static_artist;
                    info_.artwork_url = st.metadata.static_cover_url;
                }
                break;
            }
        }
        has_pipe = (pipe_ != nullptr);
    } // mu_ released before poller stop/start
    restart_metadata_for_current();
    if (has_pipe) state_.store(PlaybackState::playing, std::memory_order_release);
}

void ShoutcastSource::previous() {
    bool has_pipe = false;
    {
        std::scoped_lock lk{mu_};
        cycle_preset(-1);
        start_pipe_locked();
        // Handle static metadata from new station.
        for (const auto& st : stations_) {
            if (st.id == active_station_id_) {
                if (st.metadata.type == "static") {
                    info_.title =
                        st.metadata.static_title.empty() ? st.name : st.metadata.static_title;
                    info_.artist      = st.metadata.static_artist;
                    info_.artwork_url = st.metadata.static_cover_url;
                }
                break;
            }
        }
        has_pipe = (pipe_ != nullptr);
    } // mu_ released before poller stop/start
    restart_metadata_for_current();
    if (has_pipe) state_.store(PlaybackState::playing, std::memory_order_release);
}

TrackInfo ShoutcastSource::current_track() const {
    std::scoped_lock lk{mu_};
    TrackInfo t   = info_;
    t.position_ms = position_ms_.load(std::memory_order_acquire);
    return t;
}

std::string ShoutcastSource::auth_instructions() const {
    return "Enter a Shoutcast or Icecast stream URL (e.g. http://server:8000/stream), "
           "or load station presets from a stations.json catalog. "
           "ffmpeg must be on PATH or set [shoutcast].ffmpeg_path in config.toml.";
}

// ---- Metadata poller (dispatcher) ----

void ShoutcastSource::restart_metadata_for_current() {
    // Spawn a background thread that fetches metadata for the currently
    // active station. Supports both REST polling and WebSocket live feeds.
    // Called from select_station(), next(), and previous() after selecting
    // a new station.
    //
    // REST: first HTTP request fires after a 1-second settle delay so the
    // station/stream names are visible briefly in-game, then repeats every
    // poll_interval_secs.
    // WebSocket: opens a persistent connection and receives updates in
    // real-time, with automatic reconnect on disconnect.
    //
    // All state is captured by value — no dangling reference issues.
    // ICY StreamTitle from ffmpeg stderr (parsed in pump()) continues to
    // provide real-time updates for stations that embed ICY tags.

    std::unique_lock lk{mu_};
    if (active_station_id_.empty() || metadata_url_.empty()) return;

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
    // If this stream has its own mapping override, use it instead.
    if (stream && (!stream->mapping_override.title.empty() ||
                   !stream->mapping_override.split_separator.empty() ||
                   !stream->mapping_override.artist.empty())) {
        mapping = stream->mapping_override;
        log::info("[shoutcast] using stream-level mapping override for '{}'", stream->id);
    }
    auto selector = station->metadata.entry_selector;

    std::string match_name;
    if (!selector.match_value_from.empty() && stream) {
        auto nit = station->metadata.stream_name_map.find(stream->id);
        if (nit != station->metadata.stream_name_map.end()) match_name = nit->second;
    }

    auto url     = metadata_url_;
    int interval = station->metadata.poll_interval_secs;
    if (interval < 5) interval = 20;

    // Capture metadata type and WebSocket-specific fields.
    auto md_type      = station->metadata.type;
    auto ws_filter    = station->metadata.ws_filter;
    auto ws_heartbeat = station->metadata.ws_heartbeat;

    // Reset ICY timestamp so the first REST/WS poller response fills title/artist.
    last_icy_ms_.store(0, std::memory_order_release);

    lk.unlock();

    // Stop any existing background poller before starting a new one.
    stop_metadata_poller();

    // Static or no type -> no background poller needed.
    if (md_type == "static" || md_type.empty()) return;

    metadata_stop_.store(false, std::memory_order_release);

    // ---- WebSocket metadata poller ----
    if (md_type == "websocket") {
        metadata_thread_ = std::jthread([this, url = std::move(url), mapping = std::move(mapping),
                                         filter = std::move(ws_filter),
                                         hb =
                                             std::move(ws_heartbeat)](const std::stop_token& stop) {
            while (!stop.stop_requested() && !metadata_stop_.load(std::memory_order_acquire)) {
                // ---- Parse the WebSocket URL ----
                // WinHttpCrackUrl doesn't recognise the wss:// scheme, so
                // substitute https:// (the actual WebSocket upgrade is handled
                // by WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET regardless).
                std::string ws_url = url;
                auto ws_pos        = ws_url.find("://");
                if (ws_pos != std::string::npos) {
                    std::string scheme = ws_url.substr(0, ws_pos);
                    if (scheme == "wss")
                        ws_url.replace(0, 3, "https");
                    else if (scheme == "ws")
                        ws_url.replace(0, 2, "http");
                }
                std::wstring wurl = fh6::subprocess::widen(ws_url);
                bool is_ssl       = false;
                URL_COMPONENTS uc{};
                uc.dwStructSize = sizeof(uc);

                wchar_t host[256]   = {};
                wchar_t path[1024]  = {};
                uc.lpszHostName     = host;
                uc.dwHostNameLength = 255;
                uc.lpszUrlPath      = path;
                uc.dwUrlPathLength  = 1023;

                if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
                    log::warn("[shoutcast] ws: failed to parse URL '{}'", url);
                    break;
                }

                is_ssl     = (uc.nScheme == INTERNET_SCHEME_HTTPS);
                DWORD port = uc.nPort ? uc.nPort : (is_ssl ? 443 : 80);

                HINTERNET hSession =
                    winhttp_session_ ? static_cast<HINTERNET>(winhttp_session_) : nullptr;
                bool local_session = false;
                if (!hSession) {
                    hSession = WinHttpOpen(L"fh6-radio/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                    if (!hSession) {
                        log::warn("[shoutcast] ws: WinHttpOpen failed ({})", GetLastError());
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        continue;
                    }
                    local_session = true;
                }

                if (local_session) {
                    DWORD recv_timeout_ms = 5000;
                    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &recv_timeout_ms,
                                     sizeof(recv_timeout_ms));
                }

                HINTERNET hConnect =
                    WinHttpConnect(hSession, host, static_cast<INTERNET_PORT>(port), 0);
                if (!hConnect) {
                    log::warn("[shoutcast] ws: WinHttpConnect failed ({})", GetLastError());
                    if (local_session) WinHttpCloseHandle(hSession);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }

                HINTERNET hRequest = WinHttpOpenRequest(
                    hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES, is_ssl ? WINHTTP_FLAG_SECURE : 0);
                if (!hRequest) {
                    log::warn("[shoutcast] ws: WinHttpOpenRequest failed ({})", GetLastError());
                    CloseHandle(hConnect);
                    if (local_session) WinHttpCloseHandle(hSession);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }

                // Ignore SSL certificate errors (same as REST poller).
                {
                    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
                    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
                }

                if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
                    log::warn("[shoutcast] ws: upgrade failed ({})", GetLastError());
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    if (local_session) WinHttpCloseHandle(hSession);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }

                if (!WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0)) {
                    log::warn("[shoutcast] ws: SendRequest failed ({})", GetLastError());
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    if (local_session) WinHttpCloseHandle(hSession);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }

                if (!WinHttpReceiveResponse(hRequest, nullptr)) {
                    log::warn("[shoutcast] ws: ReceiveResponse failed ({})", GetLastError());
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    if (local_session) WinHttpCloseHandle(hSession);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }

                HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
                if (!hWebSocket) {
                    log::warn("[shoutcast] ws: upgrade complete failed ({})", GetLastError());
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    if (local_session) WinHttpCloseHandle(hSession);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }

                {
                    std::scoped_lock lk{ws_mu_};
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
                while (!stop.stop_requested() && !metadata_stop_.load(std::memory_order_acquire)) {
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

                    std::string title = resolve_mapped_value(*data_root, mapping.title, "", "", "");
                    std::string artist =
                        resolve_mapped_value(*data_root, mapping.artist, mapping.artist_array_field,
                                             mapping.artist_separator, "");
                    std::string artwork_url = resolve_mapped_value(
                        *data_root, mapping.artwork_url, "", "", mapping.artwork_url_template);

                    // If ICY metadata has ever been received for this stream
                    // (since the last reset in restart_metadata_for_current),
                    // only let the WS poller update artwork — ICY provides
                    // perfectly-synced title/artist at the audio transition point
                    // and never goes stale (the flag is reset on station change).
                    auto last_icy   = last_icy_ms_.load(std::memory_order_acquire);
                    bool icy_active = (last_icy > 0);

                    if (!title.empty() || !artist.empty() || !artwork_url.empty()) {
                        std::scoped_lock lk{mu_};
                        // Skip write if a new station was selected while
                        // we were processing — metadata_stop_ is set early
                        // in select_station() under mu_.
                        if (!metadata_stop_.load(std::memory_order_acquire)) {
                            if (!title.empty() && !icy_active)
                                info_.title = sanitize_brackets(std::move(title));
                            if (!artist.empty() && !icy_active)
                                info_.artist = sanitize_brackets(std::move(artist));
                            if (!artwork_url.empty()) info_.artwork_url = std::move(artwork_url);
                            log::info("[shoutcast] ws metadata: {} - {} (icy={})", info_.artist,
                                      info_.title, icy_active);
                        }
                    }
                }

                // Cleanup connection.
                {
                    std::scoped_lock lk{ws_mu_};
                    if (ws_handle_ == hWebSocket) ws_handle_ = nullptr;
                }
                if (!metadata_stop_.load(std::memory_order_acquire))
                    WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                          nullptr, 0);
                WinHttpCloseHandle(hWebSocket);
                WinHttpCloseHandle(hConnect);
                if (local_session) WinHttpCloseHandle(hSession);

                if (stop.stop_requested() || metadata_stop_.load(std::memory_order_acquire)) break;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        });
        return;
    }

    // ---- REST metadata poller (default) ----
    // Fetches after a brief settle delay so the station/stream names are
    // visible in-game before song metadata overwrites them, then sleeps
    // between subsequent polls.
    metadata_thread_ = std::jthread([this, url = std::move(url), mapping = std::move(mapping),
                                     sel = std::move(selector), match_name = std::move(match_name),
                                     interval](const std::stop_token& stop) {
        // Settle delay so the station name is visible in-game before
        // song metadata overwrites it (only fires once on station open).
        for (int i = 0; i < 10; ++i) {
            if (stop.stop_requested() || metadata_stop_.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        while (!stop.stop_requested() && !metadata_stop_.load(std::memory_order_acquire)) {
            std::string raw = winhttp_get(winhttp_session_, url, 3);
            if (raw.empty()) {
                // If the request failed, still sleep before retrying.
                for (int i = 0; i < interval; ++i) {
                    if (stop.stop_requested() || metadata_stop_.load(std::memory_order_acquire))
                        return;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                continue;
            }

            json body;
            try {
                body = json::parse(raw);
            } catch (...) {
                for (int i = 0; i < interval; ++i) {
                    if (stop.stop_requested() || metadata_stop_.load(std::memory_order_acquire))
                        return;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                continue;
            }

            auto [title, artist, artwork_url] = resolve_mapping(body, mapping, sel, match_name);

            // If ICY metadata has ever been received for this stream
            // (since the last reset in restart_metadata_for_current),
            // only let the REST poller update artwork — ICY provides
            // perfectly-synced title/artist at the audio transition point
            // and never goes stale (the flag is reset on station change).
            auto last_icy   = last_icy_ms_.load(std::memory_order_acquire);
            bool icy_active = (last_icy > 0);

            if (!title.empty() || !artist.empty() || !artwork_url.empty()) {
                std::scoped_lock lk{mu_};
                // Skip write if a new station was selected while we were
                // processing — metadata_stop_ is set early in
                // select_station() under mu_.
                if (!metadata_stop_.load(std::memory_order_acquire)) {
                    if (!title.empty() && !icy_active)
                        info_.title = sanitize_brackets(std::move(title));
                    if (!artist.empty() && !icy_active)
                        info_.artist = sanitize_brackets(std::move(artist));
                    if (!artwork_url.empty()) info_.artwork_url = std::move(artwork_url);
                    log::info("[shoutcast] bg metadata: {} - {} (icy={})", info_.artist,
                              info_.title, icy_active);
                }
            }

            // Sleep for the poll interval, checking stop every second.
            for (int i = 0; i < interval; ++i) {
                if (stop.stop_requested() || metadata_stop_.load(std::memory_order_acquire)) return;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void ShoutcastSource::stop_metadata_poller() {
    metadata_stop_.store(true, std::memory_order_release);
    // Force-close any active WebSocket handle to unblock WinHttpWebSocketReceive.
    // Only send the close frame — do NOT WinHttpCloseHandle here; the thread
    // owns the handle and will close it in its cleanup path.
    {
        std::scoped_lock lk{ws_mu_};
        if (ws_handle_) {
            WinHttpWebSocketClose(ws_handle_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            ws_handle_ = nullptr;
        }
    }
    if (metadata_thread_.joinable()) metadata_thread_.request_stop();
    if (metadata_thread_.joinable()) metadata_thread_.join();
    metadata_stop_.store(false, std::memory_order_release);
}

// ---- PCM pump ----

void ShoutcastSource::pump(RingBuffer& ring) {
    {
        auto st = state_.load(std::memory_order_acquire);
        if (st != PlaybackState::playing && st != PlaybackState::buffering) return;
    }

    std::unique_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    auto update_position = [&] {
        const std::size_t r        = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        position_ms_.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };

    // ---- Stderr drain & ICY metadata parse ----
    if (p->stderr_pipe) {
        // Static metadata stations must not have their info_ overwritten by
        // incidental ICY StreamTitle tags from the underlying stream.
        bool has_static_md = false;
        for (const auto& st : stations_) {
            if (st.id == active_station_id_ && st.metadata.type == "static") {
                has_static_md = true;
                break;
            }
        }
        for (int safety = 0; safety < 8; ++safety) {
            DWORD eavail = 0;
            if (!PeekNamedPipe(p->stderr_pipe, nullptr, 0, nullptr, &eavail, nullptr)) {
                CloseHandle(p->stderr_pipe);
                p->stderr_pipe = nullptr;
                break;
            }
            if (eavail == 0) break;
            char ebuf[512];
            DWORD egot = 0;
            if (!ReadFile(p->stderr_pipe, ebuf, sizeof(ebuf) - 1, &egot, nullptr) || egot == 0) {
                CloseHandle(p->stderr_pipe);
                p->stderr_pipe = nullptr;
                break;
            }
            p->stderr_buf.append(ebuf, egot);
            if (p->stderr_buf.size() > 65536) p->stderr_buf.erase(0, p->stderr_buf.size() - 32768);
            for (;;) {
                auto nl = p->stderr_buf.find('\n');
                if (nl == std::string::npos) break;
                std::string line = p->stderr_buf.substr(0, nl);
                p->stderr_buf.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty()) continue;
                // Only process ICY for non-static stations (static metadata
                // is set directly in select_station and must not be overwritten).
                if (line.starts_with("[icy]") && !has_static_md) {
                    auto st_pos = line.find("StreamTitle='");
                    if (st_pos != std::string::npos) {
                        auto val_start = st_pos + 13;
                        auto val_end   = line.find('\'', val_start);
                        if (val_end != std::string::npos) {
                            std::string val = line.substr(val_start, val_end - val_start);
                            val             = sanitize_brackets(std::move(val));
                            auto dash       = val.find(" - ");
                            if (dash != std::string::npos) {
                                info_.artist = val.substr(0, dash);
                                info_.title  = val.substr(dash + 3);
                            } else {
                                info_.artist = display_name_;
                                info_.title  = std::move(val);
                            }
                            // Record timestamp so REST/WS poller knows ICY is active.
                            last_icy_ms_.store(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count(),
                                std::memory_order_release);
                            log::info("[shoutcast] icy: {} - {}", info_.artist, info_.title);
                        }
                    }
                } else if (!line.starts_with("[icy]")) {
                    log::warn("[shoutcast] ffmpeg: {}", line);
                }
            }
        }
    }

    // If ffmpeg has exited and the ring is drained, try reconnecting.
    if (p->ended) {
        update_position();
        if (ring.readable() == 0) {
            if (reconnect_attempts_ < 3) {
                reconnect_attempts_++;
                log::info("[shoutcast] stream disconnected; attempting reconnect {}/3",
                          reconnect_attempts_);
            } else {
                reconnect_attempts_ = 0;
                log::warn("[shoutcast] max reconnect attempts reached; cycling presets");
                if (!cfg_.presets.empty() || !stations_.empty()) cycle_preset();
            }
            // Caution: p (captured via pipe_.get()) becomes dangling here
            // because start_pipe_locked() calls stop_pipe_locked() → pipe_.reset().
            // p is intentionally not dereferenced after this point.
            start_pipe_locked();
            bool has_pipe = (pipe_ != nullptr);
            lk.unlock();
            // Deliberately NOT calling restart_metadata_for_current() here —
            // that does a blocking HTTP GET which would stall the audio thread.
            // The sync poll already ran during select_station()/next()/previous().
            // ICY metadata from ffmpeg will fill in track info as the stream plays.
            if (has_pipe) state_.store(PlaybackState::playing, std::memory_order_release);
        }
        return;
    }

    if (!p->read_pipe) return;

    auto on_eof = [&] {
        if (p->bytes_written == 0) {
            log::warn("[shoutcast] stream produced 0 bytes; stopping");
            stop_pipe_locked();
            return;
        }
        p->ended = true;
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
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
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
        if (!ReadFile(p->read_pipe, buf, static_cast<DWORD>(want), &got, nullptr) || got == 0) {
            on_eof();
            return;
        }
        if (got > 0 && reconnect_attempts_ > 0) {
            reconnect_attempts_ = 0;
        }
        ring.write(reinterpret_cast<std::byte*>(buf), got);
        p->bytes_written += got;
        update_position();
        avail = avail > got ? avail - got : 0;
        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() > 32 * 1024)
            state_.store(PlaybackState::playing, std::memory_order_release);
    }
    update_position();
}

} // namespace fh6::sources
