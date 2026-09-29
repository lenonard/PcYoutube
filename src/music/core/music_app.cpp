#include "music_app.h"

#include <algorithm>
#include <cctype>

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
        "PcYoutube Audio",
        "Resolve YouTube audio directly with yt-dlp and play it through an audio-only mpv engine."
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

    return "ytsearch1:" + value;
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
           make_yt_dlp_target("lofi hip hop") == "ytsearch1:lofi hip hop" &&
           make_yt_dlp_target("https://example.com/audio") == "https://example.com/audio";
}

}  // namespace pcyoutube::music
