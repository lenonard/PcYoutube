#include "hello_app.h"

namespace pcyoutube::hello {

AppText app_text() noexcept {
    return {
        "PcYoutube",
        "Hello World",
        "Native C++ shell is running. Core code is kept platform-independent for a future Android frontend."
    };
}

bool self_test() noexcept {
    const auto text = app_text();
    return !text.title.empty() && !text.heading.empty() && !text.body.empty();
}

}
