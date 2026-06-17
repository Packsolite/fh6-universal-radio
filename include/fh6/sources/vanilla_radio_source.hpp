#pragma once
#include "fh6/audio_source.hpp"

namespace fh6::sources {

class VanillaRadioSource : public IAudioSource {
public:
    std::string_view name() const noexcept override { return "vanilla_radio"; }
    std::string_view display_name() const noexcept override { return "Vanilla Radio"; }
    bool initialize() override { return true; }
    void shutdown() noexcept override {}
    
    void play() override { state_ = PlaybackState::playing; }
    void pause() override { state_ = PlaybackState::paused; }
    void stop() override { state_ = PlaybackState::stopped; }
    
    TrackInfo current_track() const override { return {"Vanilla Radio", "In-game Audio", "", ""}; }
    PlaybackState playback_state() const noexcept override { return state_; }
    AuthState auth_state() const noexcept override { return AuthState::none_required; }
    SourceCapabilities capabilities() const noexcept override { return {}; }
private:
    PlaybackState state_ = PlaybackState::stopped;
};

} // namespace fh6::sources