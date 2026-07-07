#include "frdx/sources/mpg123_stream.hpp"
#include "frdx/log.hpp"

#include <mpg123.h>

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#pragma comment(lib, "winhttp.lib")

namespace frdx::sources {

// Cast helpers: mh_ is stored as void* in the header to avoid
// exposing mpg123.h to the rest of the codebase.
static mpg123_handle* MH(void* p) { return static_cast<mpg123_handle*>(p); }

// ── Global mpg123 initialization (once) ──
static std::once_flag g_init_once;
static bool g_init_ok = false;

static bool ensure_mpg123_init() {
    std::call_once(g_init_once, [] {
        int err = mpg123_init();
        if (err != MPG123_OK) {
            log::error("[mpg123] mpg123_init() failed: {}", mpg123_plain_strerror(err));
            g_init_ok = false;
        } else {
            log::info("[mpg123] library initialized (API version {})", MPG123_API_VERSION);
            g_init_ok = true;
        }
    });
    return g_init_ok;
}

// ── WinHTTP URL parsing helper ──
struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
};

static std::optional<ParsedUrl> parse_url(const std::string& url) {
    // Convert to wide string for WinHTTP
    std::wstring wurl(url.begin(), url.end());

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength   = static_cast<DWORD>(-1);
    uc.dwUrlPathLength    = static_cast<DWORD>(-1);
    uc.dwSchemeLength     = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc))
        return std::nullopt;

    ParsedUrl out;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.port  = uc.nPort;
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (out.path.empty()) out.path = L"/";
    return out;
}

// ── Mpg123Stream implementation ──

std::expected<std::unique_ptr<Mpg123Stream>, Mpg123StreamError>
Mpg123Stream::create(const std::string& url, void* winhttp_session) {
    if (!ensure_mpg123_init())
        return std::unexpected{Mpg123StreamError::mpg123_init_failed};

    auto parsed = parse_url(url);
    if (!parsed) {
        log::error("[mpg123] failed to parse URL: {}", url);
        return std::unexpected{Mpg123StreamError::http_connect_failed};
    }

    auto stream = std::unique_ptr<Mpg123Stream>(new Mpg123Stream());

    // ── Create mpg123 handle ──
    int err = 0;
    stream->mh_ = mpg123_new(nullptr, &err);
    if (!stream->mh_) {
        log::error("[mpg123] mpg123_new() failed: {}", mpg123_plain_strerror(err));
        return std::unexpected{Mpg123StreamError::handle_creation_failed};
    }

    // Configure for low-latency streaming:
    // - QUIET: suppress console output
    // - FORCE_STEREO: always output 2 channels (mono streams get duplicated)
    mpg123_param2(MH(stream->mh_), MPG123_ADD_FLAGS, MPG123_QUIET, 0.0);
    mpg123_param2(MH(stream->mh_), MPG123_ADD_FLAGS, MPG123_FORCE_STEREO, 0.0);

    // Only allow 48kHz signed-16-bit output — this is what the ring buffer
    // and FMOD DSP expect.  mpg123 will resample if the source is different.
    mpg123_format_none(MH(stream->mh_));
    if (mpg123_format(MH(stream->mh_), 48000, MPG123_STEREO, MPG123_ENC_SIGNED_16) != MPG123_OK) {
        log::error("[mpg123] failed to set output format");
        mpg123_delete(MH(stream->mh_));
        stream->mh_ = nullptr;
        return std::unexpected{Mpg123StreamError::handle_creation_failed};
    }

    if (mpg123_open_feed(MH(stream->mh_)) != MPG123_OK) {
        log::error("[mpg123] mpg123_open_feed() failed: {}", mpg123_strerror(MH(stream->mh_)));
        mpg123_delete(MH(stream->mh_));
        stream->mh_ = nullptr;
        return std::unexpected{Mpg123StreamError::open_feed_failed};
    }

    // ── WinHTTP connect ──
    HINTERNET session = winhttp_session
        ? static_cast<HINTERNET>(winhttp_session)
        : WinHttpOpen(L"forza-radio-dx/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!session) {
        log::error("[mpg123] WinHttpOpen failed ({})", GetLastError());
        mpg123_delete(MH(stream->mh_));
        stream->mh_ = nullptr;
        return std::unexpected{Mpg123StreamError::http_connect_failed};
    }

    // Short timeouts: connect 3s, send 3s, receive 10s (longer for streaming).
    DWORD connect_timeout = 3000;
    DWORD send_timeout    = 3000;
    DWORD receive_timeout = 10000;
    WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_TIMEOUT,
                     &connect_timeout, sizeof(connect_timeout));
    WinHttpSetOption(session, WINHTTP_OPTION_SEND_TIMEOUT,
                     &send_timeout, sizeof(send_timeout));
    WinHttpSetOption(session, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                     &receive_timeout, sizeof(receive_timeout));

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(),
                                        parsed->port, 0);
    if (!connect) {
        log::error("[mpg123] WinHttpConnect failed ({})", GetLastError());
        if (!winhttp_session) WinHttpCloseHandle(session);
        mpg123_delete(MH(stream->mh_));
        stream->mh_ = nullptr;
        return std::unexpected{Mpg123StreamError::http_connect_failed};
    }

    DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE;
    if (parsed->https) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", parsed->path.c_str(),
        L"HTTP/1.1", WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        log::error("[mpg123] WinHttpOpenRequest failed ({})", GetLastError());
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        mpg123_delete(MH(stream->mh_));
        stream->mh_ = nullptr;
        return std::unexpected{Mpg123StreamError::http_request_failed};
    }

    // Request headers: ask for audio, don't request ICY metadata (we use
    // REST polling for track info instead).
    const wchar_t* headers =
        L"User-Agent: WinampMPEG/5.0\r\n"
        L"Accept: audio/*\r\n";
    if (!WinHttpAddRequestHeaders(request, headers, static_cast<DWORD>(-1),
                                   WINHTTP_ADDREQ_FLAG_ADD)) {
        log::warn("[mpg123] WinHttpAddRequestHeaders failed ({})", GetLastError());
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        log::error("[mpg123] WinHttpSendRequest failed ({})", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        mpg123_delete(MH(stream->mh_));
        stream->mh_ = nullptr;
        return std::unexpected{Mpg123StreamError::http_request_failed};
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        log::error("[mpg123] WinHttpReceiveResponse failed ({})", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        mpg123_delete(MH(stream->mh_));
        stream->mh_ = nullptr;
        return std::unexpected{Mpg123StreamError::http_response_failed};
    }

    // Check HTTP status code
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                             &status_size, WINHTTP_NO_HEADER_INDEX)) {
        if (status_code != 200) {
            log::error("[mpg123] HTTP {} for {}", status_code, url);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            if (!winhttp_session) WinHttpCloseHandle(session);
            mpg123_delete(MH(stream->mh_));
            stream->mh_ = nullptr;
            return std::unexpected{Mpg123StreamError::http_response_failed};
        }
    }

    stream->h_connect_ = connect;
    stream->h_request_ = request;
    stream->raw_buf_.resize(64 * 1024); // 64KB read buffer

    log::info("[mpg123] stream connected: {}", url);
    return stream;
}

Mpg123Stream::~Mpg123Stream() {
    if (mh_) {
        mpg123_close(MH(mh_));
        mpg123_delete(MH(mh_));
    }
    if (h_request_)  WinHttpCloseHandle(static_cast<HINTERNET>(h_request_));
    if (h_connect_)  WinHttpCloseHandle(static_cast<HINTERNET>(h_connect_));
}

std::size_t Mpg123Stream::read_pcm(std::span<std::byte> out, bool& ended) {
    ended = false;
    if (!mh_ || !h_request_) {
        ended = true;
        return 0;
    }

    std::size_t total_written = 0;
    auto* request = static_cast<HINTERNET>(h_request_);

    // Loop: read from HTTP → feed to mpg123 → read decoded PCM → write to out.
    // We keep looping until either:
    //   - out is full
    //   - no more HTTP data available right now
    //   - mpg123 needs more data but HTTP has none
    for (int iteration = 0; iteration < 4; ++iteration) {
        // ── Read decoded PCM from mpg123 (from previously fed data) ──
        while (total_written < out.size()) {
            std::size_t remaining = out.size() - total_written;
            size_t got = 0;
            int ret = mpg123_read(MH(mh_),
                reinterpret_cast<unsigned char*>(out.data() + total_written),
                remaining, &got);

            if (got > 0) {
                total_written += got;
                bytes_decoded_ += got;
                if (!format_known_) {
                    long rate = 0;
                    int channels = 0, encoding = 0;
                    if (mpg123_getformat(MH(mh_), &rate, &channels, &encoding) == MPG123_OK) {
                        log::info("[mpg123] format: {}Hz {}ch enc={}", rate, channels, encoding);
                        format_known_ = true;
                    }
                }
            }

            if (ret == MPG123_DONE) {
                ended = true;
                return total_written;
            }
            if (ret == MPG123_NEED_MORE) {
                break; // need to feed more data
            }
            if (ret != MPG123_OK && ret != MPG123_NEW_FORMAT) {
                log::warn("[mpg123] decode error: {}", mpg123_strerror(MH(mh_)));
                ended = true;
                return total_written;
            }
            // MPG123_OK or MPG123_NEW_FORMAT — continue reading
        }

        if (total_written >= out.size()) break; // out buffer is full

        // ── Read raw data from WinHTTP ──
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            log::warn("[mpg123] WinHttpQueryDataAvailable failed ({})", GetLastError());
            ended = true;
            return total_written;
        }

        if (avail == 0) {
            // No data available right now — check if the response is complete
            // (WinHttpQueryDataAvailable returns 0 at EOF too, but we can't
            // distinguish "no data yet" from "EOF" without trying to read).
            // We'll just return what we have; the caller will pump again.
            break;
        }

        // Read up to raw_buf_.size() or avail, whichever is smaller
        DWORD to_read = std::min<DWORD>(static_cast<DWORD>(raw_buf_.size()), avail);
        DWORD got = 0;
        if (!WinHttpReadData(request, raw_buf_.data(), to_read, &got) || got == 0) {
            log::warn("[mpg123] WinHttpReadData failed ({})", GetLastError());
            ended = true;
            return total_written;
        }

        // Feed raw data to mpg123
        int feed_ret = mpg123_feed(MH(mh_),
            reinterpret_cast<unsigned char*>(raw_buf_.data()), got);
        if (feed_ret != MPG123_OK && feed_ret != MPG123_DONE) {
            log::warn("[mpg123] mpg123_feed error: {}", mpg123_strerror(MH(mh_)));
            ended = true;
            return total_written;
        }

        // Loop back to read decoded PCM from the newly fed data
    }

    return total_written;
}

bool Mpg123Stream::is_alive() const noexcept {
    return mh_ != nullptr && h_request_ != nullptr;
}

} // namespace frdx::sources
