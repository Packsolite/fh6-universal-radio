#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <memory>
#include <d3d12.h>
#include "fh6/worker/worker_client.hpp"

namespace fh6 {
class TextureInjector {
public:
    static TextureInjector& instance() {
        static TextureInjector inst;
        return inst;
    }

    void update_artwork_url(const std::string& url);
    
    // the DX12 hook will call this to see if there's a new image ready to be converted
    bool pop_pending_pixels(std::vector<uint8_t>& out_pixels, int& out_width, int& out_height);

    // lets the control loop know if currently building a new texture
    bool is_processing() const { return is_processing_.load(); }

    void set_worker_client(std::shared_ptr<worker::WorkerClient> w) { 
        std::lock_guard<std::mutex> lock(mtx_);
        worker_ = std::move(w); 
    }

private:
    std::mutex mtx_;
    std::vector<uint8_t> pending_pixels_;
    int width_ = 0;
    int height_ = 0;
    bool has_new_image_ = false;

    std::atomic<bool> is_processing_{false}; 
    std::atomic<uint64_t> latest_job_id_{0};

    std::shared_ptr<worker::WorkerClient> worker_;
};
} // namespace fh6