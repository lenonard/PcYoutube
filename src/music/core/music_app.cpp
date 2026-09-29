#include "music_app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace pcyoutube::music {
namespace {

bool is_video_id_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-';
}

bool is_valid_video_id(std::string_view value) {
    return value.size() == 11 &&
           std::all_of(value.begin(), value.end(), [](char ch) { return is_video_id_char(ch); });
}

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

std::optional<std::string> read_id_after(std::string_view value, std::string_view marker) {
    const auto pos = value.find(marker);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }

    const auto start = pos + marker.size();
    if (start + 11 > value.size()) {
        return std::nullopt;
    }

    const std::string candidate(value.substr(start, 11));
    return is_valid_video_id(candidate) ? std::optional<std::string>(candidate) : std::nullopt;
}

}  // namespace

AppText app_text() {
    return {
        "PcYoutube Music",
        "Fast native YouTube audio search, direct-stream playback and a modern music-player interface."
    };
}

bool looks_like_http_url(std::string_view input) {
    const std::string value = trim(input);
    return value.starts_with("https://") || value.starts_with("http://");
}

std::optional<std::string> extract_video_id(std::string_view input) {
    const std::string value = trim(input);
    if (is_valid_video_id(value)) {
        return value;
    }

    for (const std::string_view marker : {
             std::string_view("youtu.be/"),
             std::string_view("youtube.com/embed/"),
             std::string_view("youtube.com/shorts/"),
             std::string_view("youtube.com/watch?v="),
             std::string_view("music.youtube.com/watch?v="),
             std::string_view("?v="),
             std::string_view("&v=")}) {
        if (auto id = read_id_after(value, marker)) {
            return id;
        }
    }

    return std::nullopt;
}

std::string make_yt_dlp_target(std::string_view input) {
    const std::string value = trim(input);
    if (value.empty()) {
        return {};
    }

    if (const auto id = extract_video_id(value)) {
        return "https://www.youtube.com/watch?v=" + *id;
    }

    if (looks_like_http_url(value)) {
        return value;
    }

    return make_search_target(value, 1);
}

std::string make_search_target(std::string_view query, int max_results) {
    const std::string value = trim(query);
    if (value.empty()) {
        return {};
    }
    max_results = std::clamp(max_results, 1, 50);
    return "ytsearch" + std::to_string(max_results) + ":" + value;
}

std::string_view quality_label(AudioQuality quality) noexcept {
    switch (quality) {
    case AudioQuality::Best: return "Best available";
    case AudioQuality::High: return "High";
    case AudioQuality::Balanced: return "Balanced";
    case AudioQuality::DataSaver: return "Data saver";
    default: return "Best available";
    }
}

std::string_view quality_selector(AudioQuality quality) noexcept {
    switch (quality) {
    case AudioQuality::Best:
        return "bestaudio";
    case AudioQuality::High:
        return "bestaudio[abr>=160]/bestaudio[ext=m4a]/bestaudio";
    case AudioQuality::Balanced:
        return "bestaudio[abr<=128]/bestaudio[ext=m4a]/bestaudio";
    case AudioQuality::DataSaver:
        return "worstaudio";
    default:
        return "bestaudio";
    }
}

std::string format_time(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    const int total = static_cast<int>(std::llround(seconds));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;

    std::ostringstream out;
    if (hours > 0) {
        out << hours << ':' << std::setfill('0') << std::setw(2) << minutes << ':'
            << std::setw(2) << secs;
    } else {
        out << minutes << ':' << std::setfill('0') << std::setw(2) << secs;
    }
    return out.str();
}

bool self_test() {
    const auto direct = extract_video_id("M7lc1UVf-VE");
    const auto watch = extract_video_id("https://www.youtube.com/watch?v=M7lc1UVf-VE&t=1");
    const auto short_url = extract_video_id("https://youtu.be/M7lc1UVf-VE");
    const auto invalid = extract_video_id("not-a-video-id");

    return direct && *direct == "M7lc1UVf-VE" &&
           watch && *watch == "M7lc1UVf-VE" &&
           short_url && *short_url == "M7lc1UVf-VE" &&
           !invalid &&
           make_yt_dlp_target("M7lc1UVf-VE") ==
               "https://www.youtube.com/watch?v=M7lc1UVf-VE" &&
           make_search_target("lofi hip hop", 12) == "ytsearch12:lofi hip hop" &&
           quality_selector(AudioQuality::DataSaver) == "worstaudio" &&
           format_time(125.0) == "2:05";
}

}  // namespace pcyoutube::music
