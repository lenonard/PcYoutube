#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "audio_backend.h"
#include "music_app.h"
#include "playlist_store.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using pcyoutube::music::AudioQuality;
using pcyoutube::windows::AudioBackend;
using pcyoutube::windows::BackendResult;
using pcyoutube::windows::PlaybackSnapshot;
using pcyoutube::windows::Playlist;
using pcyoutube::windows::PlaylistStore;
using pcyoutube::windows::SearchTrack;
using pcyoutube::windows::TrackInfo;

constexpr int kAppIconResourceId = 101;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_device_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_main_render_target = nullptr;
UINT g_resize_width = 0;
UINT g_resize_height = 0;

AudioBackend* g_backend = nullptr;
PlaylistStore* g_playlist_store = nullptr;
std::jthread g_search_thread;
std::jthread g_resolve_thread;
std::jthread g_poll_thread;
std::atomic_bool g_search_busy{false};
std::atomic_bool g_resolve_busy{false};

std::mutex g_async_mutex;
std::optional<std::vector<SearchTrack>> g_pending_search_results;
std::optional<std::string> g_pending_search_error;

enum class ResolveSource {
    Direct,
    Search,
    Playlist,
};

struct PendingResolve {
    BackendResult result;
    ResolveSource source = ResolveSource::Direct;
    int item_index = -1;
    int playlist_index = -1;
};
std::optional<PendingResolve> g_pending_resolve;

std::mutex g_playback_mutex;
PlaybackSnapshot g_playback;

std::vector<SearchTrack> g_search_results;
std::optional<TrackInfo> g_current_track;
std::string g_search_error;
std::string g_status = "Ready";
int g_current_index = -1;
int g_current_playlist = -1;
int g_current_playlist_track = -1;
ResolveSource g_queue_source = ResolveSource::Direct;
int g_selected_playlist = -1;
int g_quality_index = 0;
int g_volume = 75;
std::array<char, 1024> g_search_buffer{};
std::array<char, 160> g_playlist_name_buffer{};
bool g_focus_search = true;
bool g_seek_dragging = false;
float g_seek_value = 0.0f;

std::optional<SearchTrack> g_add_to_playlist_track;
bool g_request_add_popup = false;

ImFont* g_font_body = nullptr;
ImFont* g_font_small = nullptr;
ImFont* g_font_heading = nullptr;
ImFont* g_font_display = nullptr;

constexpr ImVec4 kAccent = ImVec4(0.25f, 0.78f, 0.48f, 1.0f);
constexpr ImVec4 kAccentHover = ImVec4(0.31f, 0.86f, 0.55f, 1.0f);
constexpr ImVec4 kPanel = ImVec4(0.075f, 0.082f, 0.105f, 1.0f);
constexpr ImVec4 kCard = ImVec4(0.105f, 0.115f, 0.145f, 1.0f);
constexpr ImVec4 kMuted = ImVec4(0.58f, 0.61f, 0.68f, 1.0f);

bool create_device_d3d(HWND window) {
    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_desc.BufferCount = 2;
    swap_desc.BufferDesc.Width = 0;
    swap_desc.BufferDesc.Height = 0;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.BufferDesc.RefreshRate.Numerator = 60;
    swap_desc.BufferDesc.RefreshRate.Denominator = 1;
    swap_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.OutputWindow = window;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.SampleDesc.Quality = 0;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT create_flags = 0;
    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags, feature_levels,
        static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, &swap_desc,
        &g_swap_chain, &g_device, &feature_level, &g_device_context);
    if (result == DXGI_ERROR_UNSUPPORTED) {
        return SUCCEEDED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, create_flags, feature_levels,
            static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, &swap_desc,
            &g_swap_chain, &g_device, &feature_level, &g_device_context));
    }
    return SUCCEEDED(result);
}

void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_main_render_target);
        back_buffer->Release();
    }
}

void cleanup_render_target() {
    if (g_main_render_target != nullptr) {
        g_main_render_target->Release();
        g_main_render_target = nullptr;
    }
}

void cleanup_device_d3d() {
    cleanup_render_target();
    if (g_swap_chain != nullptr) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context != nullptr) {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
}

void apply_dark_title_bar(HWND window) {
    constexpr DWORD kUseImmersiveDarkMode = 20;
    BOOL enabled = TRUE;
    DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &enabled, sizeof(enabled));
}

LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param)) {
        return true;
    }

    switch (message) {
    case WM_SIZE:
        if (w_param == SIZE_MINIMIZED) return 0;
        g_resize_width = static_cast<UINT>(LOWORD(l_param));
        g_resize_height = static_cast<UINT>(HIWORD(l_param));
        return 0;
    case WM_SYSCOMMAND:
        if ((w_param & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void configure_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18.0f, 18.0f);
    style.FramePadding = ImVec2(12.0f, 9.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 10.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 14.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 10.0f;
    style.GrabRounding = 10.0f;
    style.TabRounding = 9.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.98f, 1.0f);
    colors[ImGuiCol_TextDisabled] = kMuted;
    colors[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.049f, 0.064f, 1.0f);
    colors[ImGuiCol_ChildBg] = kPanel;
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.085f, 0.11f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.18f, 0.19f, 0.24f, 0.72f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.11f, 0.14f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.15f, 0.19f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.17f, 0.22f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.12f, 0.13f, 0.17f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.19f, 0.24f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.22f, 0.28f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.22f, 0.28f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.23f, 0.25f, 0.31f, 1.0f);
    colors[ImGuiCol_CheckMark] = kAccent;
    colors[ImGuiCol_SliderGrab] = kAccent;
    colors[ImGuiCol_SliderGrabActive] = kAccentHover;
    colors[ImGuiCol_Separator] = ImVec4(0.17f, 0.18f, 0.22f, 1.0f);
    colors[ImGuiCol_ResizeGrip] = kAccent;
    colors[ImGuiCol_NavHighlight] = kAccent;
}

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* segoe_semibold = "C:\\Windows\\Fonts\\seguisb.ttf";

    g_font_body = io.Fonts->AddFontFromFileTTF(segoe, 18.0f);
    if (g_font_body != nullptr) io.FontDefault = g_font_body;
    g_font_small = io.Fonts->AddFontFromFileTTF(segoe, 15.0f);
    g_font_heading = io.Fonts->AddFontFromFileTTF(segoe_semibold, 24.0f);
    g_font_display = io.Fonts->AddFontFromFileTTF(segoe_semibold, 31.0f);
}

AudioQuality selected_quality() {
    return static_cast<AudioQuality>(std::clamp(g_quality_index, 0, 3));
}

std::string watch_url_for(const SearchTrack& track) {
    return track.id.empty() ? std::string{} :
           "https://www.youtube.com/watch?v=" + track.id;
}

SearchTrack search_track_from_current() {
    SearchTrack track;
    if (!g_current_track) return track;
    track.id = g_current_track->id;
    track.title = g_current_track->title;
    track.channel = g_current_track->channel;
    track.thumbnail_url = g_current_track->thumbnail_url;
    track.duration = g_current_track->duration;
    return track;
}

bool input_is_direct_target() {
    const std::string input(g_search_buffer.data());
    return pcyoutube::music::looks_like_http_url(input) ||
           pcyoutube::music::extract_video_id(input).has_value();
}

void start_search(std::string query) {
    if (g_backend == nullptr || g_search_busy.exchange(true)) return;
    if (query.empty()) {
        g_search_busy = false;
        return;
    }

    if (g_search_thread.joinable()) g_search_thread.join();
    g_search_error.clear();
    g_status = "Searching YouTube...";

    g_search_thread = std::jthread([query = std::move(query)] {
        std::string error;
        auto results = g_backend->search(query, 16, error);
        {
            std::lock_guard lock(g_async_mutex);
            g_pending_search_results = std::move(results);
            g_pending_search_error = std::move(error);
        }
        g_search_busy = false;
    });
}

void start_resolve(std::string target, ResolveSource source, int item_index, int playlist_index = -1) {
    if (g_backend == nullptr || g_resolve_busy.exchange(true)) return;
    if (target.empty()) {
        g_resolve_busy = false;
        return;
    }

    if (g_resolve_thread.joinable()) g_resolve_thread.join();
    const AudioQuality quality = selected_quality();
    g_status = "Resolving direct audio stream...";

    g_resolve_thread = std::jthread([
        target = std::move(target), source, item_index, playlist_index, quality] {
        BackendResult result = g_backend->resolve(target, quality);
        {
            std::lock_guard lock(g_async_mutex);
            g_pending_resolve = PendingResolve{std::move(result), source, item_index, playlist_index};
        }
        g_resolve_busy = false;
    });
}

void start_track_index(int index) {
    if (index < 0 || index >= static_cast<int>(g_search_results.size())) return;
    start_resolve(watch_url_for(g_search_results[static_cast<std::size_t>(index)]),
                  ResolveSource::Search, index);
}

void start_playlist_track(int playlist_index, int track_index) {
    if (g_playlist_store == nullptr) return;
    const auto& playlists = g_playlist_store->playlists();
    if (playlist_index < 0 || playlist_index >= static_cast<int>(playlists.size())) return;
    const auto& tracks = playlists[static_cast<std::size_t>(playlist_index)].tracks;
    if (track_index < 0 || track_index >= static_cast<int>(tracks.size())) return;
    start_resolve(watch_url_for(tracks[static_cast<std::size_t>(track_index)]),
                  ResolveSource::Playlist, track_index, playlist_index);
}

void apply_async_results() {
    std::optional<std::vector<SearchTrack>> search_results;
    std::optional<std::string> search_error;
    std::optional<PendingResolve> resolved;
    {
        std::lock_guard lock(g_async_mutex);
        if (g_pending_search_results) {
            search_results = std::move(g_pending_search_results);
            g_pending_search_results.reset();
        }
        if (g_pending_search_error) {
            search_error = std::move(g_pending_search_error);
            g_pending_search_error.reset();
        }
        if (g_pending_resolve) {
            resolved = std::move(g_pending_resolve);
            g_pending_resolve.reset();
        }
    }

    if (search_results) {
        g_search_results = std::move(*search_results);
        g_status = g_search_results.empty() ? "No songs found" :
                   std::to_string(g_search_results.size()) + " songs found";
    }
    if (search_error) {
        g_search_error = *search_error;
        if (!g_search_error.empty()) g_status = "Search failed";
    }

    if (resolved) {
        if (resolved->result.ok) {
            if (g_backend->play(resolved->result.track)) {
                g_backend->set_volume(g_volume);
                g_current_track = std::move(resolved->result.track);
                g_queue_source = resolved->source;
                if (resolved->source == ResolveSource::Search) {
                    g_current_index = resolved->item_index;
                    g_current_playlist = -1;
                    g_current_playlist_track = -1;
                } else if (resolved->source == ResolveSource::Playlist) {
                    g_current_index = -1;
                    g_current_playlist = resolved->playlist_index;
                    g_current_playlist_track = resolved->item_index;
                } else {
                    g_current_index = -1;
                    g_current_playlist = -1;
                    g_current_playlist_track = -1;
                }
                g_status = "Playing";
            } else {
                g_status = "Could not start mpv playback";
            }
        } else {
            g_status = resolved->result.error.empty() ? "Could not resolve audio" :
                       resolved->result.error;
        }
    }
}

PlaybackSnapshot playback_snapshot() {
    std::lock_guard lock(g_playback_mutex);
    return g_playback;
}

void start_playback_poller() {
    g_poll_thread = std::jthread([](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (g_backend != nullptr) {
                const PlaybackSnapshot snapshot = g_backend->snapshot();
                {
                    std::lock_guard lock(g_playback_mutex);
                    g_playback = snapshot;
                }
            }
            for (int i = 0; i < 5 && !stop_token.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });
}

void text_muted(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void spinner(const char* label) {
    const int dots = static_cast<int>(ImGui::GetTime() * 2.8) % 4;
    std::string value(label);
    value.append(static_cast<std::size_t>(dots), '.');
    text_muted(value.c_str());
}

void section_heading(const char* text) {
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::TextUnformatted(text);
    if (g_font_heading) ImGui::PopFont();
}

void draw_album_placeholder(float size) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##album-art", ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.12f, 0.15f, 0.19f, 1.0f));
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(kAccent);
    draw->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), bg, 24.0f);

    const ImVec2 center(pos.x + size * 0.50f, pos.y + size * 0.51f);
    const float radius = size * 0.19f;
    draw->AddCircle(center, radius, accent, 48, 5.0f);
    draw->AddCircleFilled(ImVec2(center.x - radius * 0.48f, center.y + radius * 0.48f),
                          radius * 0.20f, accent, 24);
    draw->AddLine(ImVec2(center.x - radius * 0.29f, center.y + radius * 0.43f),
                  ImVec2(center.x - radius * 0.29f, center.y - radius * 0.62f), accent, 5.0f);
    draw->AddLine(ImVec2(center.x - radius * 0.29f, center.y - radius * 0.62f),
                  ImVec2(center.x + radius * 0.50f, center.y - radius * 0.42f), accent, 5.0f);
}

enum class TransportIcon { Previous, Play, Pause, Stop, Next };

bool transport_button(const char* id, TransportIcon icon, float diameter, bool accent) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(diameter, diameter));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImVec4 base = accent ? kAccent : ImVec4(0.14f, 0.15f, 0.19f, 1.0f);
    if (hovered) base = accent ? kAccentHover : ImVec4(0.20f, 0.21f, 0.26f, 1.0f);
    if (held) base = ImVec4(base.x * 0.88f, base.y * 0.88f, base.z * 0.88f, base.w);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center(pos.x + diameter * 0.5f, pos.y + diameter * 0.5f);
    draw->AddCircleFilled(center, diameter * 0.5f, ImGui::ColorConvertFloat4ToU32(base), 40);

    const ImU32 fg = ImGui::ColorConvertFloat4ToU32(
        accent ? ImVec4(0.04f, 0.06f, 0.05f, 1.0f) : ImVec4(0.94f, 0.95f, 0.98f, 1.0f));
    const float r = diameter * 0.19f;

    if (icon == TransportIcon::Play) {
        draw->AddTriangleFilled(ImVec2(center.x - r * 0.55f, center.y - r),
                                ImVec2(center.x - r * 0.55f, center.y + r),
                                ImVec2(center.x + r, center.y), fg);
    } else if (icon == TransportIcon::Pause) {
        const float w = r * 0.48f;
        draw->AddRectFilled(ImVec2(center.x - r * 0.78f, center.y - r),
                            ImVec2(center.x - r * 0.78f + w, center.y + r), fg, 2.0f);
        draw->AddRectFilled(ImVec2(center.x + r * 0.28f, center.y - r),
                            ImVec2(center.x + r * 0.28f + w, center.y + r), fg, 2.0f);
    } else if (icon == TransportIcon::Stop) {
        draw->AddRectFilled(ImVec2(center.x - r * 0.75f, center.y - r * 0.75f),
                            ImVec2(center.x + r * 0.75f, center.y + r * 0.75f), fg, 2.0f);
    } else {
        const bool previous = icon == TransportIcon::Previous;
        const float sign = previous ? -1.0f : 1.0f;
        const float line_x = center.x + sign * (-r * 0.92f);
        draw->AddLine(ImVec2(line_x, center.y - r), ImVec2(line_x, center.y + r), fg, 2.6f);
        if (previous) {
            draw->AddTriangleFilled(ImVec2(center.x + r * 0.62f, center.y - r),
                                    ImVec2(center.x + r * 0.62f, center.y + r),
                                    ImVec2(center.x - r * 0.62f, center.y), fg);
        } else {
            draw->AddTriangleFilled(ImVec2(center.x - r * 0.62f, center.y - r),
                                    ImVec2(center.x - r * 0.62f, center.y + r),
                                    ImVec2(center.x + r * 0.62f, center.y), fg);
        }
    }
    return clicked;
}

void quality_combo() {
    static const char* labels[] = {
        "Best source",
        "Prefer Opus",
        "Prefer AAC / M4A",
        "Data saver",
    };
    ImGui::SetNextItemWidth(178.0f);
    ImGui::Combo("##quality", &g_quality_index, labels, static_cast<int>(std::size(labels)));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Direct source formats only. Actual bitrate depends on the YouTube source.\n"
            "MP3 320 kbps would require transcoding and does not create extra source quality.");
    }
}

void request_add_to_playlist(const SearchTrack& track) {
    if (track.id.empty() || track.title.empty()) return;
    g_add_to_playlist_track = track;
    g_request_add_popup = true;
}

void render_search_contents() {
    const bool direct = input_is_direct_target();
    ImGui::SetNextItemWidth(-112.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##search", "Search song, artist, album or paste a YouTube URL...",
        g_search_buffer.data(), g_search_buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (g_focus_search) {
        ImGui::SetKeyboardFocusHere(-1);
        g_focus_search = false;
    }
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.06f, 0.05f, 1.0f));
    const bool action = ImGui::Button(direct ? "Play URL" : "Search", ImVec2(102.0f, 0.0f));
    ImGui::PopStyleColor(3);

    if (enter || action) {
        const std::string input(g_search_buffer.data());
        if (direct) start_resolve(input, ResolveSource::Direct, -1);
        else start_search(input);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_search_busy) {
        spinner("Searching");
    } else if (!g_search_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.50f, 0.50f, 1.0f));
        ImGui::TextWrapped("%s", g_search_error.c_str());
        ImGui::PopStyleColor();
    } else if (g_search_results.empty()) {
        text_muted("Search results will appear here. Double-click a song or press Play.");
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
        for (int i = 0; i < static_cast<int>(g_search_results.size()); ++i) {
            const SearchTrack& track = g_search_results[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const bool current = g_queue_source == ResolveSource::Search && i == g_current_index;
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                  current ? ImVec4(0.12f, 0.19f, 0.16f, 1.0f) : kCard);
            ImGui::BeginChild("track", ImVec2(0.0f, 88.0f), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            ImGui::SetCursorPos(ImVec2(14.0f, 12.0f));
            const ImVec2 art_pos = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(62.0f, 62.0f));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(art_pos, ImVec2(art_pos.x + 62.0f, art_pos.y + 62.0f),
                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f, 0.17f, 0.21f, 1.0f)), 10.0f);
            draw->AddCircleFilled(ImVec2(art_pos.x + 31.0f, art_pos.y + 31.0f), 12.0f,
                                  ImGui::ColorConvertFloat4ToU32(kAccent), 24);

            ImGui::SameLine(88.0f);
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 176.0f);
            ImGui::TextUnformatted(track.title.c_str());
            ImGui::PopTextWrapPos();
            if (g_font_small) ImGui::PushFont(g_font_small);
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            const std::string meta = track.channel + "  •  " + pcyoutube::music::format_time(track.duration);
            ImGui::TextUnformatted(meta.c_str());
            ImGui::PopStyleColor();
            if (g_font_small) ImGui::PopFont();
            ImGui::EndGroup();

            ImGui::SameLine(ImGui::GetWindowWidth() - 150.0f);
            ImGui::SetCursorPosY(27.0f);
            if (ImGui::Button("Add", ImVec2(56.0f, 34.0f))) request_add_to_playlist(track);
            ImGui::SameLine(0.0f, 6.0f);
            if (ImGui::Button("Play", ImVec2(58.0f, 34.0f))) start_track_index(i);

            if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                start_track_index(i);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
}

void render_playlist_contents() {
    if (g_playlist_store == nullptr) {
        text_muted("Playlist storage is unavailable.");
        return;
    }

    ImGui::SetNextItemWidth(-116.0f);
    ImGui::InputTextWithHint("##playlist-name", "New playlist name...",
                             g_playlist_name_buffer.data(), g_playlist_name_buffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Create", ImVec2(106.0f, 0.0f))) {
        std::string error;
        const int created = g_playlist_store->create(g_playlist_name_buffer.data(), &error);
        if (created >= 0) {
            g_selected_playlist = created;
            g_playlist_name_buffer.fill('\0');
            g_status = "Playlist created";
        } else if (!error.empty()) {
            g_status = error;
        }
    }

    auto& playlists = g_playlist_store->playlists();
    if (playlists.empty()) {
        ImGui::Spacing();
        text_muted("Create a playlist, then use Add on any search result.");
        return;
    }

    if (g_selected_playlist < 0 || g_selected_playlist >= static_cast<int>(playlists.size())) {
        g_selected_playlist = 0;
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-96.0f);
    const char* preview = playlists[static_cast<std::size_t>(g_selected_playlist)].name.c_str();
    if (ImGui::BeginCombo("##playlist-picker", preview)) {
        for (int i = 0; i < static_cast<int>(playlists.size()); ++i) {
            const bool selected = i == g_selected_playlist;
            if (ImGui::Selectable(playlists[static_cast<std::size_t>(i)].name.c_str(), selected)) {
                g_selected_playlist = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(86.0f, 0.0f))) {
        const int removed = g_selected_playlist;
        std::string error;
        if (g_playlist_store->remove(static_cast<std::size_t>(removed), &error)) {
            if (g_queue_source == ResolveSource::Playlist) {
                if (g_current_playlist == removed) {
                    g_queue_source = ResolveSource::Direct;
                    g_current_playlist = -1;
                    g_current_playlist_track = -1;
                } else if (g_current_playlist > removed) {
                    --g_current_playlist;
                }
            }
            if (g_selected_playlist >= static_cast<int>(g_playlist_store->playlists().size())) {
                g_selected_playlist = static_cast<int>(g_playlist_store->playlists().size()) - 1;
            }
            g_status = "Playlist deleted";
        } else if (!error.empty()) {
            g_status = error;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_selected_playlist < 0 ||
        g_selected_playlist >= static_cast<int>(g_playlist_store->playlists().size())) {
        return;
    }

    const Playlist& playlist = g_playlist_store->playlists()[static_cast<std::size_t>(g_selected_playlist)];
    if (playlist.tracks.empty()) {
        text_muted("This playlist is empty. Add songs from Search or Now Playing.");
        return;
    }

    for (int i = 0; i < static_cast<int>(playlist.tracks.size()); ++i) {
        const SearchTrack track = playlist.tracks[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        const bool current = g_queue_source == ResolveSource::Playlist &&
                             g_current_playlist == g_selected_playlist &&
                             g_current_playlist_track == i;
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              current ? ImVec4(0.12f, 0.19f, 0.16f, 1.0f) : kCard);
        ImGui::BeginChild("playlist-track", ImVec2(0.0f, 78.0f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(14.0f, 12.0f));
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 168.0f);
        ImGui::TextUnformatted(track.title.c_str());
        ImGui::PopTextWrapPos();
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        const std::string meta = track.channel + "  •  " + pcyoutube::music::format_time(track.duration);
        ImGui::TextUnformatted(meta.c_str());
        ImGui::PopStyleColor();
        if (g_font_small) ImGui::PopFont();

        ImGui::SameLine(ImGui::GetWindowWidth() - 144.0f);
        ImGui::SetCursorPosY(22.0f);
        if (ImGui::Button("Play", ImVec2(58.0f, 34.0f))) {
            start_playlist_track(g_selected_playlist, i);
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button("Remove", ImVec2(68.0f, 34.0f))) {
            std::string error;
            if (g_playlist_store->remove_track(static_cast<std::size_t>(g_selected_playlist),
                                               static_cast<std::size_t>(i), &error)) {
                if (g_queue_source == ResolveSource::Playlist &&
                    g_current_playlist == g_selected_playlist) {
                    if (g_current_playlist_track == i) {
                        g_queue_source = ResolveSource::Direct;
                        g_current_playlist_track = -1;
                    } else if (g_current_playlist_track > i) {
                        --g_current_playlist_track;
                    }
                }
                g_status = "Removed from playlist";
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopID();
                break;
            }
            if (!error.empty()) g_status = error;
        }

        if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            start_playlist_track(g_selected_playlist, i);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void render_library_panel(float width, float height) {
    ImGui::BeginChild("LibraryPanel", ImVec2(width, height), ImGuiChildFlags_Borders);

    section_heading("Library");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 204.0f);
    quality_combo();
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("Direct source stream • bitrate shown after resolve");
    ImGui::PopStyleColor();
    if (g_font_small) ImGui::PopFont();

    ImGui::Spacing();
    if (ImGui::BeginTabBar("LibraryTabs")) {
        if (ImGui::BeginTabItem("Search")) {
            ImGui::Spacing();
            render_search_contents();
            ImGui::EndTabItem();
        }
        const std::string playlist_tab = g_playlist_store == nullptr ? "Playlists" :
            "Playlists (" + std::to_string(g_playlist_store->playlists().size()) + ")";
        if (ImGui::BeginTabItem(playlist_tab.c_str())) {
            ImGui::Spacing();
            render_playlist_contents();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();
}

std::string format_audio_meta(const TrackInfo& track) {
    std::string value;
    if (!track.extension.empty()) value += track.extension;
    if (!track.codec.empty()) {
        if (!value.empty()) value += "  •  ";
        value += track.codec;
    }
    if (track.abr_kbps > 0.0) {
        if (!value.empty()) value += "  •  ";
        value += std::to_string(static_cast<int>(std::round(track.abr_kbps))) + " kbps";
    }
    if (track.sample_rate_hz > 0.0) {
        if (!value.empty()) value += "  •  ";
        value += std::to_string(static_cast<int>(std::round(track.sample_rate_hz / 1000.0))) + " kHz";
    }
    return value.empty() ? "Audio stream" : value;
}

void render_now_playing(float width, float height) {
    ImGui::BeginChild("NowPlaying", ImVec2(width, height), ImGuiChildFlags_Borders);
    section_heading("Now Playing");
    ImGui::Spacing();

    const float cover = std::clamp(ImGui::GetContentRegionAvail().x * 0.58f, 170.0f, 270.0f);
    const float center_x = (ImGui::GetContentRegionAvail().x - cover) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_x);
    draw_album_placeholder(cover);
    ImGui::Spacing();

    if (!g_current_track) {
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::TextWrapped("Choose a song to start listening");
        if (g_font_heading) ImGui::PopFont();
        text_muted("Search YouTube or open a saved playlist from the Library.");
    } else {
        const TrackInfo& track = *g_current_track;
        if (g_font_heading) ImGui::PushFont(g_font_heading);
        ImGui::TextWrapped("%s", track.title.c_str());
        if (g_font_heading) ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("%s", track.channel.c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.18f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        const std::string quality = std::string(pcyoutube::music::quality_label(track.requested_quality));
        ImGui::Button(quality.c_str());
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        const std::string audio_meta = format_audio_meta(track);
        text_muted(audio_meta.c_str());

        if (track.duration > 0.0) {
            const std::string duration = "Duration  " + pcyoutube::music::format_time(track.duration);
            text_muted(duration.c_str());
        }

        if (!track.id.empty()) {
            ImGui::Spacing();
            if (ImGui::Button("Add to playlist")) {
                request_add_to_playlist(search_track_from_current());
            }
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Track details")) {
            if (!track.id.empty()) ImGui::Text("Video ID: %s", track.id.c_str());
            if (!track.format_id.empty()) ImGui::Text("Format: %s", track.format_id.c_str());
            if (track.abr_kbps > 0.0) ImGui::Text("Source bitrate: %.0f kbps", track.abr_kbps);
            if (track.sample_rate_hz > 0.0) ImGui::Text("Sample rate: %.0f Hz", track.sample_rate_hz);
            ImGui::TextWrapped("Source: %s", track.webpage_url.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted("Direct audio URL");
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("%s", track.direct_url.c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button("Copy direct URL")) {
                ImGui::SetClipboardText(track.direct_url.c_str());
                g_status = "Direct audio URL copied";
            }
        }
    }

    if (g_resolve_busy) {
        ImGui::Spacing();
        spinner("Preparing audio");
    }

    ImGui::EndChild();
}

void render_add_to_playlist_popup() {
    if (g_request_add_popup) {
        ImGui::OpenPopup("Add song to playlist");
        g_request_add_popup = false;
    }

    if (!ImGui::BeginPopupModal("Add song to playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    if (!g_add_to_playlist_track || g_playlist_store == nullptr) {
        text_muted("No song selected.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("%s", g_add_to_playlist_track->title.c_str());
    ImGui::Spacing();

    auto& playlists = g_playlist_store->playlists();
    if (playlists.empty()) {
        text_muted("No playlists yet. Create one below.");
    } else {
        for (int i = 0; i < static_cast<int>(playlists.size()); ++i) {
            ImGui::PushID(i);
            const std::string label = playlists[static_cast<std::size_t>(i)].name +
                                      "  (" + std::to_string(playlists[static_cast<std::size_t>(i)].tracks.size()) + ")";
            if (ImGui::Button(label.c_str(), ImVec2(330.0f, 0.0f))) {
                std::string error;
                if (g_playlist_store->add_track(static_cast<std::size_t>(i),
                                                *g_add_to_playlist_track, &error)) {
                    g_selected_playlist = i;
                    g_status = "Added to " + playlists[static_cast<std::size_t>(i)].name;
                    g_add_to_playlist_track.reset();
                    ImGui::CloseCurrentPopup();
                } else if (!error.empty()) {
                    g_status = error;
                }
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##popup-new-playlist", "New playlist name...",
                             g_playlist_name_buffer.data(), g_playlist_name_buffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Create + Add")) {
        std::string error;
        const int created = g_playlist_store->create(g_playlist_name_buffer.data(), &error);
        if (created >= 0) {
            if (g_playlist_store->add_track(static_cast<std::size_t>(created),
                                            *g_add_to_playlist_track, &error)) {
                g_selected_playlist = created;
                g_playlist_name_buffer.fill('\0');
                g_status = "Playlist created and song added";
                g_add_to_playlist_track.reset();
                ImGui::CloseCurrentPopup();
            } else if (!error.empty()) {
                g_status = error;
            }
        } else if (!error.empty()) {
            g_status = error;
        }
    }

    if (ImGui::Button("Cancel")) {
        g_add_to_playlist_track.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void start_previous_track() {
    if (g_resolve_busy) return;
    if (g_queue_source == ResolveSource::Search && g_current_index > 0) {
        start_track_index(g_current_index - 1);
        return;
    }
    if (g_queue_source == ResolveSource::Playlist && g_current_playlist_track > 0) {
        start_playlist_track(g_current_playlist, g_current_playlist_track - 1);
    }
}

void start_next_track() {
    if (g_resolve_busy) return;
    if (g_queue_source == ResolveSource::Search &&
        g_current_index >= 0 && g_current_index + 1 < static_cast<int>(g_search_results.size())) {
        start_track_index(g_current_index + 1);
        return;
    }
    if (g_queue_source == ResolveSource::Playlist && g_playlist_store != nullptr &&
        g_current_playlist >= 0 &&
        g_current_playlist < static_cast<int>(g_playlist_store->playlists().size())) {
        const auto& tracks = g_playlist_store->playlists()[static_cast<std::size_t>(g_current_playlist)].tracks;
        if (g_current_playlist_track >= 0 &&
            g_current_playlist_track + 1 < static_cast<int>(tracks.size())) {
            start_playlist_track(g_current_playlist, g_current_playlist_track + 1);
        }
    }
}

bool has_previous_track() {
    if (g_queue_source == ResolveSource::Search) return g_current_index > 0;
    if (g_queue_source == ResolveSource::Playlist) return g_current_playlist_track > 0;
    return false;
}

bool has_next_track() {
    if (g_queue_source == ResolveSource::Search) {
        return g_current_index >= 0 &&
               g_current_index + 1 < static_cast<int>(g_search_results.size());
    }
    if (g_queue_source == ResolveSource::Playlist && g_playlist_store != nullptr &&
        g_current_playlist >= 0 &&
        g_current_playlist < static_cast<int>(g_playlist_store->playlists().size())) {
        const auto& tracks = g_playlist_store->playlists()[static_cast<std::size_t>(g_current_playlist)].tracks;
        return g_current_playlist_track >= 0 &&
               g_current_playlist_track + 1 < static_cast<int>(tracks.size());
    }
    return false;
}

void render_player_bar(float height) {
    ImGui::BeginChild("PlayerBar", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
    const PlaybackSnapshot snapshot = playback_snapshot();

    const double duration = snapshot.duration > 0.0 ? snapshot.duration :
                            (g_current_track ? g_current_track->duration : 0.0);
    const float max_value = static_cast<float>(std::max(0.001, duration));
    if (!g_seek_dragging) g_seek_value = static_cast<float>(snapshot.position);

    const std::string left_time = pcyoutube::music::format_time(g_seek_dragging ? g_seek_value : snapshot.position);
    const std::string right_time = pcyoutube::music::format_time(duration);
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::TextUnformatted(left_time.c_str());
    ImGui::SameLine();
    const float time_width = ImGui::CalcTextSize(right_time.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - time_width - 18.0f);
    ImGui::TextUnformatted(right_time.c_str());
    if (g_font_small) ImGui::PopFont();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 10.0f);
    ImGui::SliderFloat("##progress", &g_seek_value, 0.0f, max_value, "", ImGuiSliderFlags_NoInput);
    if (ImGui::IsItemActivated()) g_seek_dragging = true;
    if (g_seek_dragging && ImGui::IsItemDeactivatedAfterEdit()) {
        if (g_backend != nullptr) g_backend->seek(g_seek_value);
        g_seek_dragging = false;
    }
    ImGui::PopStyleVar();

    // Anchor transport controls from the bottom instead of relying on layout flow.
    // This leaves a fixed safe margin and prevents the 58 px play button from being clipped.
    const float controls_y = std::max(ImGui::GetCursorPosY() + 8.0f, height - 78.0f);
    ImGui::SetCursorPosY(controls_y);

    const float center_width = 42.0f + 14.0f + 58.0f + 14.0f + 42.0f + 14.0f + 42.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - center_width) * 0.5f);

    const bool has_prev = has_previous_track();
    const bool has_next = has_next_track();

    ImGui::BeginDisabled(!has_prev || g_resolve_busy);
    if (transport_button("##prev", TransportIcon::Previous, 42.0f, false)) start_previous_track();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 14.0f);

    const TransportIcon play_icon = snapshot.running && !snapshot.paused ?
                                    TransportIcon::Pause : TransportIcon::Play;
    if (transport_button("##playpause", play_icon, 58.0f, true)) {
        if (g_backend != nullptr) {
            if (snapshot.running) {
                g_backend->toggle_pause();
            } else if (g_current_track) {
                g_backend->play(*g_current_track);
            } else if (!g_search_results.empty()) {
                start_track_index(0);
            }
        }
    }
    ImGui::SameLine(0.0f, 14.0f);

    ImGui::BeginDisabled(!has_next || g_resolve_busy);
    if (transport_button("##next", TransportIcon::Next, 42.0f, false)) start_next_track();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 14.0f);

    if (transport_button("##stop", TransportIcon::Stop, 42.0f, false)) {
        if (g_backend != nullptr) g_backend->stop();
    }

    const float volume_width = 180.0f;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - volume_width - 18.0f, controls_y + 9.0f));
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::TextUnformatted("VOL");
    if (g_font_small) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(volume_width - 54.0f);
    int volume = g_volume;
    if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d")) {
        g_volume = volume;
        if (g_backend != nullptr) g_backend->set_volume(g_volume);
    }

    ImGui::SetCursorPos(ImVec2(18.0f, controls_y + 14.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::TextUnformatted(g_status.c_str());
    if (g_font_small) ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::EndChild();
}

void render_app() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
    ImGui::Begin("PcYoutubeRoot", nullptr, flags);

    if (g_font_display) ImGui::PushFont(g_font_display);
    ImGui::TextUnformatted("PcYoutube Music");
    if (g_font_display) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
    text_muted("direct audio player • v0.5");

    ImGui::Spacing();

    const float player_height = 172.0f;
    const float content_height = std::max(260.0f, ImGui::GetContentRegionAvail().y - player_height - 10.0f);
    const float available_width = ImGui::GetContentRegionAvail().x;
    const float left_width = std::max(480.0f, available_width * 0.61f);
    const float right_width = std::max(340.0f, available_width - left_width - 10.0f);

    render_library_panel(left_width, content_height);
    ImGui::SameLine();
    render_now_playing(right_width, content_height);

    ImGui::Spacing();
    render_player_bar(player_height);
    render_add_to_playlist_popup();

    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (std::wcsstr(GetCommandLineW(), L"--self-test") != nullptr) {
        return pcyoutube::music::self_test() ? 0 : 1;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"PcYoutubeMusicImGui";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    RegisterClassExW(&window_class);

    HWND window = CreateWindowW(
        window_class.lpszClassName, L"PcYoutube Music",
        WS_OVERLAPPEDWINDOW, 100, 80, 1240, 850,
        nullptr, nullptr, window_class.hInstance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        return 2;
    }

    apply_dark_title_bar(window);
    if (!create_device_d3d(window)) {
        cleanup_device_d3d();
        DestroyWindow(window);
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        return 3;
    }
    create_render_target();

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;

    configure_style();
    load_fonts();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_device_context);

    AudioBackend backend;
    PlaylistStore playlist_store;
    g_backend = &backend;
    g_playlist_store = &playlist_store;

    std::string playlist_error;
    if (!playlist_store.load(&playlist_error) && !playlist_error.empty()) {
        g_status = playlist_error;
    } else if (!playlist_store.playlists().empty()) {
        g_selected_playlist = 0;
    }
    if (!backend.ready()) {
        g_status = backend.readiness_error();
    }
    start_playback_poller();

    bool done = false;
    while (!done) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }

        apply_async_results();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        render_app();
        ImGui::Render();

        const float clear_color[4] = {0.045f, 0.049f, 0.064f, 1.0f};
        g_device_context->OMSetRenderTargets(1, &g_main_render_target, nullptr);
        g_device_context->ClearRenderTargetView(g_main_render_target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    if (g_poll_thread.joinable()) {
        g_poll_thread.request_stop();
        g_poll_thread.join();
    }
    if (g_search_thread.joinable()) g_search_thread.join();
    if (g_resolve_thread.joinable()) g_resolve_thread.join();

    backend.shutdown();
    g_backend = nullptr;
    g_playlist_store = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device_d3d();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    return 0;
}
