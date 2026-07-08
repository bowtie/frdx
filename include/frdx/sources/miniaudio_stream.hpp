#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace frdx::sources {

enum class MiniaudioStreamError {
    init_failed,
    decoder_init_failed,
    file_open_failed,
};

// In-process MP3 stream decoder using miniaudio + WinHTTP.
//
// Drop-in alternative to Mpg123Stream — same interface, different decoder
// engine.  miniaudio uses its own built-in MP3 decoder (dr_mp3 under the
// hood) so there's no external DLL dependency.
//
// Output format: 48kHz, stereo, signed 16-bit PCM (same as Mpg123Stream).
class MiniaudioStream {
public:
    // Creates and connects a new HTTP stream.  Performs the HTTP handshake
    // synchronously (same as Mpg123Stream::create).
    static std::expected<std::unique_ptr<MiniaudioStream>, MiniaudioStreamError>
    create(const std::string& url, void* winhttp_session);

    // Creates a decoder for a local file (used by the benchmark harness).
    static std::expected<std::unique_ptr<MiniaudioStream>, MiniaudioStreamError>
    create_from_file(const std::string& path);

    ~MiniaudioStream();

    MiniaudioStream(const MiniaudioStream&)            = delete;
    MiniaudioStream& operator=(const MiniaudioStream&) = delete;
    MiniaudioStream(MiniaudioStream&&)                 = delete;
    MiniaudioStream& operator=(MiniaudioStream&&)      = delete;

    // Reads decoded PCM data (s16le, 48kHz, stereo) into the provided buffer.
    // Returns the number of bytes written.  Returns 0 if no data is currently
    // available.  Sets `ended` to true if the stream has ended (EOF or error).
    std::size_t read_pcm(std::span<std::byte> out, bool& ended);

    // Returns true if the stream is still alive.
    bool is_alive() const noexcept;

private:
    MiniaudioStream() = default;

    // miniaudio decoder (opaque — stored as void* to avoid exposing
    // miniaudio.h to the rest of the codebase)
    void* decoder_ = nullptr;

    // WinHTTP handles (opaque HINTERNET) — null for file-based decoding
    void* h_connect_ = nullptr;
    void* h_request_  = nullptr;

    // Raw byte buffer for HTTP reads
    std::vector<std::byte> raw_buf_;

    // Total bytes of PCM decoded so far
    std::uint64_t bytes_decoded_ = 0;

    // True for file-based mode (no HTTP)
    bool file_mode_ = false;
};

} // namespace frdx::sources
