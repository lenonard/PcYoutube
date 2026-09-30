#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "audio_backend.h"
#include "music_app.h"
#include "playlist_store.h"
#include "thumbnail_cache.h"

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
using pcyoutube::windows::ThumbnailCache;
using pcyoutube::windows::TrackInfo;

constexpr int kAppIconResourceId = 101;

enum class ResolveSource { Direct, Search, Playlist };
enum class RepeatMode { Off = 0, All = 1, One = 2 };
enum class TransportIcon { Previous, Play, Pause, Stop, Next };

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_device_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
UINT g_resize_width = 0;
UINT g_resize_height = 0;

AudioBackend* g_backend = nullptr;
PlaylistStore* g_store = nullptr;
ThumbnailCache* g_thumbnails = nullptr;

std::jthread g_search_thread;
std::jthread g_resolve_thread;
std::jthread g_poll_thread;
std::atomic_bool g_search_busy{false};
std::atomic_bool g_resolve_busy{false};

std::mutex g_async_mutex;
std::optional<std::vector<SearchTrack>> g_pending_search;
std::optional<std::string> g_pending_search_error;

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
ResolveSource g_queue_source = ResolveSource::Direct;
int g_current_search_index = -1;
int g_current_playlist = -1;
int g_current_playlist_track = -1;
int g_selected_playlist = -1;

int g_quality_index = 0;
int g_volume = 75;
bool g_shuffle = false;
RepeatMode g_repeat = RepeatMode::Off;
bool g_seek_dragging = false;
float g_seek_value = 0.0f;
std::string g_status = "Ready";
std::string g_search_error;
std::array<char, 1024> g_search_buffer{};
std::array<char, 160> g_playlist_name_buffer{};
bool g_focus_search = true;

std::optional<SearchTrack> g_add_track;
bool g_open_add_popup = false;

bool g_previous_running = false;
bool g_auto_advance_armed = false;
bool g_manual_stop = false;
std::mt19937 g_rng{std::random_device{}()};

ImFont* g_font_body = nullptr;
ImFont* g_font_small = nullptr;
ImFont* g_font_heading = nullptr;
ImFont* g_font_display = nullptr;

constexpr ImVec4 kAccent = ImVec4(0.25f, 0.78f, 0.48f, 1.0f);
constexpr ImVec4 kAccentHover = ImVec4(0.31f, 0.86f, 0.55f, 1.0f);
constexpr ImVec4 kPanel = ImVec4(0.075f, 0.082f, 0.105f, 1.0f);
constexpr ImVec4 kCard = ImVec4(0.105f, 0.115f, 0.145f, 1.0f);
constexpr ImVec4 kMuted = ImVec4(0.58f, 0.61f, 0.68f, 1.0f);

bool create_device(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL feature_level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc,
        &g_swap_chain, &g_device, &feature_level, &g_device_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc,
            &g_swap_chain, &g_device, &feature_level, &g_device_context);
    }
    return SUCCEEDED(hr);
}

void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    if (g_swap_chain != nullptr &&
        SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
        back_buffer->Release();
    }
}

void cleanup_render_target() {
    if (g_render_target != nullptr) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain != nullptr) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_device_context != nullptr) { g_device_context->Release(); g_device_context = nullptr; }
    if (g_device != nullptr) { g_device->Release(); g_device = nullptr; }
}

void apply_dark_title_bar(HWND window) {
    constexpr DWORD kUseImmersiveDarkMode = 20;
    BOOL enabled = TRUE;
    DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &enabled, sizeof(enabled));
}

LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param)) return true;
    switch (message) {
    case WM_SIZE:
        if (w_param != SIZE_MINIMIZED) {
            g_resize_width = LOWORD(l_param);
            g_resize_height = HIWORD(l_param);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
        info->ptMinTrackSize.x = 980;
        info->ptMinTrackSize.y = 680;
        return 0;
    }
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
    style.WindowPadding = ImVec2(18, 18);
    style.FramePadding = ImVec2(12, 9);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 11;
    style.WindowRounding = 0;
    style.ChildRounding = 14;
    style.FrameRounding = 10;
    style.PopupRounding = 10;
    style.ScrollbarRounding = 10;
    style.GrabRounding = 10;
    style.TabRounding = 9;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;

    auto& c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.98f, 1);
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.049f, 0.064f, 1);
    c[ImGuiCol_ChildBg] = kPanel;
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.085f, 0.11f, 1);
    c[ImGuiCol_Border] = ImVec4(0.18f, 0.19f, 0.24f, 0.72f);
    c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.11f, 0.14f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.15f, 0.19f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.17f, 0.22f, 1);
    c[ImGuiCol_Button] = ImVec4(0.12f, 0.13f, 0.17f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.19f, 0.24f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.22f, 0.28f, 1);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.18f, 0.22f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.22f, 0.28f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.23f, 0.25f, 0.31f, 1);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_NavHighlight] = kAccent;
}

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    g_font_body = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    if (g_font_body != nullptr) io.FontDefault = g_font_body;
    g_font_small = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f);
    g_font_heading = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 24.0f);
    g_font_display = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 31.0f);
}

void text_muted(const char* value) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
}

void heading(const char* value) {
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::TextUnformatted(value);
    if (g_font_heading) ImGui::PopFont();
}

void spinner(const char* label) {
    std::string value(label);
    value.append(static_cast<std::size_t>(static_cast<int>(ImGui::GetTime() * 3.0) % 4), '.');
    text_muted(value.c_str());
}

AudioQuality selected_quality() {
    return static_cast<AudioQuality>(std::clamp(g_quality_index, 0, 3));
}

std::string watch_url(const SearchTrack& track) {
    return track.id.empty() ? std::string{} : "https://www.youtube.com/watch?v=" + track.id;
}

SearchTrack current_as_search_track() {
    SearchTrack track;
    if (!g_current_track) return track;
    track.id = g_current_track->id;
    track.title = g_current_track->title;
    track.channel = g_current_track->channel;
    track.thumbnail_url = g_current_track->thumbnail_url;
    track.duration = g_current_track->duration;
    return track;
}

void save_settings() {
    if (g_store == nullptr) return;
    auto& settings = g_store->settings();
    settings.volume = g_volume;
    settings.quality_index = g_quality_index;
    settings.selected_playlist = g_selected_playlist;
    settings.shuffle = g_shuffle;
    settings.repeat_mode = static_cast<int>(g_repeat);
    std::string error;
    if (!g_store->save(&error) && !error.empty()) g_status = error;
}

ID3D11ShaderResourceView* thumbnail(std::string_view id) {
    return g_thumbnails == nullptr ? nullptr : g_thumbnails->get_or_request(id);
}

ImTextureRef texture_ref(ID3D11ShaderResourceView* texture) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)));
}

void draw_square_thumbnail(std::string_view id, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f, 0.17f, 0.21f, 1)), 9.0f);
    if (ID3D11ShaderResourceView* image = thumbnail(id)) {
        draw->AddImage(texture_ref(image), p, ImVec2(p.x + size, p.y + size),
                       ImVec2(0.21875f, 0.0f), ImVec2(0.78125f, 1.0f));
    } else {
        draw->AddCircleFilled(ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.16f,
                              ImGui::ColorConvertFloat4ToU32(kAccent), 24);
    }
}

void draw_wide_thumbnail(std::string_view id, float width) {
    const float height = width * 9.0f / 16.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.12f, 0.15f, 0.19f, 1)), 14.0f);
    if (ID3D11ShaderResourceView* image = thumbnail(id)) {
        draw->AddImage(texture_ref(image), p, ImVec2(p.x + width, p.y + height));
    } else {
        const ImVec2 center(p.x + width * 0.5f, p.y + height * 0.5f);
        draw->AddCircle(center, height * 0.17f, ImGui::ColorConvertFloat4ToU32(kAccent), 32, 5.0f);
    }
}

bool transport_button(const char* id, TransportIcon icon, float diameter, bool accent) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(diameter, diameter));
    const bool hovered = ImGui::IsItemHovered();
    ImVec4 base = accent ? kAccent : ImVec4(0.14f, 0.15f, 0.19f, 1);
    if (hovered) base = accent ? kAccentHover : ImVec4(0.20f, 0.21f, 0.26f, 1);
    const ImVec2 center(p.x + diameter * 0.5f, p.y + diameter * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(center, diameter * 0.5f, ImGui::ColorConvertFloat4ToU32(base), 40);
    const ImU32 fg = ImGui::ColorConvertFloat4ToU32(
        accent ? ImVec4(0.04f, 0.06f, 0.05f, 1) : ImVec4(0.94f, 0.95f, 0.98f, 1));
    const float r = diameter * 0.19f;
    if (icon == TransportIcon::Play) {
        draw->AddTriangleFilled(ImVec2(center.x - r * 0.55f, center.y - r),
                                ImVec2(center.x - r * 0.55f, center.y + r),
                                ImVec2(center.x + r, center.y), fg);
    } else if (icon == TransportIcon::Pause) {
        draw->AddRectFilled(ImVec2(center.x - r * 0.8f, center.y - r),
                            ImVec2(center.x - r * 0.3f, center.y + r), fg, 2);
        draw->AddRectFilled(ImVec2(center.x + r * 0.3f, center.y - r),
                            ImVec2(center.x + r * 0.8f, center.y + r), fg, 2);
    } else if (icon == TransportIcon::Stop) {
        draw->AddRectFilled(ImVec2(center.x - r * 0.72f, center.y - r * 0.72f),
                            ImVec2(center.x + r * 0.72f, center.y + r * 0.72f), fg, 2);
    } else {
        const bool previous = icon == TransportIcon::Previous;
        const float line_x = center.x + (previous ? -r * 0.92f : r * 0.92f);
        draw->AddLine(ImVec2(line_x, center.y - r), ImVec2(line_x, center.y + r), fg, 2.5f);
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

bool direct_input() {
    const std::string input(g_search_buffer.data());
    return pcyoutube::music::looks_like_http_url(input) ||
           pcyoutube::music::extract_video_id(input).has_value();
}

void start_search(std::string query) {
    if (g_backend == nullptr || query.empty() || g_search_busy.exchange(true)) return;
    if (g_search_thread.joinable()) g_search_thread.join();
    g_search_error.clear();
    g_status = "Searching YouTube...";
    g_search_thread = std::jthread([query = std::move(query)] {
        std::string error;
        auto tracks = g_backend->search(query, 18, error);
        std::lock_guard lock(g_async_mutex);
        g_pending_search = std::move(tracks);
        g_pending_search_error = std::move(error);
        g_search_busy = false;
    });
}

void start_resolve(std::string target, ResolveSource source, int item_index,
                   int playlist_index = -1) {
    if (g_backend == nullptr || target.empty() || g_resolve_busy.exchange(true)) return;
    if (g_resolve_thread.joinable()) g_resolve_thread.join();
    const AudioQuality quality = selected_quality();
    g_manual_stop = false;
    g_status = "Preparing audio...";
    g_resolve_thread = std::jthread([
        target = std::move(target), source, item_index, playlist_index, quality] {
        BackendResult result = g_backend->resolve(target, quality);
        std::lock_guard lock(g_async_mutex);
        g_pending_resolve = PendingResolve{std::move(result), source, item_index, playlist_index};
        g_resolve_busy = false;
    });
}

void start_search_track(int index) {
    if (index < 0 || index >= static_cast<int>(g_search_results.size())) return;
    start_resolve(watch_url(g_search_results[static_cast<std::size_t>(index)]),
                  ResolveSource::Search, index);
}

void start_playlist_track(int playlist, int index) {
    if (g_store == nullptr) return;
    const auto& lists = g_store->playlists();
    if (playlist < 0 || playlist >= static_cast<int>(lists.size())) return;
    const auto& tracks = lists[static_cast<std::size_t>(playlist)].tracks;
    if (index < 0 || index >= static_cast<int>(tracks.size())) return;
    start_resolve(watch_url(tracks[static_cast<std::size_t>(index)]),
                  ResolveSource::Playlist, index, playlist);
}

void apply_async_results() {
    std::optional<std::vector<SearchTrack>> search;
    std::optional<std::string> search_error;
    std::optional<PendingResolve> resolved;
    {
        std::lock_guard lock(g_async_mutex);
        if (g_pending_search) { search = std::move(g_pending_search); g_pending_search.reset(); }
        if (g_pending_search_error) {
            search_error = std::move(g_pending_search_error);
            g_pending_search_error.reset();
        }
        if (g_pending_resolve) { resolved = std::move(g_pending_resolve); g_pending_resolve.reset(); }
    }

    if (search) {
        g_search_results = std::move(*search);
        g_status = g_search_results.empty() ? "No songs found" :
                   std::to_string(g_search_results.size()) + " songs found";
    }
    if (search_error) {
        g_search_error = *search_error;
        if (!g_search_error.empty()) g_status = "Search failed";
    }

    if (!resolved) return;
    if (!resolved->result.ok) {
        g_status = resolved->result.error.empty() ? "Could not resolve audio" : resolved->result.error;
        return;
    }
    if (!g_backend->play(resolved->result.track)) {
        g_status = "Could not start mpv playback";
        return;
    }

    g_backend->set_volume(g_volume);
    g_current_track = std::move(resolved->result.track);
    g_queue_source = resolved->source;
    g_current_search_index = -1;
    g_current_playlist = -1;
    g_current_playlist_track = -1;
    if (resolved->source == ResolveSource::Search) {
        g_current_search_index = resolved->item_index;
    } else if (resolved->source == ResolveSource::Playlist) {
        g_current_playlist = resolved->playlist_index;
        g_current_playlist_track = resolved->item_index;
    }
    g_auto_advance_armed = false;
    g_manual_stop = false;
    g_status = "Playing";
}

PlaybackSnapshot playback_snapshot() {
    std::lock_guard lock(g_playback_mutex);
    return g_playback;
}

void start_poller() {
    g_poll_thread = std::jthread([](std::stop_token token) {
        while (!token.stop_requested()) {
            if (g_backend != nullptr) {
                const PlaybackSnapshot state = g_backend->snapshot();
                std::lock_guard lock(g_playback_mutex);
                g_playback = state;
            }
            for (int i = 0; i < 5 && !token.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

int queue_count() {
    if (g_queue_source == ResolveSource::Search) return static_cast<int>(g_search_results.size());
    if (g_queue_source == ResolveSource::Playlist && g_store != nullptr &&
        g_current_playlist >= 0 && g_current_playlist < static_cast<int>(g_store->playlists().size())) {
        return static_cast<int>(g_store->playlists()[static_cast<std::size_t>(g_current_playlist)].tracks.size());
    }
    return 0;
}

int queue_index() {
    if (g_queue_source == ResolveSource::Search) return g_current_search_index;
    if (g_queue_source == ResolveSource::Playlist) return g_current_playlist_track;
    return -1;
}

void start_queue_index(int index) {
    if (g_queue_source == ResolveSource::Search) start_search_track(index);
    else if (g_queue_source == ResolveSource::Playlist) start_playlist_track(g_current_playlist, index);
}

int shuffled_index() {
    const int count = queue_count();
    const int current = queue_index();
    if (count <= 1) return current;
    std::uniform_int_distribution<int> dist(0, count - 2);
    int value = dist(g_rng);
    if (value >= current) ++value;
    return value;
}

bool can_previous() {
    const int count = queue_count();
    const int index = queue_index();
    return count > 1 && (index > 0 || g_repeat == RepeatMode::All || g_shuffle);
}

bool can_next() {
    const int count = queue_count();
    const int index = queue_index();
    return count > 1 && (index + 1 < count || g_repeat == RepeatMode::All || g_shuffle);
}

void previous_track() {
    if (g_resolve_busy) return;
    const PlaybackSnapshot state = playback_snapshot();
    if (state.position > 4.0 && g_backend != nullptr) {
        g_backend->seek(0.0);
        return;
    }
    const int count = queue_count();
    const int current = queue_index();
    if (count <= 1 || current < 0) return;
    if (g_shuffle) { start_queue_index(shuffled_index()); return; }
    if (current > 0) start_queue_index(current - 1);
    else if (g_repeat == RepeatMode::All) start_queue_index(count - 1);
}

void next_track(bool from_end = false) {
    if (g_resolve_busy) return;
    const int count = queue_count();
    const int current = queue_index();
    if (count <= 0 || current < 0) return;
    if (g_shuffle && count > 1) { start_queue_index(shuffled_index()); return; }
    if (current + 1 < count) { start_queue_index(current + 1); return; }
    if (g_repeat == RepeatMode::All) { start_queue_index(0); return; }
    if (from_end) g_status = "Finished";
}

void handle_auto_advance() {
    const PlaybackSnapshot state = playback_snapshot();
    if (state.running) {
        g_auto_advance_armed = true;
        g_manual_stop = false;
    }

    if (g_previous_running && !state.running && g_auto_advance_armed &&
        !g_manual_stop && !g_resolve_busy && g_current_track) {
        g_auto_advance_armed = false;
        if (g_repeat == RepeatMode::One) {
            if (g_backend != nullptr && g_backend->play(*g_current_track)) {
                g_backend->set_volume(g_volume);
                g_status = "Repeating";
            }
        } else {
            next_track(true);
        }
    }
    g_previous_running = state.running;
}

void request_add(const SearchTrack& track) {
    if (track.id.empty() || track.title.empty()) return;
    g_add_track = track;
    g_open_add_popup = true;
}

bool quality_combo() {
    static const char* labels[] = {"Best source", "Prefer Opus", "Prefer AAC / M4A", "Data saver"};
    ImGui::SetNextItemWidth(178.0f);
    const bool changed = ImGui::Combo("##quality", &g_quality_index, labels, 4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Direct source streams only; bitrate shown after resolve.");
    }
    return changed;
}

void render_search() {
    const bool direct = direct_input();
    ImGui::SetNextItemWidth(-112.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##search", "Search song, artist, album or paste a YouTube URL...",
        g_search_buffer.data(), g_search_buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (g_focus_search) { ImGui::SetKeyboardFocusHere(-1); g_focus_search = false; }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.06f, 0.05f, 1));
    const bool action = ImGui::Button(direct ? "Play URL" : "Search", ImVec2(102, 0));
    ImGui::PopStyleColor(3);
    if (enter || action) {
        const std::string input(g_search_buffer.data());
        if (direct) start_resolve(input, ResolveSource::Direct, -1);
        else start_search(input);
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if (g_search_busy) { spinner("Searching"); return; }
    if (!g_search_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.5f, 1));
        ImGui::TextWrapped("%s", g_search_error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (g_search_results.empty()) {
        text_muted("Search results will appear here.");
        return;
    }

    for (int i = 0; i < static_cast<int>(g_search_results.size()); ++i) {
        const SearchTrack& track = g_search_results[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        const bool current = g_queue_source == ResolveSource::Search && i == g_current_search_index;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, current ? ImVec4(0.12f, 0.19f, 0.16f, 1) : kCard);
        ImGui::BeginChild("result", ImVec2(0, 88), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(14, 13));
        draw_square_thumbnail(track.id, 62);
        ImGui::SameLine(88);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 176);
        ImGui::TextUnformatted(track.title.c_str());
        ImGui::PopTextWrapPos();
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        const std::string meta = track.channel + "  •  " + pcyoutube::music::format_time(track.duration);
        ImGui::TextUnformatted(meta.c_str());
        ImGui::PopStyleColor();
        if (g_font_small) ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetWindowWidth() - 150);
        ImGui::SetCursorPosY(27);
        if (ImGui::Button("Add", ImVec2(56, 34))) request_add(track);
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Play", ImVec2(58, 34))) start_search_track(i);
        ImGui::EndChild();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            start_search_track(i);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void adjust_current_after_move(int playlist, int from, int to) {
    if (g_queue_source != ResolveSource::Playlist || g_current_playlist != playlist) return;
    if (g_current_playlist_track == from) g_current_playlist_track = to;
    else if (from < g_current_playlist_track && g_current_playlist_track <= to) --g_current_playlist_track;
    else if (to <= g_current_playlist_track && g_current_playlist_track < from) ++g_current_playlist_track;
}

void render_playlists() {
    if (g_store == nullptr) return;
    ImGui::SetNextItemWidth(-116.0f);
    ImGui::InputTextWithHint("##new-list", "New playlist name...",
                             g_playlist_name_buffer.data(), g_playlist_name_buffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Create", ImVec2(106, 0))) {
        std::string error;
        const int created = g_store->create(g_playlist_name_buffer.data(), &error);
        if (created >= 0) {
            g_selected_playlist = created;
            g_playlist_name_buffer.fill('\0');
            save_settings();
            g_status = "Playlist created";
        } else if (!error.empty()) g_status = error;
    }

    auto& playlists = g_store->playlists();
    if (playlists.empty()) { ImGui::Spacing(); text_muted("Create a playlist, then add songs from Search."); return; }
    g_selected_playlist = std::clamp(g_selected_playlist, 0, static_cast<int>(playlists.size()) - 1);

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-96.0f);
    if (ImGui::BeginCombo("##list-picker", playlists[static_cast<std::size_t>(g_selected_playlist)].name.c_str())) {
        for (int i = 0; i < static_cast<int>(playlists.size()); ++i) {
            if (ImGui::Selectable(playlists[static_cast<std::size_t>(i)].name.c_str(), i == g_selected_playlist)) {
                g_selected_playlist = i;
                save_settings();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(86, 0))) {
        const int removed = g_selected_playlist;
        std::string error;
        if (g_store->remove(static_cast<std::size_t>(removed), &error)) {
            if (g_current_playlist == removed) {
                g_queue_source = ResolveSource::Direct;
                g_current_playlist = g_current_playlist_track = -1;
            } else if (g_current_playlist > removed) --g_current_playlist;
            g_selected_playlist = g_store->playlists().empty() ? -1 :
                std::min(removed, static_cast<int>(g_store->playlists().size()) - 1);
            save_settings();
            g_status = "Playlist deleted";
        } else if (!error.empty()) g_status = error;
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if (g_selected_playlist < 0 || g_selected_playlist >= static_cast<int>(g_store->playlists().size())) return;
    const Playlist& playlist = g_store->playlists()[static_cast<std::size_t>(g_selected_playlist)];
    if (playlist.tracks.empty()) { text_muted("This playlist is empty."); return; }

    int move_from = -1;
    int move_to = -1;
    int remove_index = -1;
    for (int i = 0; i < static_cast<int>(playlist.tracks.size()); ++i) {
        const SearchTrack track = playlist.tracks[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        const bool current = g_queue_source == ResolveSource::Playlist &&
                             g_current_playlist == g_selected_playlist && g_current_playlist_track == i;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, current ? ImVec4(0.12f, 0.19f, 0.16f, 1) : kCard);
        ImGui::BeginChild("playlist-row", ImVec2(0, 82), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(12, 20));
        if (ImGui::Button("::", ImVec2(30, 34))) {}
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("PCY_TRACK", &i, sizeof(i));
            ImGui::TextUnformatted(track.title.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::SameLine(50);
        ImGui::SetCursorPosY(10);
        draw_square_thumbnail(track.id, 60);
        ImGui::SameLine(120);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 176);
        ImGui::TextUnformatted(track.title.c_str());
        ImGui::PopTextWrapPos();
        if (g_font_small) ImGui::PushFont(g_font_small);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        const std::string meta = track.channel + "  •  " + pcyoutube::music::format_time(track.duration);
        ImGui::TextUnformatted(meta.c_str());
        ImGui::PopStyleColor();
        if (g_font_small) ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetWindowWidth() - 150);
        ImGui::SetCursorPosY(24);
        if (ImGui::Button("Play", ImVec2(58, 34))) start_playlist_track(g_selected_playlist, i);
        ImGui::SameLine(0, 6);
        if (ImGui::Button("Remove", ImVec2(68, 34))) remove_index = i;
        ImGui::EndChild();

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PCY_TRACK")) {
                if (payload->DataSize == sizeof(int)) {
                    move_from = *static_cast<const int*>(payload->Data);
                    move_to = i;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            start_playlist_track(g_selected_playlist, i);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    if (move_from >= 0 && move_to >= 0 && move_from != move_to) {
        std::string error;
        if (g_store->move_track(static_cast<std::size_t>(g_selected_playlist),
                                static_cast<std::size_t>(move_from), static_cast<std::size_t>(move_to), &error)) {
            adjust_current_after_move(g_selected_playlist, move_from, move_to);
            g_status = "Playlist reordered";
        } else if (!error.empty()) g_status = error;
    }
    if (remove_index >= 0) {
        std::string error;
        if (g_store->remove_track(static_cast<std::size_t>(g_selected_playlist),
                                  static_cast<std::size_t>(remove_index), &error)) {
            if (g_queue_source == ResolveSource::Playlist && g_current_playlist == g_selected_playlist) {
                if (g_current_playlist_track == remove_index) {
                    g_queue_source = ResolveSource::Direct;
                    g_current_playlist_track = -1;
                } else if (g_current_playlist_track > remove_index) --g_current_playlist_track;
            }
            g_status = "Removed from playlist";
        } else if (!error.empty()) g_status = error;
    }
}

void render_library(float width, float height) {
    ImGui::BeginChild("Library", ImVec2(width, height), ImGuiChildFlags_Borders);
    heading("Library");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 204);
    if (quality_combo()) save_settings();
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("native thumbnails • direct source audio");
    ImGui::PopStyleColor();
    if (g_font_small) ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Search")) { ImGui::Spacing(); render_search(); ImGui::EndTabItem(); }
        std::string label = "Playlists";
        if (g_store) label += " (" + std::to_string(g_store->playlists().size()) + ")";
        if (ImGui::BeginTabItem(label.c_str())) { ImGui::Spacing(); render_playlists(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

std::string audio_meta(const TrackInfo& track) {
    std::string value;
    if (!track.extension.empty()) value += track.extension;
    if (!track.codec.empty()) { if (!value.empty()) value += "  •  "; value += track.codec; }
    if (track.abr_kbps > 0) {
        if (!value.empty()) value += "  •  ";
        value += std::to_string(static_cast<int>(std::round(track.abr_kbps))) + " kbps";
    }
    if (track.sample_rate_hz > 0) {
        if (!value.empty()) value += "  •  ";
        value += std::to_string(static_cast<int>(std::round(track.sample_rate_hz / 1000.0))) + " kHz";
    }
    return value.empty() ? "Audio stream" : value;
}

void render_now_playing(float width, float height) {
    ImGui::BeginChild("NowPlaying", ImVec2(width, height), ImGuiChildFlags_Borders);
    heading("Now Playing");
    ImGui::Spacing();
    if (!g_current_track) {
        const float art_width = std::min(ImGui::GetContentRegionAvail().x, 330.0f);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - art_width) * 0.5f);
        draw_wide_thumbnail("", art_width);
        ImGui::Spacing();
        text_muted("Choose a song from Search or a saved playlist.");
        ImGui::EndChild();
        return;
    }

    const TrackInfo& track = *g_current_track;
    const float art_width = std::min(ImGui::GetContentRegionAvail().x, 350.0f);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - art_width) * 0.5f);
    draw_wide_thumbnail(track.id, art_width);
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading);
    ImGui::TextWrapped("%s", track.title.c_str());
    if (g_font_heading) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", track.channel.c_str());
    ImGui::PopStyleColor();
    const std::string meta = audio_meta(track);
    text_muted(meta.c_str());

    ImGui::Spacing();
    if (!track.id.empty() && ImGui::Button("Add to playlist")) request_add(current_as_search_track());
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Track details")) {
        if (!track.format_id.empty()) ImGui::Text("Format: %s", track.format_id.c_str());
        if (track.abr_kbps > 0) ImGui::Text("Source bitrate: %.0f kbps", track.abr_kbps);
        if (track.sample_rate_hz > 0) ImGui::Text("Sample rate: %.0f Hz", track.sample_rate_hz);
        ImGui::TextWrapped("Source: %s", track.webpage_url.c_str());
        if (ImGui::Button("Copy direct URL")) {
            ImGui::SetClipboardText(track.direct_url.c_str());
            g_status = "Direct audio URL copied";
        }
    }
    if (g_resolve_busy) { ImGui::Spacing(); spinner("Preparing audio"); }
    ImGui::EndChild();
}

void render_add_popup() {
    if (g_open_add_popup) { ImGui::OpenPopup("Add song to playlist"); g_open_add_popup = false; }
    if (!ImGui::BeginPopupModal("Add song to playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (!g_add_track || g_store == nullptr) {
        text_muted("No song selected.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextWrapped("%s", g_add_track->title.c_str());
    ImGui::Spacing();
    auto& lists = g_store->playlists();
    for (int i = 0; i < static_cast<int>(lists.size()); ++i) {
        ImGui::PushID(i);
        const std::string label = lists[static_cast<std::size_t>(i)].name + "  (" +
                                  std::to_string(lists[static_cast<std::size_t>(i)].tracks.size()) + ")";
        if (ImGui::Button(label.c_str(), ImVec2(340, 0))) {
            std::string error;
            if (g_store->add_track(static_cast<std::size_t>(i), *g_add_track, &error)) {
                g_selected_playlist = i;
                save_settings();
                g_status = "Added to playlist";
                g_add_track.reset();
                ImGui::CloseCurrentPopup();
            } else if (!error.empty()) g_status = error;
        }
        ImGui::PopID();
    }
    if (lists.empty()) text_muted("No playlists yet. Create one below.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##popup-list", "New playlist name...",
                             g_playlist_name_buffer.data(), g_playlist_name_buffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Create + Add")) {
        std::string error;
        const int created = g_store->create(g_playlist_name_buffer.data(), &error);
        if (created >= 0 && g_store->add_track(static_cast<std::size_t>(created), *g_add_track, &error)) {
            g_selected_playlist = created;
            g_playlist_name_buffer.fill('\0');
            save_settings();
            g_status = "Playlist created and song added";
            g_add_track.reset();
            ImGui::CloseCurrentPopup();
        } else if (!error.empty()) g_status = error;
    }
    if (ImGui::Button("Cancel")) { g_add_track.reset(); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void render_mode_buttons(float y) {
    ImGui::SetCursorPos(ImVec2(18, y));
    ImGui::PushStyleColor(ImGuiCol_Button, g_shuffle ? ImVec4(0.12f, 0.22f, 0.17f, 1) : ImVec4(0.12f, 0.13f, 0.17f, 1));
    if (ImGui::Button(g_shuffle ? "Shuffle ON" : "Shuffle", ImVec2(92, 32))) {
        g_shuffle = !g_shuffle;
        save_settings();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const char* repeat_label = g_repeat == RepeatMode::Off ? "Repeat" :
                               g_repeat == RepeatMode::All ? "Repeat ALL" : "Repeat ONE";
    ImGui::PushStyleColor(ImGuiCol_Button, g_repeat == RepeatMode::Off ?
                          ImVec4(0.12f, 0.13f, 0.17f, 1) : ImVec4(0.12f, 0.22f, 0.17f, 1));
    if (ImGui::Button(repeat_label, ImVec2(104, 32))) {
        g_repeat = static_cast<RepeatMode>((static_cast<int>(g_repeat) + 1) % 3);
        save_settings();
    }
    ImGui::PopStyleColor();
}

void render_player(float height) {
    ImGui::BeginChild("Player", ImVec2(0, height), ImGuiChildFlags_Borders);
    const PlaybackSnapshot state = playback_snapshot();
    const double duration = state.duration > 0 ? state.duration : (g_current_track ? g_current_track->duration : 0.0);
    const float maximum = static_cast<float>(std::max(0.001, duration));
    if (!g_seek_dragging) g_seek_value = static_cast<float>(state.position);

    const std::string left = pcyoutube::music::format_time(g_seek_dragging ? g_seek_value : state.position);
    const std::string right = pcyoutube::music::format_time(duration);
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::TextUnformatted(left.c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 18);
    ImGui::TextUnformatted(right.c_str());
    if (g_font_small) ImGui::PopFont();

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##progress", &g_seek_value, 0, maximum, "", ImGuiSliderFlags_NoInput);
    if (ImGui::IsItemActivated()) g_seek_dragging = true;
    if (g_seek_dragging && ImGui::IsItemDeactivatedAfterEdit()) {
        if (g_backend) g_backend->seek(g_seek_value);
        g_seek_dragging = false;
    }

    const float controls_y = std::max(ImGui::GetCursorPosY() + 8, height - 78.0f);
    const float center_width = 42 + 14 + 58 + 14 + 42 + 14 + 42;
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - center_width) * 0.5f, controls_y));
    ImGui::BeginDisabled(!can_previous() || g_resolve_busy);
    if (transport_button("##prev", TransportIcon::Previous, 42, false)) previous_track();
    ImGui::EndDisabled();
    ImGui::SameLine(0, 14);
    const TransportIcon icon = state.running && !state.paused ? TransportIcon::Pause : TransportIcon::Play;
    if (transport_button("##play", icon, 58, true) && g_backend) {
        if (state.running) g_backend->toggle_pause();
        else if (g_current_track) {
            g_manual_stop = false;
            g_backend->play(*g_current_track);
            g_backend->set_volume(g_volume);
        } else if (!g_search_results.empty()) start_search_track(0);
    }
    ImGui::SameLine(0, 14);
    ImGui::BeginDisabled(!can_next() || g_resolve_busy);
    if (transport_button("##next", TransportIcon::Next, 42, false)) next_track(false);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 14);
    if (transport_button("##stop", TransportIcon::Stop, 42, false) && g_backend) {
        g_manual_stop = true;
        g_auto_advance_armed = false;
        g_backend->stop();
        g_status = "Stopped";
    }

    render_mode_buttons(controls_y + 10);

    const float volume_width = 180;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - volume_width - 18, controls_y + 9));
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::TextUnformatted("VOL");
    if (g_font_small) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(volume_width - 54);
    int volume = g_volume;
    if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d")) {
        g_volume = volume;
        if (g_backend) g_backend->set_volume(g_volume);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();

    ImGui::SetCursorPos(ImVec2(18, controls_y - 22));
    if (g_font_small) ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(g_status.c_str());
    ImGui::PopStyleColor();
    if (g_font_small) ImGui::PopFont();
    ImGui::EndChild();
}

void render_app() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    ImGui::Begin("PcYoutubeRoot", nullptr, flags);
    if (g_font_display) ImGui::PushFont(g_font_display);
    ImGui::TextUnformatted("PcYoutube Music");
    if (g_font_display) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
    text_muted("native audio player • v0.6");
    ImGui::Spacing();

    const float player_height = 172;
    const float content_height = std::max(300.0f, ImGui::GetContentRegionAvail().y - player_height - 10);
    const float available = ImGui::GetContentRegionAvail().x;
    const float left = std::max(500.0f, available * 0.62f);
    const float right = std::max(350.0f, available - left - 10);
    render_library(left, content_height);
    ImGui::SameLine();
    render_now_playing(right, content_height);
    ImGui::Spacing();
    render_player(player_height);
    render_add_popup();
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (std::wcsstr(GetCommandLineW(), L"--self-test") != nullptr)
        return pcyoutube::music::self_test() ? 0 : 1;

    HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResourceId),
                                               IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    HICON small_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResourceId),
                                                     IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.hIconSm = small_icon;
    wc.lpszClassName = L"PcYoutubeMusicImGui";
    RegisterClassExW(&wc);

    HWND window = CreateWindowW(wc.lpszClassName, L"PcYoutube Music",
                                WS_OVERLAPPEDWINDOW, 100, 70, 1280, 840,
                                nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 2;
    apply_dark_title_bar(window);
    if (!create_device(window)) {
        DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, instance);
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
    PlaylistStore store;
    ThumbnailCache thumbnails(g_device);
    g_backend = &backend;
    g_store = &store;
    g_thumbnails = &thumbnails;

    std::string load_error;
    if (!store.load(&load_error) && !load_error.empty()) g_status = load_error;
    const auto& settings = store.settings();
    g_volume = settings.volume;
    g_quality_index = settings.quality_index;
    g_selected_playlist = settings.selected_playlist;
    g_shuffle = settings.shuffle;
    g_repeat = static_cast<RepeatMode>(std::clamp(settings.repeat_mode, 0, 2));
    if (!backend.ready()) g_status = backend.readiness_error();
    start_poller();

    bool done = false;
    while (!done) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resize_width && g_resize_height) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }

        apply_async_results();
        handle_auto_advance();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        render_app();
        ImGui::Render();

        const float clear[4] = {0.045f, 0.049f, 0.064f, 1};
        g_device_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        g_device_context->ClearRenderTargetView(g_render_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    save_settings();
    if (g_poll_thread.joinable()) { g_poll_thread.request_stop(); g_poll_thread.join(); }
    if (g_search_thread.joinable()) g_search_thread.join();
    if (g_resolve_thread.joinable()) g_resolve_thread.join();
    backend.shutdown();
    g_thumbnails = nullptr;
    g_store = nullptr;
    g_backend = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, instance);
    if (icon) DestroyIcon(icon);
    if (small_icon) DestroyIcon(small_icon);
    return 0;
}
