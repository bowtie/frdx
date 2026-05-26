#include "fh6/sources/youtube_music_source.hpp"
#include "fh6/log.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace fh6::sources {

struct YouTubeMusicSource::Pipe {
    HANDLE proc_yt   = nullptr;
    HANDLE proc_ff   = nullptr;
    HANDLE read_pipe = nullptr;
    // Separate yt-dlp --print child; populates info_ once it finishes.
    HANDLE proc_title = nullptr;
    HANDLE title_pipe = nullptr;
    std::string title_buf;
    std::atomic<bool> running{false};

    ~Pipe() {
        running.store(false, std::memory_order_release);
        if (read_pipe) CloseHandle(read_pipe);
        if (title_pipe) CloseHandle(title_pipe);
        if (proc_yt) {
            TerminateProcess(proc_yt, 0);
            CloseHandle(proc_yt);
        }
        if (proc_ff) {
            TerminateProcess(proc_ff, 0);
            CloseHandle(proc_ff);
        }
        if (proc_title) {
            TerminateProcess(proc_title, 0);
            CloseHandle(proc_title);
        }
    }
};

YouTubeMusicSource::YouTubeMusicSource(YouTubeMusicConfig cfg) : cfg_{std::move(cfg)} {}

YouTubeMusicSource::~YouTubeMusicSource() { stop_pipe(); }

bool YouTubeMusicSource::initialize() {
    if (!cfg_.enabled) return false;
    if (!cfg_.default_playlist.empty()) target_url_ = cfg_.default_playlist;
    // Cookies are optional; without them, only public tracks work.
    auth_ = cfg_.cookies_path.empty() ? AuthState::none_required : AuthState::authenticated;
    return true;
}

void YouTubeMusicSource::shutdown() noexcept { stop_pipe(); }

void YouTubeMusicSource::set_target(std::string url) {
    std::scoped_lock lk{mu_};
    target_url_ = std::move(url);
}

namespace {

// CreateProcess hands one string to the child via GetCommandLineW, so any
// argument with whitespace must be double-quoted.
std::wstring quote(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out{L"\""};
    for (auto c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// GetStdHandle returns NULL in a windowed DLL injection; passing NULL with
// STARTF_USESTDHANDLES makes the child's stdio invalid and yt-dlp exits
// before producing audio. NUL is Windows' /dev/null and works as a safe substitute.
HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

// Tee both children's stderr to %TEMP%\fh6-yt-stderr.log so failures (bad
// cookies, geo-block, codec issues) can be diagnosed without a debug build.
// FILE_APPEND_DATA makes per-syscall writes atomic across both processes.
HANDLE open_stderr_log() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    auto path = std::filesystem::temp_directory_path() / "fh6-yt-stderr.log";
    HANDLE h  = CreateFileW(path.wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? open_nul(GENERIC_WRITE) : h;
}

std::filesystem::path stderr_log_path() {
    return std::filesystem::temp_directory_path() / "fh6-yt-stderr.log";
}

HANDLE spawn(const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h, HANDLE stderr_h,
             HANDLE* out_proc) {
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_h;
    si.hStdOutput = stdout_h;
    si.hStdError  = stderr_h;

    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    if (out_proc) {
        *out_proc = pi.hProcess;
    } else {
        CloseHandle(pi.hProcess);
    }
    return pi.hProcess;
}

} // namespace

void YouTubeMusicSource::start_pipe() {
    stop_pipe();
    std::scoped_lock lk{mu_};
    if (target_url_.empty()) return;
    pipe_ = std::make_unique<Pipe>();

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE yt_out_r = nullptr, yt_out_w = nullptr;
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;
    if (!CreatePipe(&yt_out_r, &yt_out_w, &sa, 1 << 20)) {
        pipe_.reset();
        return;
    }
    SetHandleInformation(yt_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 1 << 20)) {
        CloseHandle(yt_out_r);
        CloseHandle(yt_out_w);
        pipe_.reset();
        return;
    }
    SetHandleInformation(ff_out_r, 0, HANDLE_FLAG_INHERIT); // keep parent end private

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const auto yt = cfg_.yt_dlp_path.empty() ? L"yt-dlp" : cfg_.yt_dlp_path.wstring();
    const auto ff = cfg_.ffmpeg_path.empty() ? L"ffmpeg" : cfg_.ffmpeg_path.wstring();

    // `--` terminates options so a URL starting with `-` isn't read as a flag.
    // `--no-playlist` keeps yt-dlp on the single video when the URL has both
    // v= and list= params (playlist iteration is not implemented).
    std::wstring yt_cmd = quote(yt) + L" --no-warnings --no-progress "
                                      L"--format bestaudio --no-playlist -o - ";
    if (!cfg_.cookies_path.empty())
        yt_cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
    yt_cmd += L"-- " + quote(widen(target_url_));

    std::wstring ff_cmd = quote(ff) + L" -loglevel error -i pipe:0 -f s16le "
                                      L"-acodec pcm_s16le -ar 44100 -ac 2 pipe:1";

    if (!spawn(yt_cmd, nul_in, yt_out_w, err_log, &pipe_->proc_yt)) {
        log::warn("[yt] failed to launch yt-dlp ({}) -- check {}", GetLastError(),
                  stderr_log_path().string());
        CloseHandle(yt_out_r);
        CloseHandle(yt_out_w);
        CloseHandle(ff_out_r);
        CloseHandle(ff_out_w);
        if (nul_in) CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        pipe_.reset();
        return;
    }
    CloseHandle(yt_out_w);

    if (!spawn(ff_cmd, yt_out_r, ff_out_w, err_log, &pipe_->proc_ff)) {
        log::warn("[yt] failed to launch ffmpeg ({}) -- check {}", GetLastError(),
                  stderr_log_path().string());
        CloseHandle(yt_out_r);
        CloseHandle(ff_out_r);
        CloseHandle(ff_out_w);
        if (nul_in) CloseHandle(nul_in);
        if (err_log) CloseHandle(err_log);
        TerminateProcess(pipe_->proc_yt, 0);
        pipe_.reset();
        return;
    }
    CloseHandle(yt_out_r);
    CloseHandle(ff_out_w);

    pipe_->read_pipe = ff_out_r;
    pipe_->running.store(true, std::memory_order_release);

    // Separate yt-dlp --print process. pump() reads its stdout and replaces
    // the URL placeholder in info_ once it lands.
    HANDLE tl_out_r = nullptr, tl_out_w = nullptr;
    if (CreatePipe(&tl_out_r, &tl_out_w, &sa, 1 << 16)) {
        SetHandleInformation(tl_out_r, 0, HANDLE_FLAG_INHERIT);
        std::wstring tl_cmd = quote(yt) + L" --skip-download --no-warnings "
                                          L"--no-playlist "
                                          L"--print \"%(title)s\" "
                                          L"--print \"%(uploader)s\" "
                                          L"--print \"%(duration)s\" ";
        if (!cfg_.cookies_path.empty())
            tl_cmd += L"--cookies " + quote(cfg_.cookies_path.wstring()) + L" ";
        tl_cmd += L"-- " + quote(widen(target_url_));
        if (spawn(tl_cmd, nul_in, tl_out_w, err_log, &pipe_->proc_title)) {
            pipe_->title_pipe = tl_out_r;
            CloseHandle(tl_out_w);
        } else {
            CloseHandle(tl_out_r);
            CloseHandle(tl_out_w);
        }
    }

    if (nul_in) CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    info_        = TrackInfo{};
    info_.title  = target_url_; // placeholder until the resolver lands
    info_.artist = "YouTube Music";
    state_.store(PlaybackState::buffering, std::memory_order_release);
    log::info("[yt] pipe started for {} (child stderr -> {})", target_url_,
              stderr_log_path().string());
}

void YouTubeMusicSource::stop_pipe() {
    std::scoped_lock lk{mu_};
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void YouTubeMusicSource::play() {
    if (!pipe_) start_pipe();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void YouTubeMusicSource::pause() { state_.store(PlaybackState::paused, std::memory_order_release); }

void YouTubeMusicSource::stop() { stop_pipe(); }
void YouTubeMusicSource::next() {
    stop_pipe();
    if (!cfg_.default_playlist.empty()) {
        target_url_ = cfg_.default_playlist;
        start_pipe();
        if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

TrackInfo YouTubeMusicSource::current_track() const {
    std::scoped_lock lk{mu_};
    return info_;
}

std::string YouTubeMusicSource::auth_instructions() const {
    return "Export your YouTube cookies to a Netscape cookies.txt and set "
           "[youtube_music].cookies_path in config.toml. Public content works "
           "without cookies.";
}

void YouTubeMusicSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing &&
        state_.load(std::memory_order_acquire) != PlaybackState::buffering)
        return;

    Pipe* p = pipe_.get();
    if (!p || !p->read_pipe) return;

    // yt-dlp --print emits one line per arg (title, uploader, duration). We
    // buffer until the child exits, then parse, so a partial read doesn't
    // drop a line that's only half-arrived.
    if (p->title_pipe) {
        DWORD tavail = 0;
        BOOL ok      = PeekNamedPipe(p->title_pipe, nullptr, 0, nullptr, &tavail, nullptr);
        if (!ok) {
            CloseHandle(p->title_pipe);
            p->title_pipe = nullptr;
        } else if (tavail > 0) {
            char tbuf[1024];
            DWORD got = 0;
            if (ReadFile(p->title_pipe, tbuf, sizeof(tbuf), &got, nullptr) && got > 0)
                p->title_buf.append(tbuf, got);
        } else {
            DWORD exit_code = STILL_ACTIVE;
            if (p->proc_title && GetExitCodeProcess(p->proc_title, &exit_code) &&
                exit_code != STILL_ACTIVE) {
                CloseHandle(p->title_pipe);
                p->title_pipe  = nullptr;
                auto& s        = p->title_buf;
                auto take_line = [&] {
                    auto nl          = s.find('\n');
                    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                        line.pop_back();
                    s.erase(0, nl == std::string::npos ? s.size() : nl + 1);
                    return line;
                };
                std::scoped_lock lk{mu_};
                auto title    = take_line();
                auto uploader = take_line();
                auto duration = take_line();
                if (!title.empty() && title != "NA") info_.title = std::move(title);
                if (!uploader.empty() && uploader != "NA") info_.artist = std::move(uploader);
                try {
                    if (!duration.empty() && duration != "NA")
                        info_.duration_ms = static_cast<uint64_t>(std::stod(duration) * 1000.0);
                } catch (...) {}
            }
        }
    }

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        stop_pipe();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            stop_pipe();
            return;
        }
        ring.write(buf, got);
        avail = avail > got ? avail - got : 0;
        if (state_.load(std::memory_order_acquire) == PlaybackState::buffering &&
            ring.readable() > 32 * 1024)
            state_.store(PlaybackState::playing, std::memory_order_release);
    }
}

} // namespace fh6::sources
