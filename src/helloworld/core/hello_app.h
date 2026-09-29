#pragma once

#include <string_view>

namespace pcyoutube::hello {
struct AppText {
    std::string_view title;
    std::string_view heading;
    std::string_view body;
};
AppText app_text() noexcept;
bool self_test() noexcept;
}
