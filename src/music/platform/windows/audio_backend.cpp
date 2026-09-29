#include "audio_backend.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace pcyoutube::windows {
namespace {

using json = nlohmann::json;

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (size <= 0) {
        flags = 0;
        size = MultiByteToWideChar(CP_UTF8, flags, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0);
    }
    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, flags, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
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

std::string run_process_capture(const std::filesystem::path& executable,
                                const std::vector<std::wstring>& arguments,
                                DWORD& exit_code) {
    exit_code = static_cast<DWORD>(-1);

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        return "Could not create process output pipe.";
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
        executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup, &process);

    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        return "Could not start process. Win32 error=" + std::to_string(GetLastError());
    }

    CloseHandle(process.hThread);

    std::string output;
    char buffer[8192];
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

std::string compact_error(std::string value) {
    if (value.empty()) {
        return "The external tool returned no details.";
    }
    if (value.size() > 1200U) {
        value.resize(1200U);
        value += "...";
    }
    return value;
}

std::string json_string(const json& value, std::string_view key) {
    const auto it = value.find(std::string(key));
    if (it == value.end() || it->is_null()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return {};
}

double json_number(const json& value, std::string_view key) {
    const auto it = value.find(std::string(key));
    if (it == value.end() || it->is_null() || !it->is_number()) {
        return 0.0;
    }
    return it->get<double>();
}

std::string channel_from_json(const json& value) {
    std::string channel = json_string(value, "channel");
    if (channel.empty()) channel = json_string(value, "uploader");
    if (channel.empty()) channel = json_string(value, "artist");
    return channel.empty() ? "Unknown channel" : channel;
}

std::string thumbnail_from_json(const json& value) {
    std::string thumbnail = json_string(value, "thumbnail");
    if (!thumbnail.empty()) {
        return thumbnail;
    }

    const auto it = value.find("thumbnails");
    if (it != value.end() && it->is_array()) {
        for (auto rit = it->rbegin(); rit != it->rend(); ++rit) {
            const std::string url = json_string(*rit, "url");
            if (!url.empty()) {
                return url;
            }
        }
    }
    return {};
}

json unwrap_first_entry(json root) {
    if (root.is_object()) {
        const auto entries = root.find("entries");
        if (entries != root.end() && entries->is_array()) {
            for (const auto& entry : *entries) {
                if (entry.is_object()) {
                    return entry;
                }
            }
        }
    }
    return root;
}

std::string make_watch_url(std::string_view id) {
    return id.empty() ? std::string{} : "https://www.youtube.com/watch?v=" + std::string(id);
}

}  // namespace

AudioBackend::AudioBackend() {
    exe_dir_ = executable_directory();
    yt_dlp_path_ = exe_dir_ / L"tools" / L"yt-dlp.exe";
    mpv_path_ = exe_dir_ / L"tools" / L"mpv" / L"mpv.exe";
}

AudioBackend::~AudioBackend() {
    shutdown();
}

bool AudioBackend::ready() const {
    return file_exists(yt_dlp_path_) && file_exists(mpv_path_);
}

std::string AudioBackend::readiness_error() const {
    if (!file_exists(yt_dlp_path_)) {
        return "Missing tools/yt-dlp.exe";
    }
    if (!file_exists(mpv_path_)) {
        return "Missing tools/mpv/mpv.exe";
    }
    return {};
}

std::vector<SearchTrack> AudioBackend::search(std::string_view query, int max_results,
                                               std::string& error) const {
    error.clear();
    std::vector<SearchTrack> tracks;
    if (!file_exists(yt_dlp_path_)) {
        error = "yt-dlp.exe is missing from the tools folder.";
        return tracks;
    }

    const std::string target = music::make_search_target(query, max_results);
    if (target.empty()) {
        error = "Enter a search term.";
        return tracks;
    }

    const std::vector<std::wstring> arguments = {
        L"--no-config", L"--flat-playlist", L"--ignore-errors", L"--no-warnings",
        L"--no-progress", L"--no-color", L"--dump-single-json", utf8_to_wide(target)};

    DWORD exit_code = 0;
    const std::string output = run_process_capture(yt_dlp_path_, arguments, exit_code);
    if (exit_code != 0) {
        error = compact_error(output);
        return tracks;
    }

    try {
        const json root = json::parse(output);
        const auto entries = root.find("entries");
        if (entries == root.end() || !entries->is_array()) {
            error = "yt-dlp returned no search result list.";
            return tracks;
        }

        for (const auto& entry : *entries) {
            if (!entry.is_object()) continue;
            SearchTrack track;
            track.id = json_string(entry, "id");
            track.title = json_string(entry, "title");
            track.channel = channel_from_json(entry);
            track.thumbnail_url = thumbnail_from_json(entry);
            track.duration = json_number(entry, "duration");
            if (!track.id.empty() && !track.title.empty()) {
                tracks.push_back(std::move(track));
            }
        }
    } catch (const std::exception& exception) {
        error = std::string("Could not parse yt-dlp search data: ") + exception.what();
    }

    if (tracks.empty() && error.empty()) {
        error = "No matching tracks were found.";
    }
    return tracks;
}

BackendResult AudioBackend::resolve(std::string_view target, music::AudioQuality quality) const {
    BackendResult result;
    if (!file_exists(yt_dlp_path_)) {
        result.error = "yt-dlp.exe is missing from the tools folder.";
        return result;
    }

    const std::string resolved_target = music::make_yt_dlp_target(target);
    if (resolved_target.empty()) {
        result.error = "No track target was provided.";
        return result;
    }

    const std::vector<std::wstring> arguments = {
        L"--no-config", L"--no-playlist", L"--no-warnings", L"--no-progress",
        L"--no-color", L"--simulate", L"--format",
        utf8_to_wide(music::quality_selector(quality)), L"--dump-single-json",
        utf8_to_wide(resolved_target)};

    DWORD exit_code = 0;
    const std::string output = run_process_capture(yt_dlp_path_, arguments, exit_code);
    if (exit_code != 0) {
        result.error = compact_error(output);
        return result;
    }

    try {
        json item = unwrap_first_entry(json::parse(output));
        TrackInfo track;
        track.id = json_string(item, "id");
        track.title = json_string(item, "title");
        track.channel = channel_from_json(item);
        track.webpage_url = json_string(item, "webpage_url");
        if (track.webpage_url.empty()) track.webpage_url = make_watch_url(track.id);
        track.thumbnail_url = thumbnail_from_json(item);
        track.direct_url = json_string(item, "url");
        track.format_id = json_string(item, "format_id");
        track.extension = json_string(item, "ext");
        track.codec = json_string(item, "acodec");
        track.duration = json_number(item, "duration");
        track.abr_kbps = json_number(item, "abr");
        track.sample_rate_hz = json_number(item, "asr");
        track.requested_quality = quality;

        if (track.title.empty()) track.title = "YouTube audio";
        if (track.direct_url.empty()) {
            result.error = "yt-dlp returned metadata but no direct audio URL.";
            return result;
        }

        result.ok = true;
        result.track = std::move(track);
    } catch (const std::exception& exception) {
        result.error = std::string("Could not parse yt-dlp track data: ") + exception.what();
    }
    return result;
}

bool AudioBackend::start_mpv() {
    if (mpv_process_ != nullptr) {
        DWORD exit_code = 0;
        HANDLE process = reinterpret_cast<HANDLE>(mpv_process_);
        if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
            return true;
        }
        CloseHandle(process);
        mpv_process_ = nullptr;
    }

    if (!file_exists(mpv_path_)) {
        return false;
    }

    mpv_pipe_name_ = L"\\\\.\\pipe\\PcYoutubeMpv_" + std::to_wstring(GetCurrentProcessId());
    const std::vector<std::wstring> arguments = {
        L"--idle=yes", L"--no-video", L"--audio-display=no", L"--force-window=no",
        L"--no-terminal", L"--really-quiet", L"--volume=" + std::to_wstring(volume_),
        L"--input-ipc-server=" + mpv_pipe_name_};

    std::wstring command_line = quote_argument(mpv_path_.wstring());
    for (const auto& argument : arguments) {
        command_line.push_back(L' ');
        command_line += quote_argument(argument);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    const std::wstring working_directory = mpv_path_.parent_path().wstring();
    const BOOL created = CreateProcessW(
        mpv_path_.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, working_directory.c_str(), &startup, &process);
    if (!created) {
        return false;
    }

    CloseHandle(process.hThread);
    mpv_process_ = process.hProcess;
    return true;
}

bool AudioBackend::send_command(std::string_view json_command, std::string* response) {
    std::lock_guard lock(mpv_mutex_);
    if (!start_mpv()) {
        return false;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 40; ++attempt) {
        pipe = CreateFileW(mpv_pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(mpv_pipe_name_.c_str(), 100);
        } else {
            Sleep(25);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::string line(json_command);
    line.push_back('\n');
    DWORD written = 0;
    const BOOL write_ok = WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()),
                                    &written, nullptr);
    if (!write_ok || written != line.size()) {
        CloseHandle(pipe);
        return false;
    }

    if (response != nullptr) {
        response->clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                break;
            }
            if (available > 0) {
                char buffer[4096];
                DWORD read = 0;
                if (ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr)
                    && read > 0) {
                    response->append(buffer, buffer + read);
                    if (response->find('\n') != std::string::npos) break;
                }
            } else {
                Sleep(10);
            }
        }
    }

    CloseHandle(pipe);
    return true;
}

std::optional<double> AudioBackend::query_number(std::string_view property) {
    static std::atomic_uint request_id{1};
    const unsigned id = request_id.fetch_add(1);
    json command = {{"command", {"get_property", std::string(property)}}, {"request_id", id}};
    std::string response;
    if (!send_command(command.dump(), &response)) {
        return std::nullopt;
    }
    const auto newline = response.find('\n');
    if (newline != std::string::npos) response.resize(newline);
    try {
        const json value = json::parse(response);
        if (value.value("error", std::string("error")) == "success") {
            const auto it = value.find("data");
            if (it != value.end() && it->is_number()) return it->get<double>();
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<bool> AudioBackend::query_bool(std::string_view property) {
    static std::atomic_uint request_id{100000};
    const unsigned id = request_id.fetch_add(1);
    json command = {{"command", {"get_property", std::string(property)}}, {"request_id", id}};
    std::string response;
    if (!send_command(command.dump(), &response)) {
        return std::nullopt;
    }
    const auto newline = response.find('\n');
    if (newline != std::string::npos) response.resize(newline);
    try {
        const json value = json::parse(response);
        if (value.value("error", std::string("error")) == "success") {
            const auto it = value.find("data");
            if (it != value.end() && it->is_boolean()) return it->get<bool>();
        }
    } catch (...) {
    }
    return std::nullopt;
}

bool AudioBackend::play(const TrackInfo& track) {
    if (track.direct_url.empty()) return false;
    json command = {{"command", {"loadfile", track.direct_url, "replace"}}};
    return send_command(command.dump());
}

bool AudioBackend::toggle_pause() {
    const bool paused = query_bool("pause").value_or(false);
    json command = {{"command", {"set_property", "pause", !paused}}};
    return send_command(command.dump());
}

bool AudioBackend::stop() {
    return send_command(R"({"command":["stop"]})");
}

bool AudioBackend::seek(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    json command = {{"command", {"set_property", "time-pos", seconds}}};
    return send_command(command.dump());
}

bool AudioBackend::set_volume(int volume) {
    volume_ = std::clamp(volume, 0, 100);
    json command = {{"command", {"set_property", "volume", volume_}}};
    return send_command(command.dump());
}

PlaybackSnapshot AudioBackend::snapshot() {
    PlaybackSnapshot state;
    state.volume = volume_;

    if (mpv_process_ == nullptr) return state;
    DWORD exit_code = 0;
    HANDLE process = reinterpret_cast<HANDLE>(mpv_process_);
    if (!GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE) return state;

    const bool idle = query_bool("idle-active").value_or(true);
    state.running = !idle;
    state.paused = query_bool("pause").value_or(false);
    state.position = query_number("time-pos").value_or(0.0);
    state.duration = query_number("duration").value_or(0.0);
    return state;
}

void AudioBackend::shutdown() {
    if (mpv_process_ == nullptr) return;

    send_command(R"({"command":["quit"]})");
    HANDLE process = reinterpret_cast<HANDLE>(mpv_process_);
    if (WaitForSingleObject(process, 500) == WAIT_TIMEOUT) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 500);
    }
    CloseHandle(process);
    mpv_process_ = nullptr;
}

}  // namespace pcyoutube::windows
