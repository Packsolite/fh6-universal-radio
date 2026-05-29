#pragma once

#include "fh6/audio_source.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

namespace fh6::sources {

struct ExternalAudioDevice {
 std::string id;
 std::string name;
 bool is_default = false;
};

std::vector<ExternalAudioDevice> enumerate_external_audio_devices();
std::string external_audio_configured_endpoint();
bool set_external_audio_configured_endpoint(std::string_view endpoint_id);

class ExternalAudioSource final : public IAudioSource {
public:
 ExternalAudioSource() = default;
 ~ExternalAudioSource() override;

 std::string_view name() const noexcept override { return "external_audio"; }
 std::string_view display_name() const noexcept override { return "External Audio"; }

 bool initialize() override;
 void shutdown() noexcept override;

 void play() override;
 void pause() override;
 void stop() override;
 void pump(RingBuffer& ring) override;
 void reload_from_config();

 TrackInfo current_track() const override;
 PlaybackState playback_state() const noexcept override {
  return state_.load(std::memory_order_acquire);
 }
 AuthState auth_state() const noexcept override;
 std::string auth_instructions() const override;
 SourceCapabilities capabilities() const noexcept override { return {}; }

private:
 void start_worker();
 void stop_worker() noexcept;
 void capture_loop() noexcept;

 void append_pcm(const int16_t* data, std::size_t samples);
 void clear_queue();

 mutable std::mutex meta_mu_;
 std::string device_name_ = "Default playback device";
 std::string last_error_;

 std::mutex queue_mu_;
 std::vector<int16_t> pcm_queue_;
 std::size_t queue_offset_ = 0;

 std::mutex worker_mu_;
 std::thread worker_;
 std::atomic<bool> stop_requested_{false};

 std::atomic<PlaybackState> state_{PlaybackState::stopped};
 std::atomic<uint64_t> position_ms_{0};
};

} // namespace fh6::sources
