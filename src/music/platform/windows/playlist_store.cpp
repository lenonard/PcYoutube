#include "playlist_store.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

namespace pcyoutube::windows {
namespace {

using json = nlohmann::json;

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::filesystem::path default_storage_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
    std::filesystem::path root;
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        root = std::filesystem::path(buffer);
    } else {
        root = std::filesystem::temp_directory_path();
    }
    return root / L"PcYoutube" / L"playlists.json";
}

json track_to_json(const SearchTrack& track) {
    return {
        {"id", track.id},
        {"title", track.title},
        {"channel", track.channel},
        {"thumbnail_url", track.thumbnail_url},
        {"duration", track.duration},
    };
}

SearchTrack track_from_json(const json& value) {
    SearchTrack track;
    if (!value.is_object()) return track;
    track.id = value.value("id", std::string{});
    track.title = value.value("title", std::string{});
    track.channel = value.value("channel", std::string{});
    track.thumbnail_url = value.value("thumbnail_url", std::string{});
    track.duration = value.value("duration", 0.0);
    return track;
}

}  // namespace

PlaylistStore::PlaylistStore() : storage_path_(default_storage_path()) {}

bool PlaylistStore::load(std::string* error) {
    if (error != nullptr) error->clear();
    playlists_.clear();
    settings_ = AppSettings{};

    std::error_code fs_error;
    if (!std::filesystem::exists(storage_path_, fs_error)) {
        return true;
    }

    try {
        std::ifstream input(storage_path_, std::ios::binary);
        if (!input) {
            if (error != nullptr) *error = "Could not open playlists.json for reading.";
            return false;
        }

        json root;
        input >> root;

        const auto settings_it = root.find("settings");
        if (settings_it != root.end() && settings_it->is_object()) {
            settings_.volume = std::clamp(settings_it->value("volume", 75), 0, 100);
            settings_.quality_index = std::clamp(settings_it->value("quality_index", 0), 0, 3);
            settings_.selected_playlist = settings_it->value("selected_playlist", -1);
            settings_.shuffle = settings_it->value("shuffle", false);
            settings_.repeat_mode = std::clamp(settings_it->value("repeat_mode", 0), 0, 2);
        }

        const auto it = root.find("playlists");
        if (it == root.end() || !it->is_array()) {
            return true;
        }

        for (const auto& playlist_json : *it) {
            if (!playlist_json.is_object()) continue;
            Playlist playlist;
            playlist.name = trim(playlist_json.value("name", std::string{}));
            if (playlist.name.empty()) continue;

            const auto tracks = playlist_json.find("tracks");
            if (tracks != playlist_json.end() && tracks->is_array()) {
                for (const auto& track_json : *tracks) {
                    SearchTrack track = track_from_json(track_json);
                    if (!track.id.empty() && !track.title.empty()) {
                        playlist.tracks.push_back(std::move(track));
                    }
                }
            }
            playlists_.push_back(std::move(playlist));
        }

        if (playlists_.empty()) {
            settings_.selected_playlist = -1;
        } else {
            settings_.selected_playlist = std::clamp(
                settings_.selected_playlist, 0, static_cast<int>(playlists_.size()) - 1);
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Could not parse playlists.json: ") + exception.what();
        }
        return false;
    }
}

bool PlaylistStore::save(std::string* error) const {
    if (error != nullptr) error->clear();
    try {
        std::error_code fs_error;
        std::filesystem::create_directories(storage_path_.parent_path(), fs_error);
        if (fs_error) {
            if (error != nullptr) *error = "Could not create the PcYoutube data folder.";
            return false;
        }

        json root;
        root["version"] = 2;
        root["settings"] = {
            {"volume", std::clamp(settings_.volume, 0, 100)},
            {"quality_index", std::clamp(settings_.quality_index, 0, 3)},
            {"selected_playlist", settings_.selected_playlist},
            {"shuffle", settings_.shuffle},
            {"repeat_mode", std::clamp(settings_.repeat_mode, 0, 2)},
        };
        root["playlists"] = json::array();
        for (const Playlist& playlist : playlists_) {
            json item;
            item["name"] = playlist.name;
            item["tracks"] = json::array();
            for (const SearchTrack& track : playlist.tracks) {
                item["tracks"].push_back(track_to_json(track));
            }
            root["playlists"].push_back(std::move(item));
        }

        const std::filesystem::path temp = storage_path_.wstring() + L".tmp";
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output) {
                if (error != nullptr) *error = "Could not open playlist temp file for writing.";
                return false;
            }
            output << root.dump(2);
        }

        std::filesystem::remove(storage_path_, fs_error);
        fs_error.clear();
        std::filesystem::rename(temp, storage_path_, fs_error);
        if (fs_error) {
            if (error != nullptr) *error = "Could not replace playlists.json.";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = std::string("Could not save playlists: ") + exception.what();
        return false;
    }
}

int PlaylistStore::create(std::string_view name, std::string* error) {
    if (error != nullptr) error->clear();
    const std::string cleaned = trim(name);
    if (cleaned.empty()) {
        if (error != nullptr) *error = "Playlist name cannot be empty.";
        return -1;
    }

    const auto duplicate = std::find_if(playlists_.begin(), playlists_.end(), [&](const Playlist& p) {
        return p.name == cleaned;
    });
    if (duplicate != playlists_.end()) {
        if (error != nullptr) *error = "A playlist with this name already exists.";
        return -1;
    }

    playlists_.push_back(Playlist{cleaned, {}});
    settings_.selected_playlist = static_cast<int>(playlists_.size() - 1U);
    if (!save(error)) {
        playlists_.pop_back();
        settings_.selected_playlist = playlists_.empty() ? -1 : 0;
        return -1;
    }
    return static_cast<int>(playlists_.size() - 1U);
}

bool PlaylistStore::remove(std::size_t playlist_index, std::string* error) {
    if (error != nullptr) error->clear();
    if (playlist_index >= playlists_.size()) return false;
    const Playlist backup = playlists_[playlist_index];
    const AppSettings old_settings = settings_;
    playlists_.erase(playlists_.begin() + static_cast<std::ptrdiff_t>(playlist_index));
    if (playlists_.empty()) {
        settings_.selected_playlist = -1;
    } else if (settings_.selected_playlist >= static_cast<int>(playlists_.size())) {
        settings_.selected_playlist = static_cast<int>(playlists_.size()) - 1;
    }
    if (!save(error)) {
        playlists_.insert(playlists_.begin() + static_cast<std::ptrdiff_t>(playlist_index), backup);
        settings_ = old_settings;
        return false;
    }
    return true;
}

bool PlaylistStore::add_track(std::size_t playlist_index, const SearchTrack& track, std::string* error) {
    if (error != nullptr) error->clear();
    if (playlist_index >= playlists_.size() || track.id.empty() || track.title.empty()) return false;
    auto& tracks = playlists_[playlist_index].tracks;
    const bool exists = std::any_of(tracks.begin(), tracks.end(), [&](const SearchTrack& existing) {
        return existing.id == track.id;
    });
    if (exists) {
        if (error != nullptr) *error = "This song is already in the playlist.";
        return false;
    }
    tracks.push_back(track);
    if (!save(error)) {
        tracks.pop_back();
        return false;
    }
    return true;
}

bool PlaylistStore::remove_track(std::size_t playlist_index, std::size_t track_index,
                                 std::string* error) {
    if (error != nullptr) error->clear();
    if (playlist_index >= playlists_.size()) return false;
    auto& tracks = playlists_[playlist_index].tracks;
    if (track_index >= tracks.size()) return false;
    const SearchTrack backup = tracks[track_index];
    tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(track_index));
    if (!save(error)) {
        tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(track_index), backup);
        return false;
    }
    return true;
}

bool PlaylistStore::move_track(std::size_t playlist_index, std::size_t from_index,
                               std::size_t to_index, std::string* error) {
    if (error != nullptr) error->clear();
    if (playlist_index >= playlists_.size()) return false;
    auto& tracks = playlists_[playlist_index].tracks;
    if (from_index >= tracks.size() || to_index >= tracks.size()) return false;
    if (from_index == to_index) return true;

    const auto backup = tracks;
    SearchTrack moving = std::move(tracks[from_index]);
    tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(from_index));
    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(to_index), std::move(moving));
    if (!save(error)) {
        tracks = backup;
        return false;
    }
    return true;
}

}  // namespace pcyoutube::windows
