#include <windows.h>
#include <WebView2.h>
#include <wrl.h>
#include <wrl/event.h>

#include <cwchar>
#include <string>
#include <string_view>

#include "music_app.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSearchEditId = 1001;
constexpr int kGoButtonId = 1002;
constexpr int kBackButtonId = 1003;
constexpr int kStatusTextId = 1004;
constexpr int kTopBarHeight = 58;
constexpr int kBottomStatusHeight = 28;

HWND g_main_window = nullptr;
HWND g_search_edit = nullptr;
HWND g_go_button = nullptr;
HWND g_back_button = nullptr;
HWND g_status_text = nullptr;
ComPtr<ICoreWebView2Controller> g_webview_controller;
ComPtr<ICoreWebView2> g_webview;
EventRegistrationToken g_navigation_starting_token{};

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
        return {};
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

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

void set_status(std::wstring_view text) {
    if (g_status_text != nullptr) {
        SetWindowTextW(g_status_text, std::wstring(text).c_str());
    }
}

std::wstring read_edit_text(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) {
        return {};
    }

    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    return text;
}

void layout_children() {
    if (g_main_window == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(g_main_window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    const int margin = 12;
    const int button_width = 92;
    const int back_width = 82;
    const int control_height = 32;
    const int controls_y = 12;

    if (g_search_edit != nullptr) {
        const int edit_width = (width - margin * 4 - button_width - back_width);
        MoveWindow(g_search_edit, margin, controls_y, edit_width > 180 ? edit_width : 180, control_height, TRUE);
    }

    if (g_go_button != nullptr) {
        MoveWindow(g_go_button, width - margin * 2 - back_width - button_width, controls_y,
                   button_width, control_height, TRUE);
    }

    if (g_back_button != nullptr) {
        MoveWindow(g_back_button, width - margin - back_width, controls_y,
                   back_width, control_height, TRUE);
    }

    if (g_status_text != nullptr) {
        MoveWindow(g_status_text, margin, height - kBottomStatusHeight,
                   width - margin * 2, 20, TRUE);
    }

    if (g_webview_controller) {
        RECT bounds{};
        bounds.left = margin;
        bounds.top = kTopBarHeight;
        bounds.right = width - margin;
        bounds.bottom = height - kBottomStatusHeight - 4;

        if (bounds.right > bounds.left && bounds.bottom > bounds.top) {
            g_webview_controller->put_Bounds(bounds);
        }
    }
}

void navigate_to_input() {
    if (!g_webview || g_search_edit == nullptr) {
        set_status(L"WebView2 is still starting...");
        return;
    }

    const std::wstring input_wide = read_edit_text(g_search_edit);
    const std::string input = wide_to_utf8(input_wide);
    if (input.empty()) {
        set_status(L"Type a song/artist name or paste a YouTube link.");
        return;
    }

    if (const auto video_id = pcyoutube::music::extract_video_id(input)) {
        const std::string url = pcyoutube::music::make_embed_url(*video_id);
        const std::wstring wide_url = utf8_to_wide(url);
        g_webview->Navigate(wide_url.c_str());
        set_status(L"Playing with the official visible YouTube embedded player.");
        return;
    }

    const std::string url = pcyoutube::music::make_search_url(input);
    const std::wstring wide_url = utf8_to_wide(url);
    g_webview->Navigate(wide_url.c_str());
    set_status(L"Search results from YouTube. Select a video to play it inside PcYoutube Music.");
}

void navigate_home() {
    if (!g_webview) {
        return;
    }

    const std::wstring home = utf8_to_wide(pcyoutube::music::make_search_url("music"));
    g_webview->Navigate(home.c_str());
    set_status(L"Browse YouTube music results, or search above.");
}

HRESULT initialize_webview() {
    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        nullptr,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || environment == nullptr) {
                    set_status(L"WebView2 Runtime is unavailable. Install Microsoft Edge WebView2 Runtime.");
                    return result;
                }

                return environment->CreateCoreWebView2Controller(
                    g_main_window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controller_result) || controller == nullptr) {
                                set_status(L"Could not create the embedded YouTube browser.");
                                return controller_result;
                            }

                            g_webview_controller = controller;
                            const HRESULT webview_result = controller->get_CoreWebView2(g_webview.GetAddressOf());
                            if (FAILED(webview_result) || !g_webview) {
                                set_status(L"Could not initialize WebView2.");
                                return webview_result;
                            }

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(settings.GetAddressOf())) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(TRUE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreBrowserAcceleratorKeysEnabled(TRUE);
                            }

                            g_webview->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2* sender,
                                       ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        LPWSTR raw_uri = nullptr;
                                        if (FAILED(args->get_Uri(&raw_uri)) || raw_uri == nullptr) {
                                            return S_OK;
                                        }

                                        const std::wstring uri(raw_uri);
                                        CoTaskMemFree(raw_uri);

                                        const bool is_watch_page =
                                            uri.find(L"youtube.com/watch") != std::wstring::npos ||
                                            uri.find(L"youtu.be/") != std::wstring::npos ||
                                            uri.find(L"youtube.com/shorts/") != std::wstring::npos;

                                        if (!is_watch_page) {
                                            return S_OK;
                                        }

                                        const auto video_id = pcyoutube::music::extract_video_id(wide_to_utf8(uri));
                                        if (!video_id) {
                                            return S_OK;
                                        }

                                        const std::wstring embed_url =
                                            utf8_to_wide(pcyoutube::music::make_embed_url(*video_id));
                                        args->put_Cancel(TRUE);
                                        sender->Navigate(embed_url.c_str());
                                        set_status(L"Now playing. The YouTube player stays visible by design.");
                                        return S_OK;
                                    })
                                    .Get(),
                                &g_navigation_starting_token);

                            layout_children();

                            const wchar_t* welcome_html = LR"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:Segoe UI,Arial,sans-serif;background:#101114;color:#f4f4f4;margin:0;display:flex;align-items:center;justify-content:center;min-height:100vh}
main{max-width:680px;padding:42px;text-align:center}
h1{font-size:38px;margin:0 0 12px}
p{color:#b9bdc7;line-height:1.6;font-size:17px}
strong{color:#fff}
</style>
</head>
<body>
<main>
<h1>PcYoutube Music</h1>
<p>Search above for a song, artist, album, or paste a YouTube URL.</p>
<p><strong>Music-first UI:</strong> PcYoutube uses the official YouTube embedded player for playback; it does not extract or download a separate audio stream.</p>
</main>
</body>
</html>)HTML";
                            g_webview->NavigateToString(welcome_html);
                            set_status(L"Ready. Search for music or paste a YouTube URL.");
                            return S_OK;
                        })
                        .Get());
            })
            .Get());
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE: {
        g_main_window = hwnd;

        HFONT gui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        g_search_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0,
            0,
            100,
            30,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEditId)),
            nullptr,
            nullptr);

        g_go_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Search / Play",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0,
            0,
            100,
            30,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGoButtonId)),
            nullptr,
            nullptr);

        g_back_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Music",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0,
            0,
            80,
            30,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackButtonId)),
            nullptr,
            nullptr);

        g_status_text = CreateWindowExW(
            0,
            L"STATIC",
            L"Starting WebView2...",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0,
            0,
            100,
            20,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusTextId)),
            nullptr,
            nullptr);

        SendMessageW(g_search_edit, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
        SendMessageW(g_go_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
        SendMessageW(g_back_button, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);
        SendMessageW(g_status_text, WM_SETFONT, reinterpret_cast<WPARAM>(gui_font), TRUE);

        SendMessageW(g_search_edit, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"Song, artist, or YouTube URL"));

        layout_children();
        const HRESULT result = initialize_webview();
        if (FAILED(result)) {
            set_status(L"Failed to start WebView2.");
        }
        return 0;
    }

    case WM_SIZE:
        layout_children();
        return 0;

    case WM_COMMAND:
        if (LOWORD(w_param) == kGoButtonId && HIWORD(w_param) == BN_CLICKED) {
            navigate_to_input();
            return 0;
        }
        if (LOWORD(w_param) == kBackButtonId && HIWORD(w_param) == BN_CLICKED) {
            navigate_home();
            return 0;
        }
        if (LOWORD(w_param) == kSearchEditId && HIWORD(w_param) == EN_UPDATE) {
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (w_param == VK_RETURN && GetFocus() == g_search_edit) {
            navigate_to_input();
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_webview) {
            g_webview->remove_NavigationStarting(g_navigation_starting_token);
        }
        g_webview.Reset();
        g_webview_controller.Reset();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    if (std::wcsstr(GetCommandLineW(), L"--self-test") != nullptr) {
        return pcyoutube::music::self_test() ? 0 : 1;
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        return 2;
    }

    const auto app_text = pcyoutube::music::app_text();
    const std::wstring title = utf8_to_wide(app_text.title);
    constexpr wchar_t kWindowClass[] = L"PcYoutubeMusicWindow";

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (RegisterClassW(&window_class) == 0) {
        CoUninitialize();
        return 3;
    }

    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1120,
        760,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr) {
        CoUninitialize();
        return 4;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
