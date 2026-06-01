#include "fh6/sources/spotify_source.hpp"
#include "fh6/sources/external_media_session.hpp"
#include "fh6/log.hpp"
#include "fh6/net/http_get.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace fh6::sources {

namespace {

// 48 kHz * 2 ch * 2 bytes = 192 000 B/s, so PCM position maps to time at 192 B/ms.
constexpr uint64_t    kBytesPerMs          = 192;
constexpr std::size_t kMaxBufferBytes      = 28800;  // 150 ms of in-flight PCM
constexpr std::size_t kPipeChunk           = 4096;   // OS-minimum pipe / read granularity
// A track-load event arriving while the previous track is still this far from
// its end means the user skipped inside the Spotify app -- adopt it at once.
constexpr uint64_t    kExternalSkipGuardMs = 32000;

std::filesystem::path stderr_log_path() {
    return std::filesystem::temp_directory_path() / "fh6-spotify-stderr.log";
}

// Press-and-release one extended media key (next/prev fallback).
void send_media_key(WORD vk) {
    INPUT ip[2]      = {};
    ip[0].type       = ip[1].type = INPUT_KEYBOARD;
    ip[0].ki.wVk     = ip[1].ki.wVk = vk;
    ip[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    ip[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    SendInput(2, ip, sizeof(INPUT));
}

std::string spotify_uri_to_url(std::string uri) {
    if (uri.rfind("spotify:", 0) == 0) uri.erase(0, 8);
    std::replace(uri.begin(), uri.end(), ':', '/');
    return "https://open.spotify.com/" + uri;   // spotify:track:ID -> .../track/ID
}

// Resolve a track's cover via Spotify's public oEmbed endpoint (no auth).
std::optional<std::string> resolve_cover(const std::string& uri) {
    auto body = net::http_get("https://open.spotify.com/oembed?url=" + spotify_uri_to_url(uri));
    if (!body) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(*body);
        if (auto it = j.find("thumbnail_url"); it != j.end() && it->is_string())
            return it->get<std::string>();
    } catch (...) {}
    return std::nullopt;
}

// CreateProcess hands one string to the child via GetCommandLineW
std::wstring quote(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    // CommandLineToArgvW rules: a run of backslashes is literal unless it
    // precedes a '"' (or the closing quote), in which case each backslash must
    // be doubled -- otherwise a trailing backslash escapes our closing quote.
    std::wstring out{L"\""};
    for (std::size_t i = 0; i < s.size();) {
        std::size_t bs = 0;
        while (i < s.size() && s[i] == L'\\') { ++bs; ++i; }
        if (i == s.size())      out.append(bs * 2, L'\\');
        else if (s[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out += s[i++]; }
        else                   { out.append(bs, L'\\');         out += s[i++]; }
    }
    out += L'"';
    return out;
}

// NUL is Windows' /dev/null
HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

// tee stderr to temp folder for debugging
HANDLE open_stderr_log() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(stderr_log_path().wstring().c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? open_nul(GENERIC_WRITE) : h;
}

// Job Object with KILL_ON_JOB_CLOSE
HANDLE create_kill_on_close_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

// spawn under job
HANDLE spawn_in_job(HANDLE job, const std::wstring& cmd, HANDLE stdin_h, HANDLE stdout_h,
                    HANDLE stderr_h) {
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_h;
    si.hStdOutput = stdout_h;
    si.hStdError  = stderr_h;

    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return nullptr;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

} // namespace

struct SpotifySource::Pipe {
    HANDLE job         = nullptr;
    HANDLE proc_spot   = nullptr;
    HANDLE proc_ff     = nullptr;
    HANDLE read_pipe   = nullptr;
    // log parsing
    HANDLE err_pipe    = nullptr; 
    HANDLE log_file    = nullptr;
    std::string err_buf;
    bool ended         = false;

    // tracks the total PCM bytes pumped to calculate UI time syncing
    uint64_t bytes_consumed = 0;

    // gapless & prefetch tracking
    std::string loading_uri;   // Spotify URI from the most recent "Loading <...>" line
    std::string pending_uri;   // URI paired with pending_title
    std::string pending_title;
    uint64_t pending_duration_ms = 0;
    uint64_t track_duration_ms = 0;
    bool has_pending = false;
    int stall_ticks = 0;
    bool force_next_metadata = false;
    bool awaiting_first_track = true; // no real track adopted yet

    ~Pipe() {
        if (read_pipe)  CloseHandle(read_pipe);
        if (err_pipe)   CloseHandle(err_pipe);
        if (log_file && log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        if (job)        CloseHandle(job);
        if (proc_spot)  CloseHandle(proc_spot);
        if (proc_ff)    CloseHandle(proc_ff);
    }
};

SpotifySource::SpotifySource(SpotifyConfig cfg, std::filesystem::path ffmpeg_path) : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)} {
    info_.title = "Ready to Cast";
    info_.artist = "Spotify Connect";
}

SpotifySource::~SpotifySource() {
    stop_art_worker();
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool SpotifySource::initialize() {
    if (!cfg_.enabled) return false;

    // librespot stores credentials here; a cache dir we can't create means
    // auth can never persist, so fail fast rather than run half-configured.
    std::error_code ec;
    std::filesystem::create_directories(cfg_.cache_dir, ec);
    if (ec) {
        log::warn("[spotify] cannot create cache dir {} ({})", cfg_.cache_dir.string(),
                  ec.message());
        return false;
    }
    start_art_worker();
    return true;
}

void SpotifySource::start_art_worker() {
    if (art_thr_.joinable()) return;
    art_stop_ = false;
    art_thr_ = std::thread{[this] { artwork_worker(); }};
}

void SpotifySource::stop_art_worker() noexcept {
    {
        std::scoped_lock lk{art_mu_};
        art_stop_ = true;
    }
    art_cv_.notify_all();
    if (art_thr_.joinable()) art_thr_.join();
}

void SpotifySource::request_artwork_locked(const std::string& uri) {
    info_.artwork_url.clear();
    info_uri_ = uri;
    if (uri.empty()) return;
    {
        std::scoped_lock lk{art_mu_};
        art_req_uri_ = uri;
    }
    art_cv_.notify_one();
}

void SpotifySource::artwork_worker() {
    for (;;) {
        std::string uri;
        {
            std::unique_lock lk{art_mu_};
            art_cv_.wait(lk, [this] { return art_stop_ || !art_req_uri_.empty(); });
            if (art_stop_) return;
            uri = std::exchange(art_req_uri_, {});
        }
        auto url = resolve_cover(uri);   // network, no locks held
        if (!url) continue;
        std::scoped_lock lk{mu_};
        if (info_uri_ == uri) info_.artwork_url = std::move(*url);
    }
}

void SpotifySource::set_config(SpotifyConfig cfg, std::filesystem::path ffmpeg_path) {
    // Stored only; a live pipe keeps its current paths and picks the new ones up
    // on the next start. Restarting here would cut playback on unrelated config
    // saves (the dashboard fires on_change for any field change).
    std::scoped_lock lk{mu_};
    cfg_         = std::move(cfg);
    ffmpeg_path_ = std::move(ffmpeg_path);
}

void SpotifySource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

bool SpotifySource::cache_exists() const {
    auto creds_path = cfg_.cache_dir / "credentials.json";
    std::error_code ec;
    return std::filesystem::exists(creds_path, ec); // noexcept overload: auth_state() is noexcept
}

AuthState SpotifySource::auth_state() const noexcept {
    return cache_exists() ? AuthState::authenticated : AuthState::needs_auth;
}

std::string SpotifySource::auth_instructions() const {
    return "1. Ensure your PC and phone are on the same Wi-Fi network.\n"
           "2. Open the Spotify app on your phone.\n"
           "3. Tap the 'Devices' icon and select 'FH6 Universal Radio'.\n"
           "Once connected, credentials will automatically save to the cache folder.";
}

void SpotifySource::start_pipe_locked() {
    stop_pipe_locked();

    auto pipe = std::make_unique<Pipe>();
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[spotify] CreateJobObject failed ({})", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE spot_out_r = nullptr, spot_out_w = nullptr;
    HANDLE spot_err_r = nullptr, spot_err_w = nullptr;
    HANDLE ff_out_r = nullptr, ff_out_w = nullptr;

    auto bail = [&] {
        if (spot_out_r) CloseHandle(spot_out_r);
        if (spot_out_w) CloseHandle(spot_out_w);
        if (spot_err_r) CloseHandle(spot_err_r);
        if (spot_err_w) CloseHandle(spot_err_w);
        if (ff_out_r) CloseHandle(ff_out_r);
        if (ff_out_w) CloseHandle(ff_out_w);
    };

    // Pipes are created inheritable (sa) but every end is cleared below except
    // the three librespot inherits (its stdin/stdout/stderr). librespot is
    // spawned first; if ffmpeg's stdout write end (ff_out_w) leaked into it, the
    // read end would never see EOF when ffmpeg exits. The ffmpeg-side ends are
    // re-enabled just before ffmpeg is spawned.
    // reduced pipe size to 4KB (OS minimum) to eliminate residual data backlog
    if (!CreatePipe(&spot_out_r, &spot_out_w, &sa, 4096)) { bail(); return; }
    SetHandleInformation(spot_out_r, HANDLE_FLAG_INHERIT, 0);
    // create error pipe and ensure read end isn't passed to children
    if (!CreatePipe(&spot_err_r, &spot_err_w, &sa, 4096)) { bail(); return; }
    SetHandleInformation(spot_err_r, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&ff_out_r, &ff_out_w, &sa, 4096)) { bail(); return; }
    SetHandleInformation(ff_out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ff_out_w, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    pipe->log_file = open_stderr_log(); // keep the log file open in our Pipe struct
    if (pipe->log_file && pipe->log_file != INVALID_HANDLE_VALUE)
        SetHandleInformation(pipe->log_file, HANDLE_FLAG_INHERIT, 0); // ffmpeg only, not librespot

    const auto spot = cfg_.librespot_path.empty() ? L"librespot" : cfg_.librespot_path.wstring();
    const std::wstring ff = ffmpeg_path_.empty() ? std::wstring{L"ffmpeg"}
                                                 : ffmpeg_path_.wstring();
    const auto cache = cfg_.cache_dir.wstring();

    std::wstring spot_cmd = quote(spot) + 
                            L" --name \"FH6 Universal Radio\"" +
                            L" --bitrate 320" +
                            L" --backend pipe" +
                            L" --initial-volume 100" +
                            L" --cache " + quote(cache);

    // librespot defaults to 44100Hz s16le. We must resample to 48000Hz for FH6.
    // added flags to disable FFmpeg internal buffering for perfect UI sync
    std::wstring ff_cmd = quote(ff) + 
                          L" -loglevel error" +
                          L" -fflags nobuffer -flags low_delay" + 
                          L" -blocksize 4096" + // force micro-block processing
                          L" -f s16le -ar 44100 -ac 2 -i pipe:0" +
                          L" -flush_packets 1" + 
                          L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    // request debug logs for the player module to intercept Seek events
    SetEnvironmentVariableW(L"RUST_LOG", L"librespot_playback::player=debug");

    // pass spot_err_w to librespot instead of the raw file
    pipe->proc_spot = spawn_in_job(pipe->job, spot_cmd, nul_in, spot_out_w, spot_err_w);
    SetEnvironmentVariableW(L"RUST_LOG", nullptr);
    // librespot owns its inherited stdin/stdout/stderr now; drop the parent copies.
    CloseHandle(spot_out_w); spot_out_w = nullptr;
    CloseHandle(spot_err_w); spot_err_w = nullptr;
    if (nul_in) { CloseHandle(nul_in); nul_in = nullptr; }

    if (!pipe->proc_spot) {
        log::warn("[spotify] failed to launch librespot ({}) -- check {}", GetLastError(),
                  stderr_log_path().string());
        bail();
        return;
    }

    // librespot has inherited only its own std handles; now expose the
    // ffmpeg-side ends so ffmpeg (and only ffmpeg) inherits them.
    SetHandleInformation(spot_out_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(ff_out_w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (pipe->log_file && pipe->log_file != INVALID_HANDLE_VALUE)
        SetHandleInformation(pipe->log_file, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    // FFmpeg can keep logging to the file directly
    pipe->proc_ff = spawn_in_job(pipe->job, ff_cmd, spot_out_r, ff_out_w, pipe->log_file);
    CloseHandle(spot_out_r); spot_out_r = nullptr;
    CloseHandle(ff_out_w);   ff_out_w = nullptr;

    if (!pipe->proc_ff) {
        log::warn("[spotify] failed to launch ffmpeg ({}) -- check {}", GetLastError(),
                  stderr_log_path().string());
        bail(); // clean up unassigned pipe handles
        return;
    }

    pipe->err_pipe  = spot_err_r; // save our read handle
    pipe->read_pipe = ff_out_r;
    pipe_           = std::move(pipe);

    info_.title = "Streaming via Spotify Connect";
    info_.artist = "Spotify";
    info_.artwork_url.clear();
    info_uri_.clear();
    state_.store(PlaybackState::playing, std::memory_order_release);

    log::info("[spotify] librespot pipe started (listening on network)");
}

void SpotifySource::stop_pipe_locked() {
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void SpotifySource::play() {
    std::scoped_lock lk{mu_};
    // A dead stream (ffmpeg/librespot exited) leaves pipe_ set with ended=true;
    // respawn so the user can recover without an explicit stop() first.
    if (!pipe_ || pipe_->ended) start_pipe_locked();

    state_.store(PlaybackState::playing, std::memory_order_release);
}

void SpotifySource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void SpotifySource::stop() {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
}

void SpotifySource::transport_skip(bool forward) {
    // Prefer background SMTC control; fall back to synthesizing the media key.
    const bool sent = forward ? external_audio_media_session_next("")
                              : external_audio_media_session_previous("");
    if (!sent) send_media_key(forward ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PREV_TRACK);

    std::scoped_lock lk{mu_};
    if (pipe_) {
        pipe_->bytes_consumed      = 0;
        pipe_->has_pending         = false; // clear any queued prefetch
        pipe_->track_duration_ms   = 0;     // don't wait on the old duration
        pipe_->force_next_metadata = true;  // adopt the next track immediately
    }
    info_.position_ms = 0;
}

void SpotifySource::next() { transport_skip(true); }
void SpotifySource::previous() { transport_skip(false); }

TrackInfo SpotifySource::current_track() const {
    std::scoped_lock lk{mu_};
    return info_;
}

void SpotifySource::pump(RingBuffer& ring) {
    // do not return early if paused - keep draining the pipe
    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p || !p->read_pipe || p->ended) return;

    // check if currently playing or paused
    bool is_playing = (state_.load(std::memory_order_acquire) == PlaybackState::playing);

    // non-blocking parse of track metadata
    if (p->err_pipe) {
        DWORD err_avail = 0;
        while (PeekNamedPipe(p->err_pipe, nullptr, 0, nullptr, &err_avail, nullptr) && err_avail > 0) {
            char buf[1024];
            DWORD to_read = std::min<DWORD>(err_avail, (DWORD)sizeof(buf));
            DWORD got = 0;
            if (!ReadFile(p->err_pipe, buf, to_read, &got, nullptr) || got == 0) break;
            
            // tee the output to our log file so you can still check it on disk
            if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(p->log_file, buf, got, &w, nullptr);
            }
            
            p->err_buf.append(buf, got);
            
            // process all complete lines
            size_t pos;
            while ((pos = p->err_buf.find('\n')) != std::string::npos) {
                std::string line = p->err_buf.substr(0, pos);
                p->err_buf.erase(0, pos + 1);
                
                // strip Windows carriage return if it exists
                if (!line.empty() && line.back() == '\r') line.pop_back();

                // Capture the URI from the "Loading <..>" line preceding each
                // track's "loaded" line, to resolve its cover.
                if (auto lp = line.find("] Loading <"); lp != std::string::npos) {
                    const std::string mk = "Spotify URI <";
                    if (auto up = line.find(mk, lp); up != std::string::npos) {
                        const size_t s = up + mk.size();
                        const size_t e = line.find('>', s);
                        if (e != std::string::npos) p->loading_uri = line.substr(s, e - s);
                    }
                    continue;
                }

                // catch seek events from librespot to sync UI timer on scrub
                const std::string seek_marker = "command=Seek(";
                size_t seek_pos = line.find(seek_marker);
                if (seek_pos != std::string::npos) {
                    size_t end = line.find(")", seek_pos);
                    if (end != std::string::npos) {
                        try {
                            uint64_t parsed_seek = std::stoull(line.substr(seek_pos + seek_marker.length(), end - (seek_pos + seek_marker.length())));
                            p->bytes_consumed = parsed_seek * kBytesPerMs;
                        } catch (...) {}
                        continue; // parsed successfully, move to next line
                    }
                }
                
                // look for the track load event signature
                const std::string marker = "librespot_playback::player] <";
                size_t m_pos = line.find(marker);
                if (m_pos != std::string::npos) {
                    size_t start = m_pos + marker.length();
                    size_t end = line.find("> (", start);
                    if (end != std::string::npos) {
                        std::string parsed_title = line.substr(start, end - start);
                        
                        // extract the duration e.g. "> (___ ms) loaded"
                        uint64_t parsed_duration = 0;
                        size_t ms_start = end + 3; 
                        size_t ms_end = line.find(" ms) loaded", ms_start);
                        if (ms_end != std::string::npos) {
                            try {
                                parsed_duration = std::stoull(line.substr(ms_start, ms_end - ms_start));
                            } catch (...) {}
                        }

                        // A load event while we're still >32 s from the current
                        // track's end means a manual in-app skip -- adopt it now.
                        const uint64_t track_bytes = p->track_duration_ms * kBytesPerMs;
                        const bool is_external_skip =
                            p->track_duration_ms > 0 &&
                            p->bytes_consumed + kExternalSkipGuardMs * kBytesPerMs < track_bytes;

                        // First track, an explicit skip, or an in-app skip: apply at once.
                        if (p->awaiting_first_track || p->force_next_metadata || is_external_skip) {
                            info_.title = parsed_title;
                            info_.artist = "Spotify Connect";
                            info_.duration_ms = parsed_duration;
                            p->track_duration_ms = parsed_duration;
                            p->bytes_consumed = 0;
                            p->awaiting_first_track = false;
                            p->force_next_metadata = false;
                            p->has_pending = false;
                            p->stall_ticks = 0;
                            request_artwork_locked(p->loading_uri);
                        } else {
                            // queue it for the gapless transition
                            p->pending_title = parsed_title;
                            p->pending_duration_ms = parsed_duration;
                            p->pending_uri = p->loading_uri;
                            p->has_pending = true;
                        }
                    }
                }
            }
        }
    }

    // Paused: stop draining. The 4 KB pipe fills almost immediately, which
    // back-pressures ffmpeg and librespot -- so playback pauses without desync.
    if (!is_playing) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        p->ended = true;
        return;
    }

   if (avail == 0) {
        p->stall_ticks++;
        // if pipe is dry for just 5 ticks (~80ms)
        if (p->stall_ticks > 5) {
            // the user manually skipped during the final 30 seconds of the song
            if (p->has_pending) {
                info_.title = p->pending_title;
                info_.duration_ms = p->pending_duration_ms;
                p->track_duration_ms = p->pending_duration_ms;

                p->bytes_consumed = 0;
                p->has_pending = false;
                request_artwork_locked(p->pending_uri);
            }
            // catch-all for extreme network lag / dead stream (stall for > 800ms)
            else if (p->stall_ticks > 50) {
                p->force_next_metadata = true;
            }
        }
    } else {
        p->stall_ticks = 0;
    }

    // natural gapless transition
    if (p->has_pending && p->track_duration_ms > 0) {
        uint64_t track_bytes = p->track_duration_ms * kBytesPerMs;
        if (p->bytes_consumed >= track_bytes) {
            info_.title = p->pending_title;
            info_.duration_ms = p->pending_duration_ms;
            p->track_duration_ms = p->pending_duration_ms;

            // carry the remainder so the timer stays exact
            p->bytes_consumed -= track_bytes;
            p->has_pending = false;
            p->stall_ticks = 0;
            request_artwork_locked(p->pending_uri);
        }
    }

    while (avail > 0) {
        const std::size_t buffered = ring.readable();
        if (buffered >= kMaxBufferBytes) break;

        const std::size_t writable = ring.writable();
        if (writable < 4) break;

        std::size_t want = std::min<std::size_t>({static_cast<std::size_t>(avail), writable,
                                                  kMaxBufferBytes - buffered, kPipeChunk});

        std::byte buf[kPipeChunk];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, static_cast<DWORD>(want), &got, nullptr) || got == 0) {
            p->ended = true;
            return;
        }

        ring.write(buf, got);
        p->bytes_consumed += got; // track exact bytes pushed to the stream
        avail = avail > got ? avail - got : 0;
    }

    // UI timer = bytes pushed minus what's still queued ahead in the ring.
    const uint64_t unplayed = ring.readable();
    info_.position_ms =
        p->bytes_consumed > unplayed ? (p->bytes_consumed - unplayed) / kBytesPerMs : 0;
}

} // namespace fh6::sources
