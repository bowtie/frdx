#include "frdx/sources/miniaudio_stream.hpp"
#include "frdx/log.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

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

// ── HTTP data pre-buffer ──
// miniaudio's ma_decoder_init with custom read procs crashes on streaming
// data (it expects blocking file I/O semantics). Instead, we pre-buffer the
// raw HTTP data into a vector, then use ma_decoder_init_memory to decode
// from the buffer. This cleanly separates download time from decode time.

// Pre-fetch raw MP3 data from an HTTP stream until we have target_bytes
// or the stream ends. Returns the fetched data.
static std::vector<std::byte> prefetch_http_data(HINTERNET request, size_t target_bytes) {
    std::vector<std::byte> result;
    result.reserve(target_bytes);
    std::vector<std::byte> buf(64 * 1024);

    while (result.size() < target_bytes) {
        DWORD avail = 0;
        // Wait for data to be available (blocking with timeout)
        for (int wait = 0; wait < 5000; ++wait) {
            if (!WinHttpQueryDataAvailable(request, &avail)) {
                log::warn("[miniaudio] WinHttpQueryDataAvailable failed ({})", GetLastError());
                return result;
            }
            if (avail > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (avail == 0) break;  // timeout or EOF

        DWORD to_read = std::min<DWORD>(static_cast<DWORD>(buf.size()), avail);
        to_read = std::min<DWORD>(to_read, static_cast<DWORD>(target_bytes - result.size()));
        DWORD got = 0;
        if (!WinHttpReadData(request, buf.data(), to_read, &got) || got == 0) break;

        result.insert(result.end(), buf.data(), buf.data() + got);
    }

    return result;
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

    // Pre-fetch enough raw MP3 data for the decoder to work with.
    // 256KB is enough for format detection + several seconds of decoded
    // PCM at typical shoutcast bitrates (128-320kbps).
    constexpr size_t PREFETCH_BYTES = 256 * 1024;
    auto raw_data = prefetch_http_data(request, PREFETCH_BYTES);
    if (raw_data.empty()) {
        log::error("[miniaudio] no data received from stream: {}", url);
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (!winhttp_session) WinHttpCloseHandle(session);
        return std::unexpected{MiniaudioStreamError::decoder_init_failed};
    }

    log::info("[miniaudio] pre-buffered {} bytes from {}", raw_data.size(), url);

    // Initialize decoder from the pre-buffered memory.
    // Store the buffer in the stream so it stays alive for the decoder's
    // lifetime.
    stream->raw_buf_ = std::move(raw_data);

    auto* dec = new ma_decoder{};
    stream->decoder_ = dec;

    ma_decoder_config config = ma_decoder_config_init(
        ma_format_s16, 2, 48000);

    ma_result result = ma_decoder_init_memory(
        stream->raw_buf_.data(), stream->raw_buf_.size(), &config, dec);
    if (result != MA_SUCCESS) {
        log::error("[miniaudio] ma_decoder_init_memory failed: {}", static_cast<int>(result));
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
