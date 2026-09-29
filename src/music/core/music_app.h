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
std::string make_embed_url(std::string_view video_id);
std::string make_search_url(std::string_view query);

bool self_test();

}  // namespace pcyoutube::music
