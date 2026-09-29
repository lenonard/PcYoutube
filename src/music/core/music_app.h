#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pcyoutube::music {

struct AppText {
    std::string title;
    std::string subtitle;
};

AppText app_text();

std::optional<std::string> extract_video_id(std::string_view input);
std::string make_yt_dlp_target(std::string_view input);
bool looks_like_http_url(std::string_view input);

bool self_test();

}  // namespace pcyoutube::music
