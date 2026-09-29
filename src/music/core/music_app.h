#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pcyoutube::music {

enum class AudioQuality {
    Best = 0,
    High,
    Balanced,
    DataSaver
};

struct AppText {
    std::string title;
    std::string subtitle;
};

AppText app_text();

std::optional<std::string> extract_video_id(std::string_view input);
std::string make_yt_dlp_target(std::string_view input);
std::string make_search_target(std::string_view query, int max_results = 12);
bool looks_like_http_url(std::string_view input);

std::string_view quality_label(AudioQuality quality) noexcept;
std::string_view quality_selector(AudioQuality quality) noexcept;

std::string format_time(double seconds);

bool self_test();

}  // namespace pcyoutube::music
