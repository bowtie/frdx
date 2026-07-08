#include "frdx/sources/miniaudio_stream.hpp"
#include "frdx/log.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#pragma comment(lib, "winhttp.lib")

namespace frdx::sources {

// Cast helper: decoder_ is stored as void* in the header to avoid
// exposing miniaudio.h to the rest of the codebase.
static ma_decoder* DEC(void* p) { return static_cast<ma_decoder*>(p); }

// ── WinHTTP URL parsing helper (same as mpg123_stream.cpp) ──
struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
};

static std::optional<ParsedUrl> parse_url(const std::string& url) {
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

// ── Custom read callback for HTTP streaming mode ──
// miniaudio calls this to fetch raw bytes. We read from WinHTTP.
struct HttpReadContext {
    HINTERNET request = nullptr;
    std::vector<std::byte>* raw_buf = nullptr;
    bool eof = false;
};

static ma_result ma_http_read_proc(ma_decoder* pDecoder, void* pBufferOut, size_t bytesToRead, size_t* pBytesRead) {
    auto* ctx = static_cast<HttpReadContext*>(pDecoder->pUserData);
    if (!ctx || ctx->eof || !ctx->request) {
        if (pBytesRead) *pBytesRead = 0;
        return MA_AT_END;
    }

    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(ctx->request, &avail)) {
        log::warn("[miniaudio] WinHttpQueryDataAvailable failed ({})", GetLastError());
        ctx->eof = true;
        if (pBytesRead) *pBytesRead = 0;
        return MA_AT_END;
    }

    if (avail == 0) {
        // No data available right now — return 0 bytes read.
        if (pBytesRead) *pBytesRead = 0;
        return MA_SUCCESS;
    }

    DWORD to_read = std::min<DWORD>(static_cast<DWORD>(ctx->raw_buf->size()), avail);
    DWORD got = 0;
    if (!WinHttpReadData(ctx->request, ctx->raw_buf->data(), to_read, &got) || got == 0) {
        log::warn("[miniaudio] WinHttpReadData failed ({})", GetLastError());
        ctx->eof = true;
        if (pBytesRead) *pBytesRead = 0;
        return MA_AT_END;
    }

    size_t copy_size = std::min<size_t>(got, bytesToRead);
    std::memcpy(pBufferOut, ctx->raw_buf->data(), copy_size);
    if (pBytesRead) *pBytesRead = copy_size;
    return MA_SUCCESS;
}

// ── File-based creation (for benchmark) ──
std::expected<std::unique_ptr<MiniaudioStream>, MiniaudioStreamError>
MiniaudioStream::create_from_file(const std::string& path) {
    auto stream = std::unique_ptr<MiniaudioStream>(new MiniaudioStream());
    stream->file_mode_ = true;

    auto* dec = new ma_decoder{};
    stream->decoder_ = dec;

    // Convert path to wide string for miniaudio
    std::wstring wpath(path.begin(), path.end());

    ma_decoder_config config = ma_decoder_config_init(
        ma_format_s16, 2, 48000);  // output: s16, stereo, 48kHz

    ma_result result = ma_decoder_init_file_w(wpath.c_str(), &config, dec);
    if (result != MA_SUCCESS) {
        log::error("[miniaudio] ma_decoder_init_file failed: {}", static_cast<int>(result));
        delete dec;
        stream->decoder_ = nullptr;
        return std::unexpected{MiniaudioStreamError::file_open_failed};
    }

    log::info("[miniaudio] file decoder initialized: {}", path);
    return stream;
}

// ── HTTP streaming creation ──
std::expected<std::unique_ptr<MiniaudioStream>, MiniaudioStreamError>
MiniaudioStream::create(const std::string& url, void* winhttp_session) {
    auto parsed = parse_url(url);
    if (!parsed) {
        log::error("[miniaudio] failed to parse URL: {}", url);
        return std::unexpected{MiniaudioStreamError::init_failed};
    }

    auto stream = std::unique_ptr<MiniaudioStream>(new MiniaudioStream());

    // ── WinHTTP connect (same as mpg123_stream.cpp) ──
    HINTERNET session = winhttp_session
        ? static_cast<HINTERNET>(winhttp_session)
        : WinHttpOpen(L"forza-radio-dx/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!session) {
        log::error("[miniaudio] WinHttpOpen failed ({})", GetLastError());
        return std::unexpected{MiniaudioStreamError::init_failed};
    }

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
        log::error("[miniaudio] WinHttpConnect failed ({})", GetLastError());
        if (!winhttp_session) WinHttpCloseHandle(session);
        return std::unexpected{MiniaudioStreamError::init_failed};
    }

    DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE;
    if (parsed->https) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", parsed->path.c_str(),
        L"HTTP/1.1", WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        log::error("[miniaudio] WinHttpOpenRequest failed ({})", GetLastError());
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        return std::unexpected{MiniaudioStreamError::init_failed};
    }

    const wchar_t* headers =
        L"User-Agent: WinampMPEG/5.0\r\n"
        L"Accept: audio/*\r\n";
    WinHttpAddRequestHeaders(request, headers, static_cast<DWORD>(-1),
                              WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        log::error("[miniaudio] WinHttpSendRequest failed ({})", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        return std::unexpected{MiniaudioStreamError::init_failed};
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        log::error("[miniaudio] WinHttpReceiveResponse failed ({})", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        return std::unexpected{MiniaudioStreamError::init_failed};
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                             &status_size, WINHTTP_NO_HEADER_INDEX)) {
        if (status_code != 200) {
            log::error("[miniaudio] HTTP {} for {}", status_code, url);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            if (!winhttp_session) WinHttpCloseHandle(session);
            return std::unexpected{MiniaudioStreamError::init_failed};
        }
    }

    stream->h_connect_ = connect;
    stream->h_request_ = request;
    stream->raw_buf_.resize(64 * 1024);

    // ── Initialize miniaudio decoder with custom read callback ──
    auto* ctx = new HttpReadContext{};
    ctx->request = request;
    ctx->raw_buf = &stream->raw_buf_;

    auto* dec = new ma_decoder{};
    stream->decoder_ = dec;

    ma_decoder_config config = ma_decoder_config_init(
        ma_format_s16, 2, 48000);

    // Use custom read proc for streaming
    ma_result result = ma_decoder_init(ma_http_read_proc, nullptr, ctx, &config, dec);
    if (result != MA_SUCCESS) {
        log::error("[miniaudio] ma_decoder_init failed: {}", static_cast<int>(result));
        delete ctx;
        delete dec;
        stream->decoder_ = nullptr;
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        return std::unexpected{MiniaudioStreamError::decoder_init_failed};
    }

    log::info("[miniaudio] stream connected: {}", url);
    return stream;
}

MiniaudioStream::~MiniaudioStream() {
    if (decoder_) {
        ma_decoder_uninit(DEC(decoder_));
        // For HTTP mode, the HttpReadContext was stored as pUserData
        if (!file_mode_ && DEC(decoder_)->pUserData) {
            delete static_cast<HttpReadContext*>(DEC(decoder_)->pUserData);
        }
        delete DEC(decoder_);
    }
    if (h_request_) WinHttpCloseHandle(static_cast<HINTERNET>(h_request_));
    if (h_connect_) WinHttpCloseHandle(static_cast<HINTERNET>(h_connect_));
}

std::size_t MiniaudioStream::read_pcm(std::span<std::byte> out, bool& ended) {
    ended = false;
    if (!decoder_) {
        ended = true;
        return 0;
    }

    // miniaudio reads in PCM *frames*, not bytes.  One frame = channels *
    // bytes_per_sample.  For s16 stereo: 1 frame = 4 bytes.
    const ma_uint32 channels = 2;
    const size_t bytes_per_frame = channels * sizeof(int16_t);  // 4
    const ma_uint64 frames_to_read = static_cast<ma_uint64>(out.size() / bytes_per_frame);
    if (frames_to_read == 0) return 0;

    ma_uint64 frames_read = 0;
    ma_result result = ma_decoder_read_pcm_frames(
        DEC(decoder_), out.data(), frames_to_read, &frames_read);

    std::size_t bytes_read = static_cast<std::size_t>(frames_read * bytes_per_frame);
    bytes_decoded_ += bytes_read;

    if (result == MA_AT_END || (result == MA_SUCCESS && frames_read == 0 && file_mode_)) {
        ended = true;
    } else if (result != MA_SUCCESS) {
        log::warn("[miniaudio] decode error: {}", static_cast<int>(result));
        ended = true;
    }

    return bytes_read;
}

bool MiniaudioStream::is_alive() const noexcept {
    return decoder_ != nullptr;
}

} // namespace frdx::sources
