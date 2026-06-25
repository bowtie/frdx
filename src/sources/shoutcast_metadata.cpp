#include "frdx/sources/shoutcast_helpers.hpp"
#include "frdx/sources/shoutcast_metadata.hpp"
#include "frdx/log.hpp"
#include "frdx/platform/windows/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace frdx::sources {

using json = nlohmann::json;
using namespace helpers;

namespace {

std::optional<int> parse_positive_int(std::string_view text) noexcept {
    if (text.empty()) return std::nullopt;
    int value           = 0;
    const auto* first   = text.data();
    const auto* last    = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value <= 0) return std::nullopt;
    return value;
}

bool recent_metadata_ms(int64_t timestamp_ms, int64_t now_ms) noexcept {
    constexpr int64_t kRecentWindowMs = 12'000;
    return timestamp_ms > 0 && now_ms >= timestamp_ms &&
           now_ms - timestamp_ms <= kRecentWindowMs;
}

} // namespace

// ── Non-inline definitions for helpers ──────────────────────────────

std::optional<ParsedUrl> ParsedUrl::parse(const std::string& url) {
    ParsedUrl p;
    std::string tmp = url;
    auto ws_pos      = tmp.find("://");
    if (ws_pos != std::string::npos) {
        std::string scheme = tmp.substr(0, ws_pos);
        if (scheme == "wss") tmp.replace(0, 3, "https");
        else if (scheme == "ws") tmp.replace(0, 2, "http");
    }
    p.wurl             = frdx::subprocess::widen(tmp);
    p.uc.dwStructSize  = sizeof(p.uc);
    p.uc.lpszHostName  = p.host;
    p.uc.dwHostNameLength = 255;
    p.uc.lpszUrlPath      = p.path;
    p.uc.dwUrlPathLength  = 2047;
    if (!WinHttpCrackUrl(p.wurl.c_str(), 0, 0, &p.uc)) return std::nullopt;
    p.is_ssl = (p.uc.nScheme == INTERNET_SCHEME_HTTPS);
    p.port   = p.uc.nPort ? p.uc.nPort : (p.is_ssl ? 443 : 80);
    return p;
}

WinHttpConn helpers::winhttp_connect(void* shared_session, const ParsedUrl& parsed) {
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
    conn.connect = ScopedWinHttp(WinHttpConnect(hSession, parsed.host, parsed.port, 0));
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

// ---- MetadataController -------------------------------------------------

MetadataController::MetadataController(OnMetadata on_metadata, void* winhttp_session)
    : on_metadata_(std::move(on_metadata)), winhttp_session_(winhttp_session) {}

MetadataController::~MetadataController() { stop(); }

void MetadataController::notify_ffmpeg_icy_metadata(int64_t now_ms) noexcept {
    last_ffmpeg_icy_ms_.store(now_ms, std::memory_order_release);
}

void MetadataController::notify_icy_metadata(int64_t now_ms) noexcept {
    last_icy_ms_.store(now_ms, std::memory_order_release);
}

// ---- Polling control -----------------------------------------------------

void MetadataController::start(const std::string& stream_url,
                               const std::string& metadata_url,
                               const std::string& metadata_type,
                               const StationMetadataMapping& mapping,
                               const StationMetadataWebSocketFilter& ws_filter,
                               const StationMetadataWebSocketHeartbeat& ws_heartbeat,
                               uint64_t metadata_epoch) {
    // 1. Stop existing producers before starting new ones.
    stop();

    // Reset ICY timestamps so the first response fills title/artist.
    last_icy_ms_.store(0, std::memory_order_release);
    last_ffmpeg_icy_ms_.store(0, std::memory_order_release);

    // 2. Start ICY poller for non-WS, non-static stations.
    if (!stream_url.empty() && metadata_type != "websocket" && metadata_type != "static") {
        icy_poller_thread_ = std::jthread(
            [this, url = stream_url, metadata_epoch](const std::stop_token& stop) {
                run_icy_poller(url, metadata_epoch, stop);
            });
    }

    // 3. Static or no type → no background poller needed.
    if (metadata_type == "static" || metadata_type.empty()) return;

    // No metadata URL configured → no WS poller possible.
    if (metadata_url.empty()) return;

    // 4. WebSocket poller.
    if (metadata_type == "websocket") {
        metadata_thread_ = std::jthread(
            [this, url = metadata_url, mapping, filter = ws_filter,
             hb = ws_heartbeat, metadata_epoch](const std::stop_token& stop) {
                run_ws_poller(url, mapping, filter, hb, metadata_epoch, stop);
            });
    }
}

void MetadataController::stop() {
    // Force-close any active WebSocket handle to unblock WinHttpWebSocketReceive.
    {
        std::scoped_lock lk{ws_mu_};
        if (ws_handle_) {
            WinHttpWebSocketClose(ws_handle_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                  nullptr, 0);
            ws_handle_ = nullptr;
        }
    }
    if (metadata_thread_.joinable()) metadata_thread_.request_stop();
    if (metadata_thread_.joinable()) metadata_thread_.join();
    if (icy_poller_thread_.joinable()) icy_poller_thread_.request_stop();
    if (icy_poller_thread_.joinable()) icy_poller_thread_.join();
}

// ---- WebSocket poller ----------------------------------------------------

void MetadataController::run_ws_poller(std::string url,
                                       const StationMetadataMapping& mapping,
                                       const StationMetadataWebSocketFilter& filter,
                                       StationMetadataWebSocketHeartbeat hb,
                                       uint64_t metadata_epoch,
                                       const std::stop_token& stop) {
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
        ScopedWinHttp ws(hWebSocket);
        (void)conn.request.release();

        {
            std::scoped_lock lk{ws_mu_};
            ws_handle_ = hWebSocket;
        }
        log::info("[shoutcast] ws: connected to {}", url);

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

        // Receive loop
        while (!stop.stop_requested()) {
            if (!hb.message.empty()) {
                auto now     = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - last_heartbeat).count();
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
            } catch (...) { continue; }

            if (!hb.interval_field.empty()) {
                std::string ival = resolve_json_path(msg, hb.interval_field);
                if (!ival.empty()) {
                    if (auto new_interval = parse_positive_int(ival))
                        heartbeat_ms = *new_interval;
                }
            }

            // Apply message filter.
            if (!filter.op_field.empty()) {
                const json* op_node = resolve_json_node(msg, filter.op_field);
                if (!op_node) continue;
                int op_val = op_node->is_number_integer()
                               ? static_cast<int>(op_node->get<int64_t>()) : 0;
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
            std::string artist = resolve_mapped_value(*data_root, mapping.artist,
                                                       mapping.artist_array_field,
                                                       mapping.artist_separator);

            const auto now_ms = steady_ms();
            const bool ffmpeg_icy_active = recent_metadata_ms(
                last_ffmpeg_icy_ms_.load(std::memory_order_acquire), now_ms);

            if (!title.empty() || !artist.empty()) {
                TrackInfo info;
                if (!title.empty()) info.title = sanitize_brackets(std::move(title));
                if (!artist.empty()) info.artist = sanitize_brackets(std::move(artist));

                if (!ffmpeg_icy_active && on_metadata_) {
                    bool accepted = on_metadata_(std::move(info), now_ms, metadata_epoch);
                    if (accepted) {
                        log::info("[shoutcast] ws metadata queued: {} - {}",
                                  info.artist, info.title);
                    }
                } else if (ffmpeg_icy_active) {
                    log::info("[shoutcast] ws metadata suppressed by recent ffmpeg ICY: {} - {}",
                              info.artist, info.title);
                }
            }
        }

        // Cleanup.
        {
            std::scoped_lock lk{ws_mu_};
            if (ws_handle_ == hWebSocket) ws_handle_ = nullptr;
        }
        if (!stop.stop_requested()) {
            WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                  nullptr, 0);
        }
        if (stop.stop_requested()) break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// ---- ICY poller ----------------------------------------------------------

void MetadataController::run_icy_poller(const std::string& stream_url,
                                         uint64_t metadata_epoch,
                                         const std::stop_token& stop) {
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

        // Extract icy-metaint.
        wchar_t metaint_buf[64] = {};
        DWORD metaint_len       = sizeof(metaint_buf);
        int metaint             = 0;
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, L"icy-metaint",
                                metaint_buf, &metaint_len, WINHTTP_NO_HEADER_INDEX)) {
            metaint = static_cast<int>(std::wcstol(metaint_buf, nullptr, 10));
        } else {
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
                           (raw_headers[vs] == L' ' || raw_headers[vs] == L'\t')) vs++;
                    auto ve = vs;
                    while (ve < raw_headers.size() && raw_headers[ve] != L'\r' &&
                           raw_headers[ve] != L'\n') ve++;
                    if (ve > vs) {
                        metaint = static_cast<int>(
                            std::wcstol(raw_headers.substr(vs, ve - vs).c_str(), nullptr, 10));
                    }
                }
            }
        }

        if (metaint <= 0) {
            log::warn("[shoutcast] icy: server did not provide icy-metaint for '{}'", stream_url);
            break;
        }

        log::info("[shoutcast] icy: connected, metaint={}", metaint);

        std::size_t buf_needed = static_cast<std::size_t>(metaint) + 1 + 4096;
        if (icy_buf_.size() < buf_needed) icy_buf_.resize(buf_needed);
        char* buf = icy_buf_.data();
        bool first_title = true;

        while (!stop.stop_requested()) {
            // Read metaint audio bytes (discard).
            DWORD audio_read = 0;
            bool error       = false;
            while (std::cmp_less(audio_read, metaint) && !stop.stop_requested()) {
                DWORD chunk = 0;
                if (!WinHttpReadData(hRequest, buf + audio_read,
                                     static_cast<DWORD>(metaint) - audio_read, &chunk)) {
                    error = true; break;
                }
                if (chunk == 0) { error = true; break; }
                audio_read += chunk;
            }
            if (error || stop.stop_requested()) break;

            // Read metadata length byte.
            DWORD meta_len_byte_read = 0;
            if (!WinHttpReadData(hRequest, buf, 1, &meta_len_byte_read) ||
                meta_len_byte_read != 1)
                break;

            int meta_len = static_cast<unsigned char>(buf[0]) * 16;
            if (meta_len <= 0) continue;
            if (meta_len > 4096) meta_len = 4096;

            // Read metadata block.
            DWORD meta_read = 0;
            while (std::cmp_less(meta_read, meta_len) && !stop.stop_requested()) {
                DWORD chunk = 0;
                if (!WinHttpReadData(hRequest, buf + meta_read,
                                     static_cast<DWORD>(meta_len) - meta_read, &chunk)) {
                    error = true; break;
                }
                if (chunk == 0) { error = true; break; }
                meta_read += chunk;
            }
            if (error) break;

            std::string_view meta(buf, meta_read);
            auto val_opt = extract_stream_title(meta);
            if (!val_opt) continue;
            std::string val = std::move(*val_opt);

            auto dash = val.find(" - ");
            const auto now_ms = steady_ms();
            const bool ffmpeg_icy_active = recent_metadata_ms(
                last_ffmpeg_icy_ms_.load(std::memory_order_acquire), now_ms);

            if (!ffmpeg_icy_active && on_metadata_) {
                TrackInfo info;
                if (dash != std::string::npos) {
                    info.artist = val.substr(0, dash);
                    info.title  = val.substr(dash + 3);
                } else {
                    info.artist = "";  // caller fills fallback
                    info.title  = std::move(val);
                }
                bool accepted = on_metadata_(std::move(info), now_ms, metadata_epoch);
                if (accepted) {
                    notify_icy_metadata(now_ms);
                }
            }

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

} // namespace frdx::sources
