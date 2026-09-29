#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "music_app.h"

namespace {

constexpr int kInputId = 1001;
constexpr int kPlayId = 1002;
constexpr int kPauseId = 1003;
constexpr int kStopId = 1004;
constexpr int kCopyId = 1005;
constexpr int kVolumeId = 1006;
constexpr UINT kResolvedMessage = WM_APP + 1;

HWND g_main_window = nullptr;
HWND g_input_edit = nullptr;
HWND g_play_button = nullptr;
HWND g_pause_button = nullptr;
HWND g_stop_button = nullptr;
HWND g_copy_button = nullptr;
HWND g_volume_slider = nullptr;
HWND g_title_text = nullptr;
HWND g_direct_edit = nullptr;
HWND g_status_text = nullptr;

std::filesystem::path g_exe_dir;
std::filesystem::path g_yt_dlp_path;
std::filesystem::path g_mpv_path;
std::wstring g_mpv_pipe_name;
HANDLE g_mpv_process = nullptr;
std::atomic_bool g_resolving{false};

struct ResolveResult {
    bool ok = false;
    std::string title;
    std::string url;
    std::string error;
};

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);

    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        flags = 0;
        size = MultiByteToWideChar(
            CP_UTF8,
            flags,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0);
    }

    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        flags,
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
        0,
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
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::wstring read_window_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }

    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(control, value.data(), length + 1);
    if (copied <= 0) {
        return {};
    }
    value.resize(static_cast<std::size_t>(copied));
    return value;
}

void set_text(HWND control, std::wstring_view text) {
    if (control != nullptr) {
        const std::wstring copy(text);
        SetWindowTextW(control, copy.c_str());
    }
}

void set_status(std::wstring_view text) {
    set_text(g_status_text, text);
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::wstring quote_argument(std::wstring_view argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring result = L"\"";
    std::size_t backslashes = 0;

    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'\"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }

        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }

    result.append(backslashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}

std::string run_process_capture(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    DWORD& exit_code) {
    exit_code = static_cast<DWORD>(-1);

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        return "Could not create output pipe.";
    }

    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring command_line = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_argument(argument);
    }

    const std::wstring working_directory = executable.parent_path().wstring();
    const BOOL created = CreateProcessW(
        executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup,
        &process);

    CloseHandle(write_pipe);

    if (!created) {
        CloseHandle(read_pipe);
        return "Could not start process. Win32 error=" + std::to_string(GetLastError());
    }

    CloseHandle(process.hThread);

    std::string output;
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buffer, buffer + bytes_read);
    }

    CloseHandle(read_pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return output;
}

std::string trim_line(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

ResolveResult resolve_audio(std::string_view target) {
    ResolveResult result;

    const std::vector<std::wstring> arguments = {
        L"--no-config",
        L"--no-playlist",
        L"--no-warnings",
        L"--no-progress",
        L"--no-color",
        L"--simulate",
        L"--format",
        L"bestaudio[ext=m4a]/bestaudio",
        L"--print",
        L"PCY_TITLE=%(title)s",
        L"--print",
        L"PCY_URL=%(url)s",
        utf8_to_wide(target)
    };

    DWORD exit_code = 0;
    const std::string output = run_process_capture(g_yt_dlp_path, arguments, exit_code);

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim_line(std::move(line));
        constexpr std::string_view title_prefix = "PCY_TITLE=";
        constexpr std::string_view url_prefix = "PCY_URL=";

        if (line.rfind(title_prefix, 0) == 0) {
            result.title = line.substr(title_prefix.size());
        } else if (line.rfind(url_prefix, 0) == 0) {
            result.url = line.substr(url_prefix.size());
        }
    }

    if (exit_code == 0 && !result.url.empty()) {
        result.ok = true;
        if (result.title.empty()) {
            result.title = "YouTube audio";
        }
        return result;
    }

    result.error = output;
    if (result.error.empty()) {
        result.error = "yt-dlp did not return an audio URL.";
    }
    if (result.error.size() > 900U) {
        result.error.resize(900U);
        result.error += "...";
    }
    return result;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 32U);

    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch >= 0x20U) {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

bool start_mpv() {
    if (g_mpv_process != nullptr) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(g_mpv_process, &exit_code) && exit_code == STILL_ACTIVE) {
            return true;
        }
        CloseHandle(g_mpv_process);
        g_mpv_process = nullptr;
    }

    if (!file_exists(g_mpv_path)) {
        return false;
    }

    g_mpv_pipe_name = L"\\\\.\\pipe\\PcYoutubeMpv_" + std::to_wstring(GetCurrentProcessId());

    const std::vector<std::wstring> arguments = {
        L"--idle=yes",
        L"--no-video",
        L"--audio-display=no",
        L"--force-window=no",
        L"--no-terminal",
        L"--really-quiet",
        L"--volume=75",
        L"--input-ipc-server=" + g_mpv_pipe_name
    };

    std::wstring command_line = quote_argument(g_mpv_path.wstring());
    for (const auto& argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_argument(argument);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    const std::wstring working_directory = g_mpv_path.parent_path().wstring();
    const BOOL created = CreateProcessW(
        g_mpv_path.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process);

    if (!created) {
        return false;
    }

    CloseHandle(process.hThread);
    g_mpv_process = process.hProcess;
    return true;
}

bool send_mpv_command(std::string_view command) {
    if (!start_mpv()) {
        return false;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 30; ++attempt) {
        pipe = CreateFileW(
            g_mpv_pipe_name.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }

        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(g_mpv_pipe_name.c_str(), 100);
        } else {
            Sleep(50);
        }
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::string line(command);
    line.push_back('\n');
    DWORD written = 0;
    const BOOL ok = WriteFile(
        pipe,
        line.data(),
        static_cast<DWORD>(line.size()),
        &written,
        nullptr);
    CloseHandle(pipe);
    return ok && written == line.size();
}

void stop_mpv_process() {
    if (g_mpv_process == nullptr) {
        return;
    }

    send_mpv_command(R"({"command":["quit"]})");
    if (WaitForSingleObject(g_mpv_process, 400) == WAIT_TIMEOUT) {
        TerminateProcess(g_mpv_process, 0);
        WaitForSingleObject(g_mpv_process, 400);
    }
    CloseHandle(g_mpv_process);
    g_mpv_process = nullptr;
}

bool copy_to_clipboard(std::wstring_view text) {
    if (!OpenClipboard(g_main_window)) {
        return false;
    }

    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }

    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(destination)[text.size()] = L'\0';
    GlobalUnlock(memory);

    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

void layout_controls() {
    if (g_main_window == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(g_main_window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    constexpr int margin = 16;
    constexpr int button_width = 96;
    constexpr int row_height = 30;

    if (g_input_edit) {
        MoveWindow(g_input_edit, margin, 36, width - margin * 3 - button_width, row_height, TRUE);
    }
    if (g_play_button) {
        MoveWindow(g_play_button, width - margin - button_width, 36, button_width, row_height, TRUE);
    }

    if (g_pause_button) {
        MoveWindow(g_pause_button, margin, 82, 128, row_height, TRUE);
    }
    if (g_stop_button) {
        MoveWindow(g_stop_button, margin + 138, 82, 86, row_height, TRUE);
    }
    if (g_volume_slider) {
        MoveWindow(g_volume_slider, margin + 292, 80, width - margin * 2 - 292, 34, TRUE);
    }

    if (g_title_text) {
        MoveWindow(g_title_text, margin, 132, width - margin * 2, 22, TRUE);
    }
    if (g_direct_edit) {
        MoveWindow(g_direct_edit, margin, 164, width - margin * 3 - button_width, row_height, TRUE);
    }
    if (g_copy_button) {
        MoveWindow(g_copy_button, width - margin - button_width, 164, button_width, row_height, TRUE);
    }
    if (g_status_text) {
        MoveWindow(g_status_text, margin, height - 34, width - margin * 2, 22, TRUE);
    }
}

void begin_resolve_and_play() {
    if (g_resolving.exchange(true)) {
        return;
    }

    const std::string input = wide_to_utf8(read_window_text(g_input_edit));
    const std::string target = pcyoutube::music::make_yt_dlp_target(input);

    if (target.empty()) {
        g_resolving = false;
        set_status(L"Enter a YouTube URL, video ID, or search text.");
        return;
    }

    if (!file_exists(g_yt_dlp_path)) {
        g_resolving = false;
        set_status(L"Missing tools\\yt-dlp.exe next to PcYoutube.exe.");
        return;
    }

    if (!file_exists(g_mpv_path)) {
        g_resolving = false;
        set_status(L"Missing tools\\mpv\\mpv.exe next to PcYoutube.exe.");
        return;
    }

    EnableWindow(g_play_button, FALSE);
    set_status(L"Resolving a fresh audio-only stream URL with yt-dlp...");

    std::thread([target]() {
        auto result = std::make_unique<ResolveResult>(resolve_audio(target));
        HWND window = g_main_window;
        if (window != nullptr && IsWindow(window) &&
            PostMessageW(window, kResolvedMessage, 0, reinterpret_cast<LPARAM>(result.get()))) {
            result.release();
            return;
        }
        g_resolving = false;
    }).detach();
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE: {
        g_main_window = hwnd;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        HWND input_label = CreateWindowExW(
            0, L"STATIC", L"YouTube URL / video ID / search text", WS_CHILD | WS_VISIBLE,
            16, 14, 400, 20, hwnd, nullptr, nullptr, nullptr);

        g_input_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            16, 36, 600, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputId)), nullptr, nullptr);

        g_play_button = CreateWindowExW(
            0, L"BUTTON", L"Resolve + Play",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            630, 36, 110, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlayId)), nullptr, nullptr);

        g_pause_button = CreateWindowExW(
            0, L"BUTTON", L"Pause / Resume",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            16, 82, 128, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPauseId)), nullptr, nullptr);

        g_stop_button = CreateWindowExW(
            0, L"BUTTON", L"Stop",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            154, 82, 86, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopId)), nullptr, nullptr);

        HWND volume_label = CreateWindowExW(
            0, L"STATIC", L"Volume", WS_CHILD | WS_VISIBLE,
            252, 87, 58, 20, hwnd, nullptr, nullptr, nullptr);

        g_volume_slider = CreateWindowExW(
            0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
            308, 80, 420, 34, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVolumeId)), nullptr, nullptr);
        SendMessageW(g_volume_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(g_volume_slider, TBM_SETPOS, TRUE, 75);

        g_title_text = CreateWindowExW(
            0, L"STATIC", L"Now playing: -", WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 132, 720, 22, hwnd, nullptr, nullptr, nullptr);

        g_direct_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            16, 164, 600, 30, hwnd, nullptr, nullptr, nullptr);

        g_copy_button = CreateWindowExW(
            0, L"BUTTON", L"Copy URL",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            630, 164, 96, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyId)), nullptr, nullptr);

        g_status_text = CreateWindowExW(
            0, L"STATIC", L"Ready. No WebView is used.", WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 212, 720, 22, hwnd, nullptr, nullptr, nullptr);

        for (HWND control : {
                 input_label, g_input_edit, g_play_button, g_pause_button, g_stop_button,
                 volume_label, g_volume_slider, g_title_text, g_direct_edit,
                 g_copy_button, g_status_text}) {
            if (control != nullptr) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
        }

        layout_controls();
        return 0;
    }

    case WM_SIZE:
        layout_controls();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kPlayId:
            if (HIWORD(w_param) == BN_CLICKED) {
                begin_resolve_and_play();
                return 0;
            }
            break;
        case kPauseId:
            if (HIWORD(w_param) == BN_CLICKED) {
                if (send_mpv_command(R"({"command":["cycle","pause"]})")) {
                    set_status(L"Pause/resume toggled.");
                } else {
                    set_status(L"mpv is not available.");
                }
                return 0;
            }
            break;
        case kStopId:
            if (HIWORD(w_param) == BN_CLICKED) {
                if (send_mpv_command(R"({"command":["stop"]})")) {
                    set_status(L"Stopped.");
                }
                return 0;
            }
            break;
        case kCopyId:
            if (HIWORD(w_param) == BN_CLICKED) {
                const std::wstring url = read_window_text(g_direct_edit);
                if (!url.empty() && copy_to_clipboard(url)) {
                    set_status(L"Direct audio URL copied to clipboard.");
                }
                return 0;
            }
            break;
        default:
            break;
        }
        break;

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(l_param) == g_volume_slider) {
            const int volume = static_cast<int>(SendMessageW(g_volume_slider, TBM_GETPOS, 0, 0));
            const std::string command =
                "{\"command\":[\"set_property\",\"volume\"," + std::to_string(volume) + "]}";
            send_mpv_command(command);
            return 0;
        }
        break;

    case kResolvedMessage: {
        std::unique_ptr<ResolveResult> result(reinterpret_cast<ResolveResult*>(l_param));
        g_resolving = false;
        EnableWindow(g_play_button, TRUE);

        if (!result || !result->ok) {
            std::wstring error = result ? utf8_to_wide(result->error) : L"Unknown resolver error.";
            for (wchar_t& ch : error) {
                if (ch == L'\r' || ch == L'\n') {
                    ch = L' ';
                }
            }
            set_status(L"yt-dlp error: " + error);
            return 0;
        }

        set_text(g_title_text, L"Now playing: " + utf8_to_wide(result->title));
        set_text(g_direct_edit, utf8_to_wide(result->url));

        if (!start_mpv()) {
            set_status(L"Resolved audio URL, but mpv could not start.");
            return 0;
        }

        const std::string command =
            "{\"command\":[\"loadfile\",\"" + json_escape(result->url) + "\",\"replace\"]}";
        if (send_mpv_command(command)) {
            set_status(L"Playing direct audio stream. The URL is shown above and can be copied.");
        } else {
            set_status(L"Resolved audio URL, but could not send it to mpv.");
        }
        return 0;
    }

    case WM_DESTROY:
        g_main_window = nullptr;
        stop_mpv_process();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    if (std::wstring_view(GetCommandLineW()).find(L"--self-test") != std::wstring_view::npos) {
        return pcyoutube::music::self_test() ? 0 : 1;
    }

    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&common_controls);

    g_exe_dir = executable_directory();
    g_yt_dlp_path = g_exe_dir / L"tools" / L"yt-dlp.exe";
    g_mpv_path = g_exe_dir / L"tools" / L"mpv" / L"mpv.exe";

    const auto app_text = pcyoutube::music::app_text();
    const std::wstring title = utf8_to_wide(app_text.title);
    constexpr wchar_t kWindowClass[] = L"PcYoutubeAudioWindow";

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

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
        860,
        300,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr) {
        return 3;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
