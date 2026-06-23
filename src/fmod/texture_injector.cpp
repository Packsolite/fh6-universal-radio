#include "fh6/fmod/texture_injector.hpp"
#include "fh6/net/http_get.hpp"
#include "fh6/log.hpp"
#include <thread>
#include <fstream>
#include <filesystem>
#include <windows.h>

namespace fh6 {

void TextureInjector::update_artwork_url(const std::string& url) {
    is_processing_.store(true);
    
    // grab a unique ticket for this job
    uint64_t my_job_id = ++latest_job_id_;

    std::thread([this, url, my_job_id]() {
        // only clear the processing flag if this thread is still the newest job
        struct ProcessingGuard {
            std::atomic<bool>& flag;
            std::atomic<uint64_t>& latest;
            uint64_t mine;
            ~ProcessingGuard() { 
                if (latest.load() == mine) {
                    flag.store(false); 
                }
            }
        } guard{is_processing_, latest_job_id_, my_job_id};

        // if user already skipped the song, abort immediately
        if (latest_job_id_.load() != my_job_id) return; 

        char dll_path[MAX_PATH];
        GetModuleFileNameA(nullptr, dll_path, MAX_PATH);
        std::filesystem::path game_dir = std::filesystem::path(dll_path).parent_path();
        
        std::filesystem::path temp_dir_path = std::filesystem::temp_directory_path();
        
        std::string raw_path = (temp_dir_path / ("fh6_raw_" + std::to_string(my_job_id))).string(); 
        std::string png_path = (temp_dir_path / ("fh6_png_" + std::to_string(my_job_id) + ".png")).string();
        std::string dds_path = (temp_dir_path / ("fh6_png_" + std::to_string(my_job_id) + ".dds")).string(); 
        std::string temp_dir_str = temp_dir_path.string();

        if (!temp_dir_str.empty() && (temp_dir_str.back() == '\\' || temp_dir_str.back() == '/')) {
            temp_dir_str.pop_back();
        }

        struct FileCleanupGuard {
            std::string r, p, d;
            ~FileCleanupGuard() {
                std::error_code ec;
                if (!r.empty()) std::filesystem::remove(r, ec);
                if (!p.empty()) std::filesystem::remove(p, ec);
                if (!d.empty()) std::filesystem::remove(d, ec);
            }
        } file_guard{raw_path, png_path, dds_path};

        bool has_valid_source = false;

        if (!url.empty()) {
            log::info("[dx12] job {}: delegating artwork download to worker process: {}", my_job_id, url);
            
            if (worker_) {
                // IPC call blocks until the worker finishes downloading to raw_path
                has_valid_source = worker_->download_file(url, raw_path);
                
                if (!has_valid_source) {
                    log::warn("[dx12] job {}: worker failed to download artwork - falling back to default", my_job_id);
                } else {
                    log::info("[dx12] job {}: worker successfully downloaded artwork", my_job_id);
                }
            } else {
                log::error("[dx12] job {}: WorkerClient is not attached to TextureInjector - falling back", my_job_id);
            }
        }

        if (latest_job_id_.load() != my_job_id) return;

        if (!has_valid_source) {
            std::filesystem::path default_art = game_dir / "fh6-radio" / "assets" / "default_artwork.png";
            if (std::filesystem::exists(default_art)) {
                std::error_code ec;
                std::filesystem::copy_file(default_art, raw_path, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) {
                    has_valid_source = true;
                } else {
                    log::warn("[dx12] job {}: failed to copy default artwork", my_job_id);
                }
            } else {
                log::warn("[dx12] job {}: no default artwork found at {} - skipping texture update", my_job_id, default_art.string());
                return; 
            }
        }

        // don't saturate the CPU with FFmpeg if user skipped
        if (latest_job_id_.load() != my_job_id) return; 

        std::string ffmpeg_path = (game_dir / "fh6-radio" / "bin" / "ffmpeg.exe").string();
        std::string texconv_path = (game_dir / "fh6-radio" / "bin" / "texconv.exe").string();

        log::info("[dx12] job {}: running image pipeline...", my_job_id);

        std::string combined_cmd = "cmd.exe /c \"\"" + ffmpeg_path + "\" -y -v error -i \"" + raw_path + "\" -filter_complex \"[0:v]scale=196:104:force_original_aspect_ratio=decrease,pad=196:104:0:(oh-ih)/2:color=black@0\" -pix_fmt rgba -frames:v 1 \"" + png_path + "\" && \"" + texconv_path + "\" -f BC7_UNORM -w 196 -h 104 -m 1 -pmalpha -gpu 0 -y -o \"" + temp_dir_str + "\" \"" + png_path + "\"\"";
        std::vector<char> cmd_buf(combined_cmd.begin(), combined_cmd.end());
        cmd_buf.push_back('\0');
        
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        
        if (CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            log::warn("[dx12] job {}: failed to launch pipeline", my_job_id);
            return;
        }

        // don't lock the mutex and push pixels if stale
        if (latest_job_id_.load() != my_job_id) return; 

        std::ifstream in_dds(dds_path, std::ios::binary | std::ios::ate);
        if (in_dds) {
            std::streamsize dds_size = in_dds.tellg();
            if (dds_size > 128) { 
                in_dds.seekg(0, std::ios::beg);
                std::vector<char> dds_data(dds_size);
                if (in_dds.read(dds_data.data(), dds_size)) {
                    size_t header_size = 128;
                    uint32_t fourcc;
                    std::memcpy(&fourcc, dds_data.data() + 84, 4);
                    if (fourcc == 0x30315844) { header_size = 148; }

                    if (static_cast<size_t>(dds_size) > header_size) {
                        size_t payload_size = dds_size - header_size;
                        std::vector<uint8_t> bc7_payload(payload_size);
                        std::memcpy(bc7_payload.data(), dds_data.data() + header_size, payload_size);

                        std::lock_guard<std::mutex> lock(mtx_);
                        width_ = 196; 
                        height_ = 104;
                        pending_pixels_ = std::move(bc7_payload); 
                        has_new_image_ = true;
                        log::info("[dx12] job {} complete", my_job_id);
                    }
                }
            }
            in_dds.close();
        } else {
            log::warn("[dx12] job {}: failed to read DDS", my_job_id);
        }
    }).detach();
}

bool TextureInjector::pop_pending_pixels(std::vector<uint8_t>& out_pixels, int& out_width, int& out_height) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!has_new_image_) return false;

    out_pixels = std::move(pending_pixels_);
    out_width = width_;
    out_height = height_;
    has_new_image_ = false;
    return true;
}

} // namespace fh6