#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "music_app.h"

namespace pcyoutube::windows {

struct SearchTrack {
    std::string id;
    std::string title;
    std::string channel;
    std::string thumbnail_url;
    double duration = 0.0;
};

struct TrackInfo {
    std::string id;
    std::string title;
    std::string channel;
    std::string webpage_url;
    std::string thumbnail_url;
    std::string direct_url;
    std::string format_id;
    std::string extension;
    std::string codec;
    double duration = 0.0;
    double abr_kbps = 0.0;
    double sample_rate_hz = 0.0;
    music::AudioQuality requested_quality = music::AudioQuality::Best;
};

struct PlaybackSnapshot {
    bool running = false;
    bool paused = false;
    double position = 0.0;
    double duration = 0.0;
    int volume = 75;
};

struct BackendResult {
    bool ok = false;
    TrackInfo track;
    std::string error;
};

class AudioBackend {
public:
    AudioBackend();
    ~AudioBackend();

    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    bool ready() const;
    std::string readiness_error() const;

    std::vector<SearchTrack> search(std::string_view query, int max_results, std::string& error) const;
    BackendResult resolve(std::string_view target, music::AudioQuality quality) const;

    bool play(const TrackInfo& track);
    bool toggle_pause();
    bool stop();
    bool seek(double seconds);
    bool set_volume(int volume);
    PlaybackSnapshot snapshot();

    void shutdown();

private:
    bool start_mpv();
    bool send_command(std::string_view json_command, std::string* response = nullptr);
    std::optional<double> query_number(std::string_view property);
    std::optional<bool> query_bool(std::string_view property);

    std::filesystem::path exe_dir_;
    std::filesystem::path yt_dlp_path_;
    std::filesystem::path mpv_path_;
    std::wstring mpv_pipe_name_;
    void* mpv_process_ = nullptr;
    int volume_ = 75;
    std::mutex mpv_mutex_;
};

}  // namespace pcyoutube::windows
