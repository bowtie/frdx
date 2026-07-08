#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frdx::sources {

enum class Mpg123StreamError {
    mpg123_init_failed,
    handle_creation_failed,
    open_feed_failed,
    http_connect_failed,
    http_request_failed,
    http_response_failed,
};

// In-process MP3 stream decoder using libmpg123 + WinHTTP.
//
// Replaces the ffmpeg subprocess approach: instead of spawning ffmpeg.exe
// to decode the stream and piping PCM through stdout, we fetch raw bytes
// from the HTTP stream and feed them directly to libmpg123 in-process.
//
// This eliminates:
//   - subprocess creation/teardown overhead
//   - pipe syscall latency (context switches between processes)
//   - extra memcpy through the pipe buffer
//   - dependency on ffmpeg.exe being present on disk
//
// Usage:
//   auto stream = Mpg123Stream::create(url);
//   if (stream) {
//       while (playing) {
//           auto pcm = stream->pump();
//           // write pcm to ring buffer
//       }
//   }
class Mpg123Stream {
public:
    // Creates and connects a new stream.  Performs the HTTP handshake
    // (connect + send request + receive response headers) synchronously.
    static std::expected<std::unique_ptr<Mpg123Stream>, Mpg123StreamError>
    create(const std::string& url, void* winhttp_session);

    ~Mpg123Stream();

    Mpg123Stream(const Mpg123Stream&)            = delete;
    Mpg123Stream& operator=(const Mpg123Stream&) = delete;
    Mpg123Stream(Mpg123Stream&&)                 = delete;
    Mpg123Stream& operator=(Mpg123Stream&&)      = delete;

    // Reads decoded PCM data (s16le, 48kHz, stereo) into the provided buffer.
    // Returns the number of bytes written.  Returns 0 if no data is currently
    // available (the caller should try again later).  Sets `ended` to true
    // if the stream has ended (EOF or error).
    //
    // This is non-blocking: it reads whatever is currently available from
    // the HTTP connection, feeds it to mpg123, and returns whatever decoded
    // PCM is ready.  It will NOT block waiting for network data.
    std::size_t read_pcm(std::span<std::byte> out, bool& ended);

    // Returns true if the HTTP connection is still alive.
    bool is_alive() const noexcept;

private:
    Mpg123Stream() = default;

    // mpg123 handle (opaque)
    void* mh_ = nullptr;

    // WinHTTP handles (opaque HINTERNET)
    void* h_session_  = nullptr;
    void* h_connect_ = nullptr;
    void* h_request_  = nullptr;

    // Raw byte buffer for feeding mpg123 (read from WinHTTP)
    std::vector<std::byte> raw_buf_;

    // True once mpg123 has determined the stream format
    bool format_known_ = false;

    // Total bytes of PCM decoded so far
    std::uint64_t bytes_decoded_ = 0;
};

} // namespace frdx::sources
