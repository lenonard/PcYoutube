#include <windows.h>

#include <cwchar>
#include <string>
#include <string_view>

#include "../../core/hello_app.h"

namespace {

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);

    if (size <= 0) {
        return L"";
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size);
    return result;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);

        RECT client{};
        GetClientRect(hwnd, &client);
        client.left += 32;
        client.top += 28;
        client.right -= 32;
        client.bottom -= 28;

        const auto text = pcyoutube::hello::app_text();
        const std::wstring heading = utf8_to_wide(text.heading);
        const std::wstring body = utf8_to_wide(text.body);

        SetBkMode(dc, TRANSPARENT);
        HFONT gui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, gui_font);

        RECT heading_rect = client;
        heading_rect.bottom = heading_rect.top + 48;
        DrawTextW(dc, heading.c_str(), -1, &heading_rect, DT_LEFT | DT_TOP | DT_SINGLELINE);

        RECT body_rect = client;
        body_rect.top += 64;
        DrawTextW(dc, body.c_str(), -1, &body_rect, DT_LEFT | DT_TOP | DT_WORDBREAK);

        SelectObject(dc, old_font);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, w_param, l_param);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    if (std::wcsstr(GetCommandLineW(), L"--self-test") != nullptr) {
        return pcyoutube::hello::self_test() ? 0 : 1;
    }

    const auto app_text = pcyoutube::hello::app_text();
    const std::wstring title = utf8_to_wide(app_text.title);
    constexpr wchar_t kWindowClass[] = L"PcYoutubeHelloWindow";

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (RegisterClassW(&window_class) == 0) {
        return 2;
    }

    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        360,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr) {
        return 3;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
