#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/log.hpp"

#include <algorithm>

namespace frdx::overlay {
using namespace frdx;

// Subsystem prefix wins over ERROR/WARN keywords so a noisy subsystem can
// be muted independently.  Without this, "[overlay] WARN foo" would be
// classified as warn and the user's "Overlay" checkbox could not suppress
// it, defeating the per-category filter.  The four `[prefix]` checks run
// before the keyword tests; the prefix -> category mapping mirrors the
// distinct colour the user has set for each in the renderer below.
ConsoleLineKind classify_console_line(std::string_view line) {
    if (line.find("[overlay]") != std::string_view::npos) return ConsoleLineKind::overlay;
    if (line.find("[dualsense]") != std::string_view::npos) return ConsoleLineKind::dualsense;
    if (line.find("[discovery]") != std::string_view::npos) return ConsoleLineKind::radio;
    if (line.find("[ctrl]") != std::string_view::npos) return ConsoleLineKind::radio;
    if (line.find("[fmod]") != std::string_view::npos) return ConsoleLineKind::radio;
    if (line.find("ERROR") != std::string_view::npos) return ConsoleLineKind::error;
    if (line.find("WARN") != std::string_view::npos) return ConsoleLineKind::warn;
    return ConsoleLineKind::normal;
}

ImVec4 console_line_color(ConsoleLineKind kind) {
    switch (kind) {
        case ConsoleLineKind::error: return ImVec4(1.0f, 0.38f, 0.38f, 1.0f);
        case ConsoleLineKind::warn: return ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
        case ConsoleLineKind::overlay: return ImVec4(0.55f, 0.78f, 1.0f, 1.0f);
        case ConsoleLineKind::dualsense: return ImVec4(0.64f, 0.92f, 0.72f, 1.0f);
        case ConsoleLineKind::radio: return ImVec4(0.95f, 0.78f, 0.50f, 1.0f);
        case ConsoleLineKind::normal: break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

void draw_console_window() {
    const bool opening = g_ui.console_visible;
    const float eased  = advance_popup_anim(opening, g_ui.console_anim);
    if (!opening && g_ui.console_anim < 0.01f) {
        g_ui.console_anim = 0.0f;
        return;
    }

    auto* viewport       = ImGui::GetMainViewport();
    const ImVec2 vp_pos  = viewport->WorkPos;
    const ImVec2 vp_size = viewport->WorkSize;
    const ImVec2 size(std::min(760.0f, vp_size.x - 32.0f), std::min(360.0f, vp_size.y - 80.0f));
    ImGui::SetNextWindowPos(ImVec2(vp_pos.x + vp_size.x - 16.0f, vp_pos.y + vp_size.y - 16.0f),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    // Solid background; do NOT multiply by interactive_draw_alpha because
    // draw_console_window() is rendered outside the cursor-fade alpha pushvar
    // (so the log window stays visible when the rest of the overlay fades).
    ImGui::SetNextWindowBgAlpha(0.94f * eased);

    // Titlebar restored so the native X close button + title row are visible
    // (clean close-X on the right, F10 toggle, or per-frame QueueClose).
    // The filter row lives just below the titlebar and lets you mute the
    // routine FMOD / dualsense / discovery chatter for a clean RE console.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    // The native titlebar's X button sets *p_open = false on click instead
    // of collapsing the window to a thumbnail.  We seed local_open = true so
    // it acts as a per-frame *signal* (only flips false if the X is
    // actually clicked during this frame).  See code-review thread
    // "advance_popup_anim+fade-out sentinel" for the full reasoning.
    bool local_open = true;
    // NoScrollbar — the BeginChild log scroll below already provides one;
    // without this flag the outer window grows its own (redundant) scrollbar
    // whenever the wrapped log content exceeds the window's content rect.
    if (ImGui::Begin("FRDX Console##frdx_console", &local_open,
                     flags | ImGuiWindowFlags_NoScrollbar)) {
        // Backdrop blur does not fade with the cursor here — the console
        // window stays readable while other overlay content fades out.
        draw_backdrop_blur(1.0f);

        // ── Refresh from the global logger (with optional dedup). ────
        // Two triggers:
        //   (1) log::recent_generation() bumped (a new line was emitted).
        //   (2) the dedup toggle flipped since the last fetch.
        // Both rebuild console_lines + console_line_kinds + console_line_counts.
        // The dedup pass collapses *consecutive identical lines* into a
        // single entry whose `count` repeats-value is rendered as "(×N)".
        static bool s_last_dedup_state = false;
        const uint64_t log_generation  = log::recent_generation();
        const bool dedup = g_ui.console_filter_dedup;
        if (g_ui.console_log_generation != log_generation ||
            s_last_dedup_state != dedup) {
            const auto raw = log::recent_lines();
            if (dedup) {
                g_ui.console_lines.clear();
                g_ui.console_line_kinds.clear();
                g_ui.console_line_counts.clear();
                for (const auto& line : raw) {
                    if (!g_ui.console_lines.empty() && g_ui.console_lines.back() == line) {
                        ++g_ui.console_line_counts.back();
                    } else {
                        g_ui.console_lines.push_back(line);
                        g_ui.console_line_kinds.push_back(classify_console_line(line));
                        g_ui.console_line_counts.push_back(1);
                    }
                }
            } else {
                g_ui.console_lines.assign(raw.begin(), raw.end());
                g_ui.console_line_kinds.clear();
                g_ui.console_line_kinds.reserve(raw.size());
                g_ui.console_line_counts.assign(raw.size(), 1);
                for (const auto& line : raw) {
                    g_ui.console_line_kinds.push_back(classify_console_line(line));
                }
            }
            g_ui.console_log_generation = log_generation;
            s_last_dedup_state          = dedup;
        }

        // Resolve a single line's visibility from the user's filter toggles.
        // All categories (errors / warnings / overlay / dualsense / radio /
        // normal) are now user-toggleable; the dedup toggle is independent.
        auto visible = [](ConsoleLineKind k) {
            switch (k) {
                case ConsoleLineKind::error:     return g_ui.console_filter_errors;
                case ConsoleLineKind::warn:      return g_ui.console_filter_warnings;
                case ConsoleLineKind::overlay:   return g_ui.console_filter_overlay;
                case ConsoleLineKind::dualsense: return g_ui.console_filter_dualsense;
                case ConsoleLineKind::radio:     return g_ui.console_filter_radio;
                case ConsoleLineKind::normal:    return g_ui.console_filter_normal;
            }
            return true;
        };

        // Pre-frame counts — computed once so the titlebar filter row can
        // render "X / Y recent" inline (right-aligned next to the Clear
        // button) and the right-click popup's "Copy visible (X / Y)" carries
        // accurate totals even when the popup fires mid-render.
        const size_t total_lines = g_ui.console_lines.size();
        size_t shown_count      = 0;
        for (size_t i = 0; i < g_ui.console_lines.size(); ++i) {
            const ConsoleLineKind k = i < g_ui.console_line_kinds.size()
                                      ? g_ui.console_line_kinds[i]
                                      : ConsoleLineKind::normal;
            if (visible(k)) ++shown_count;
        }

        // ── Filter row (under the native titlebar) ───────────────────
        // Seven toggles in left-to-right reading order: errors / warnings /
        // overlay / dualsense / radio / normal / dedup.  Defaults match
        // 8c22af6 except "normal" was previously always-on — now toggleable
        // so the user can clear the catch-all noise (audio / DSP / config
        // chatter without `[bracket]` prefixes).
        ImGui::Checkbox("Errors", &g_ui.console_filter_errors);
        ImGui::SameLine();
        ImGui::Checkbox("Warnings", &g_ui.console_filter_warnings);
        ImGui::SameLine();
        ImGui::Checkbox("Overlay", &g_ui.console_filter_overlay);
        ImGui::SameLine();
        ImGui::Checkbox("DualSense", &g_ui.console_filter_dualsense);
        ImGui::SameLine();
        ImGui::Checkbox("Radio", &g_ui.console_filter_radio);
        ImGui::SameLine();
        ImGui::Checkbox("Normal", &g_ui.console_filter_normal);
        ImGui::SameLine();
        ImGui::Checkbox("Dedup", &g_ui.console_filter_dedup);

        // Right-aligned "X / Y recent" + Clear button.  Both live in the
        // titlebar region now (per the user request) instead of as a
        // footer below the log scroll.  SameLine(end_x) anchors the cursor
        // to the row's right edge minus the controls we'll draw — keeps
        // everything on one row regardless of how many checkboxes are on
        // the left.
        const std::string counter_text =
            std::to_string(shown_count) + " / " + std::to_string(total_lines) + " recent";
        const float counter_w    = ImGui::CalcTextSize(counter_text.c_str()).x;
        const float clear_w      = ImGui::CalcTextSize("Clear").x +
                                   ImGui::GetStyle().FramePadding.x * 2.0f;
        const float row_max_x    = ImGui::GetContentRegionMax().x;
        ImGui::SameLine(row_max_x - clear_w - counter_w - 8.0f);
        ImGui::TextDisabled("%s", counter_text.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            log::clear_recent();
            // Bump our cached generation to an unreachable value so the
            // refresh branch above re-runs even if no new log lines
            // arrive between the Clear and the next frame.
            g_ui.console_log_generation       = UINT64_MAX;
            g_ui.console_lines.clear();
            g_ui.console_line_kinds.clear();
            g_ui.console_line_counts.clear();
        }

        // ── Per-line copy helpers (right-click popup bodies) ─────────
        // Both helpers walk `console_lines` and apply the same filter
        // the user already sees in the panel, so the joined text matches
        // exactly what is visible.
        auto copy_visible_to_clipboard = [&] {
            std::string joined;
            size_t capacity = 0;
            for (size_t i = 0; i < g_ui.console_lines.size(); ++i) {
                const ConsoleLineKind k = i < g_ui.console_line_kinds.size()
                                          ? g_ui.console_line_kinds[i]
                                          : ConsoleLineKind::normal;
                if (!visible(k)) continue;
                capacity += g_ui.console_lines[i].size() + 1;
            }
            joined.reserve(capacity);
            for (size_t i = 0; i < g_ui.console_lines.size(); ++i) {
                const ConsoleLineKind k = i < g_ui.console_line_kinds.size()
                                          ? g_ui.console_line_kinds[i]
                                          : ConsoleLineKind::normal;
                if (!visible(k)) continue;
                joined += g_ui.console_lines[i];
                joined += '\n';
            }
            ImGui::SetClipboardText(joined.c_str());
        };
        auto copy_all_to_clipboard = [&] {
            std::string joined;
            size_t capacity = 0;
            for (const auto& ln : g_ui.console_lines) capacity += ln.size() + 1;
            joined.reserve(capacity);
            for (const auto& ln : g_ui.console_lines) {
                joined += ln;
                joined += '\n';
            }
            ImGui::SetClipboardText(joined.c_str());
        };

        ImGui::Separator();
        ImGui::BeginChild("##console_scroll", ImVec2(0.0f, 0.0f), false);
        const bool stick_to_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        // Compute the wrap target once per frame — every TextUnformatted()
        // call below uses it so long log lines wrap at the panel width
        // instead of overflowing horizontally.  GetContentRegionAvail().x is
        // the inner width of the BeginChild above (panel minus scrollbar).
        const float wrap_width = ImGui::GetContentRegionAvail().x;
        for (size_t line_idx = 0; line_idx < g_ui.console_lines.size(); ++line_idx) {
            const ConsoleLineKind kind = line_idx < g_ui.console_line_kinds.size()
                                           ? g_ui.console_line_kinds[line_idx]
                                           : ConsoleLineKind::normal;
            if (!visible(kind)) continue;
            const auto& line = g_ui.console_lines[line_idx];
            const size_t count = line_idx < g_ui.console_line_counts.size()
                                   ? g_ui.console_line_counts[line_idx]
                                   : 1u;
            // Compose display string with "(×N)" suffix when dedup mode
            // collapsed repetitions.  The suffix is purely visual — double-
            // click + right-click copies still hand back the unwrapped text
            // so clipboard output remains clean for the user.
            std::string displayed;
            displayed.reserve(line.size() + 12);
            displayed.append(line);
            if (count > 1) {
                displayed.append(" (×");
                displayed.append(std::to_string(count));
                displayed.push_back(')');
            }
            ImGui::PushID(static_cast<int>(line_idx));
            ImGui::PushStyleColor(ImGuiCol_Text, console_line_color(kind));
            // PushTextWrapPos lets TextUnformatted wrap a long line at our
            // precomputed wrap_width boundary.  See 2941e3d / 8c22af6 for the
            // reason Selectable was the wrong choice here.  The wrapped
            // block is a single ImGui item whose bounding rect spans all
            // wrapped lines, so per-line interactivity (double-click to
            // copy, right-click context menu) remains intact.
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);
            ImGui::TextUnformatted(displayed.c_str(), nullptr);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ImGui::SetClipboardText(line.c_str());
            }
            if (ImGui::BeginPopupContextItem("##line_menu")) {
                if (ImGui::MenuItem("Copy line")) {
                    ImGui::SetClipboardText(line.c_str());
                }
                ImGui::Separator();
                const std::string cv_label = "Copy visible (" +
                                             std::to_string(shown_count) + " / " +
                                             std::to_string(total_lines) + ")";
                const std::string ca_label =
                    "Copy all (" + std::to_string(total_lines) + ")";
                if (ImGui::MenuItem(cv_label.c_str(), nullptr, false, shown_count > 0)) {
                    copy_visible_to_clipboard();
                }
                if (ImGui::MenuItem(ca_label.c_str(), nullptr, false, total_lines > 0)) {
                    copy_all_to_clipboard();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (stick_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        mark_overlay_mouse_region();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    if (!local_open) {
        g_ui.console_visible.store(false, std::memory_order_release);
    }
}

} // namespace frdx::overlay
