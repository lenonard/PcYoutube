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

class PlaylistStore {
public:
    PlaylistStore();

    bool load(std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;

    const std::vector<Playlist>& playlists() const noexcept { return playlists_; }
    std::vector<Playlist>& playlists() noexcept { return playlists_; }

    int create(std::string_view name, std::string* error = nullptr);
    bool remove(std::size_t playlist_index, std::string* error = nullptr);
    bool add_track(std::size_t playlist_index, const SearchTrack& track, std::string* error = nullptr);
    bool remove_track(std::size_t playlist_index, std::size_t track_index, std::string* error = nullptr);

    const std::filesystem::path& storage_path() const noexcept { return storage_path_; }

private:
    std::filesystem::path storage_path_;
    std::vector<Playlist> playlists_;
};

}  // namespace pcyoutube::windows
