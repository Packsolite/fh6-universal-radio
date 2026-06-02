#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"
#include "fh6/worker/worker_client.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fh6::sources {

// One immediate subfolder (or drive, when listing the filesystem root) used by
// the dashboard folder browser and the exclusion tree.
struct FsEntry {
    std::string name;
    std::string path;
    bool has_children = false;
};

// List immediate subdirectories of `dir`. An empty `dir` lists drive roots.
std::vector<FsEntry> enumerate_dir(const std::filesystem::path& dir);

class LocalFileSource final : public IAudioSource {
public:
    // index_path: where the tag-grouping metadata cache is persisted.
    LocalFileSource(LocalFilesConfig cfg, std::filesystem::path ffmpeg_path,
                    std::filesystem::path index_path,
                    worker::WorkerClient* worker = nullptr);
    ~LocalFileSource() override;

    std::string_view name() const noexcept override { return "local_files"; }
    std::string_view display_name() const noexcept override { return "Local Files"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    bool skip_next() override;
    bool restart_current() override;

    void pump(RingBuffer& ring) override;

    // Replace the whole config; rescans only when scan-affecting fields of the
    // active station changed. repeat is applied live without a rescan.
    void set_config(LocalFilesConfig cfg);
    // Switch the on-air preset by name (rescans + restarts playback).
    void set_active_station(std::string name);
    void reshuffle();
    bool jump_to(std::size_t index);
    void set_ffmpeg_path(std::filesystem::path p);
    void set_playback_options(const PlaybackConfig& opts) override;

    struct QueueEntry {
        std::size_t index = 0;
        std::string title;  // metadata title when indexed, else file stem
        std::string artist; // metadata artist when indexed
        std::string folder; // parent folder name
    };
    struct QueueSnapshot {
        std::size_t cursor = 0;
        std::vector<QueueEntry> entries;
    };
    QueueSnapshot queue_snapshot() const;

    TrackInfo current_track() const override;
    std::optional<ArtworkImage> artwork() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    AuthState auth_state() const noexcept override;
    std::string auth_instructions() const override;
    std::size_t track_count() const noexcept;
    std::size_t station_count() const noexcept;
    // Bumps each time a background metadata-index pass finishes, so the UI can
    // refresh the queue once real titles/artists become available.
    std::uint64_t index_version() const noexcept {
        return index_version_.load(std::memory_order_acquire);
    }

    // Active-station descriptors for /api/sources details.
    std::string active_station_name() const;
    std::string active_order() const;
    std::string active_grouping() const;
    std::string active_repeat() const;

    SourceCapabilities capabilities() const noexcept override { return {true, true, false}; }

private:
    struct Decoder; // pimpl, keeps miniaudio out of the header

    struct TrackMeta {
        std::int64_t mtime = 0;
        std::string album, album_artist, title, artist;
        int track_no = 0, disc_no = 0;
        std::uint64_t duration_ms = 0;
    };

    void rebuild_playlist();
    const LocalStation* active_station_locked() const noexcept;
    bool is_shuffle_locked() const noexcept;
    std::string repeat_mode_locked() const;

    void apply_order_locked(const LocalStation& st);
    void order_album_by_folder_locked();
    void shuffle_in_place_locked(std::vector<std::filesystem::path>& v) const;

    // Forward navigation that ignores repeat (explicit skip/next), and the
    // repeat-aware variant used when a track ends naturally.
    bool advance_forward_locked();
    bool eof_advance_locked();

    // Tag-grouping background index. Probed off the audio path; the playlist is
    // re-sorted in place once metadata lands.
    void stop_tag_thread();
    // Always populates the metadata index in the background; reorders the
    // playlist by tags too when `resort` is set (album order, tag grouping).
    void start_index_locked(std::uint64_t gen, bool resort);
    void index_worker(const std::vector<std::filesystem::path>& paths, const std::wstring& ff,
                      std::uint64_t gen, bool resort);
    void load_index_if_needed();
    void save_index();
    std::vector<std::filesystem::path>
    order_album_by_tags(const std::vector<std::filesystem::path>& in);

    // Open one decoder for playlist_[index] (miniaudio first, ffmpeg fallback).
    // Returns nullptr if the file is unplayable / out of range.
    std::unique_ptr<Decoder> open_decoder_locked(std::size_t index);
    bool open_track_ffmpeg(Decoder& d, const std::filesystem::path& path);
    bool open_track(std::size_t index); // open + install as current
    void close_current();
    void discard_prefetch_locked() noexcept;
    bool promote_prefetch_locked(std::size_t expected_cursor);
    void maybe_spawn_prefetch_locked();
    std::size_t next_cursor_locked() const noexcept;

    LocalFilesConfig cfg_;
    std::filesystem::path ffmpeg_path_;
    std::filesystem::path index_path_;
    worker::WorkerClient* worker_;
    std::vector<std::filesystem::path> playlist_;
    std::size_t cursor_ = 0;
    std::filesystem::path last_played_; // bag-shuffle: avoid an immediate repeat on wrap

    std::unique_ptr<Decoder> dec_;
    std::unique_ptr<Decoder> prefetch_dec_;

    mutable std::mutex mu_;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};
    std::atomic<uint64_t> position_ms_{0};

    // Tag index state. index_mu_ is never held while mu_ is held by the worker
    // (the worker takes mu_ only for the final swap), avoiding lock-order issues.
    mutable std::mutex index_mu_;
    std::unordered_map<std::string, TrackMeta> index_;
    bool index_loaded_ = false;
    std::thread tag_thread_;
    std::atomic<bool> tag_cancel_{false};
    std::atomic<std::uint64_t> rebuild_gen_{0};
    std::atomic<std::uint64_t> index_version_{0};

    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{true};
    std::atomic<bool> prebuffer_next_{true};
};

} // namespace fh6::sources
