#include "music_app.h"

#include <algorithm>
#include <cctype>
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

std::string url_encode(std::string_view value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;

    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else if (ch == ' ') {
            out << '+';
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }

    return out.str();
}

}  // namespace

AppText app_text() {
    return {
        "PcYoutube Music",
        "Search YouTube, choose a track, and listen in a music-first native desktop shell."
    };
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

std::string make_embed_url(std::string_view video_id) {
    if (!is_valid_video_id(video_id)) {
        return {};
    }

    return "https://www.youtube.com/embed/" + std::string(video_id) +
           "?autoplay=1&controls=1&playsinline=1&rel=0";
}

std::string make_search_url(std::string_view query) {
    const std::string cleaned = trim(query);
    if (cleaned.empty()) {
        return "https://www.youtube.com/";
    }
    return "https://www.youtube.com/results?search_query=" + url_encode(cleaned);
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
           make_embed_url("M7lc1UVf-VE").find("youtube.com/embed/M7lc1UVf-VE") != std::string::npos &&
           make_search_url("lofi hip hop").find("lofi+hip+hop") != std::string::npos;
}

}  // namespace pcyoutube::music
