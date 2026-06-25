#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/audio_source.hpp"
#include "frdx/sources/shoutcast_source.hpp"
#include <algorithm>
#include <cstring>

namespace frdx::overlay {
using namespace frdx;

bool has_visible_toasts() {
    const auto now = std::chrono::steady_clock::now();
    return std::ranges::any_of(g_ui.toasts, [&](const Toast& toast) {
        return now - toast.created < std::chrono::duration<float>(toast.lifetime);
    });
}

void push_toast(ToastKind kind, std::string title, std::string detail, float lifetime) {
    Toast toast;
    toast.kind     = kind;
    toast.title    = std::move(title);
    toast.detail   = std::move(detail);
    toast.created  = std::chrono::steady_clock::now();
    toast.lifetime = lifetime;
    if (g_ui.toasts.size() >= 4) g_ui.toasts.pop_front();
    g_ui.toasts.push_back(std::move(toast));
}

ImVec4 toast_color(ToastKind kind) {
    switch (kind) {
        case ToastKind::success: return ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
        case ToastKind::warn: return ImVec4(1.0f, 0.72f, 0.35f, 1.0f);
        case ToastKind::info: break;
    }
    return ImVec4(0.55f, 0.78f, 1.0f, 1.0f);
}

bool is_status_artist(std::string_view artist) noexcept {
    return artist.empty() || artist == "Select a station" || artist == "Nothing playing" ||
           artist == "Attempting to reconnect to station" ||
           artist == "Unable to reach this station" || artist == "Lost connection to station";
}

bool is_station_display_track(IAudioSource* active, std::string_view title,
                              std::string_view artist) {
    if (!active || active->type() != SourceType::shoutcast) return false;
    auto* sc                                   = static_cast<sources::ShoutcastSource*>(active);
    const auto [active_station, active_stream] = sc->active_station_stream();
    for (const auto& station : sc->stations_nolock()) {
        if (station.id != active_station) continue;
        if (title == station.name) return true;
        for (const auto& stream : station.streams) {
            if (stream.id == active_stream && (artist == stream.name || title == stream.name)) {
                return true;
            }
        }
        break;
    }
    return false;
}

bool should_notify_song_change(IAudioSource* active, const TrackInfo& track, std::string_view title,
                               std::string_view artist) {
    if (!active || title.empty() || is_status_artist(artist)) return false;
    if (track.title.empty()) return false;
    if (is_station_display_track(active, title, artist)) return false;
    return true;
}

void draw_toasts() {
    const auto now = std::chrono::steady_clock::now();
    while (!g_ui.toasts.empty() && now - g_ui.toasts.front().created >=
                                       std::chrono::duration<float>(g_ui.toasts.front().lifetime)) {
        g_ui.toasts.pop_front();
    }
    if (g_ui.toasts.empty()) return;

    auto* viewport       = ImGui::GetMainViewport();
    const ImVec2 vp_pos  = viewport->WorkPos;
    const ImVec2 vp_size = viewport->WorkSize;
    const int pos_idx    = std::clamp(g_ui.notification_position_idx, 0, 5);
    const bool top       = pos_idx <= 2;
    const bool left      = pos_idx == 0 || pos_idx == 3;
    const bool center    = pos_idx == 1 || pos_idx == 4;
    const bool right     = pos_idx == 2 || pos_idx == 5;
    ImVec2 anchor;
    ImVec2 pivot;
    switch (pos_idx) {
        case 1:
            anchor = ImVec2(vp_pos.x + vp_size.x * 0.5f, vp_pos.y + 28.0f);
            pivot  = ImVec2(0.5f, 0.0f);
            break;
        case 2:
            anchor = ImVec2(vp_pos.x + vp_size.x - 18.0f, vp_pos.y + 28.0f);
            pivot  = ImVec2(1.0f, 0.0f);
            break;
        case 3:
            anchor = ImVec2(vp_pos.x + 18.0f, vp_pos.y + vp_size.y - 28.0f);
            pivot  = ImVec2(0.0f, 1.0f);
            break;
        case 4:
            anchor = ImVec2(vp_pos.x + vp_size.x * 0.5f, vp_pos.y + vp_size.y - 28.0f);
            pivot  = ImVec2(0.5f, 1.0f);
            break;
        case 5:
            anchor = ImVec2(vp_pos.x + vp_size.x - 18.0f, vp_pos.y + vp_size.y - 28.0f);
            pivot  = ImVec2(1.0f, 1.0f);
            break;
        default:
            anchor = ImVec2(vp_pos.x + 18.0f, vp_pos.y + 28.0f);
            pivot  = ImVec2(0.0f, 0.0f);
            break;
    }
    float y = anchor.y;
    int idx = 0;

    for (auto it = g_ui.toasts.rbegin(); it != g_ui.toasts.rend(); ++it, ++idx) {
        const float age     = std::chrono::duration<float>(now - it->created).count();
        const float enter_t = std::clamp(age / 0.60f, 0.0f, 1.0f);
        const float exit_t  = std::clamp((it->lifetime - age) / 0.60f, 0.0f, 1.0f);
        const float enter   = iam_eval_preset(iam_ease_out_back, enter_t);
        const float exit    = iam_eval_preset(iam_ease_out_quad, exit_t);
        const float alpha   = std::clamp(std::min(enter, exit), 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;

        ImVec2 slide{};
        if (center) {
            slide.y = (top ? -1.0f : 1.0f) * (1.0f - enter) * 44.0f;
        } else if (left) {
            slide.x = -(1.0f - enter) * 72.0f;
        } else if (right) {
            slide.x = (1.0f - enter) * 72.0f;
        }
        slide.y += (top ? 1.0f : -1.0f) * (1.0f - enter) * 6.0f * static_cast<float>(idx);

        ImGui::SetNextWindowBgAlpha(0.88f * alpha);
        ImGui::SetNextWindowPos(ImVec2(anchor.x + slide.x, y + slide.y), ImGuiCond_Always, pivot);
        ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f), ImVec2(420.0f, 180.0f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoMove;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        char window_name[32];
        std::snprintf(window_name, sizeof(window_name), "##frdx_toast_%d", idx);
        ImGui::Begin(window_name, nullptr, flags);
        ImGui::PushTextWrapPos(390.0f);
        ImGui::TextColored(toast_color(it->kind), "%s", it->title.c_str());
        if (!it->detail.empty()) {
            ImGui::TextWrapped("%s", it->detail.c_str());
        }
        ImGui::PopTextWrapPos();
        y += (top ? 1.0f : -1.0f) * (ImGui::GetWindowSize().y + 8.0f);
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

} // namespace frdx::overlay
