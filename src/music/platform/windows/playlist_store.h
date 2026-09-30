#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "audio_backend.h"

namespace pcyoutube::windows {

struct Playlist {
    std::string name;
    std::vector<SearchTrack> tracks;
};

struct AppSettings {
    int volume = 75;
    int quality_index = 0;
    int selected_playlist = -1;
    bool shuffle = false;
    int repeat_mode = 0;  // 0=off, 1=all, 2=one
};

class PlaylistStore {
public:
    PlaylistStore();

    bool load(std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;

    const std::vector<Playlist>& playlists() const noexcept { return playlists_; }
    std::vector<Playlist>& playlists() noexcept { return playlists_; }

    const AppSettings& settings() const noexcept { return settings_; }
    AppSettings& settings() noexcept { return settings_; }

    int create(std::string_view name, std::string* error = nullptr);
    bool remove(std::size_t playlist_index, std::string* error = nullptr);
    bool add_track(std::size_t playlist_index, const SearchTrack& track, std::string* error = nullptr);
    bool remove_track(std::size_t playlist_index, std::size_t track_index, std::string* error = nullptr);
    bool move_track(std::size_t playlist_index, std::size_t from_index, std::size_t to_index,
                    std::string* error = nullptr);

    const std::filesystem::path& storage_path() const noexcept { return storage_path_; }

private:
    std::filesystem::path storage_path_;
    std::vector<Playlist> playlists_;
    AppSettings settings_;
};

}  // namespace pcyoutube::windows
