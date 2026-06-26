#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include "frdx/log.hpp"

#include <algorithm>
#include <cstdio>

namespace frdx::overlay {
using namespace frdx;

// Subsystem prefix wins over ERROR/WARN keywords so a noisy subsystem can
// be muted independently.  Without this, "[overlay] WARN foo" would be
// classified as warn and the user's "Overlay" checkbox could not suppress
// it, defeating the per-category filter.  In the subsystem-filter rework
// classify_console_line is UNCHANGED: it still drives the 7 coarse category
// checkboxes.  The per-subsystem filter is *additive* and is evaluated by
// passes_filter() against a precomputed id parallel to console_lines.
//
// This split is intentional: classify_console_line has the right priority
// order (subsystem-prefix-before-keyword) for KIND classification, while
// passes_filter applies the user-facing visibility check that needs the
// actual subsystem name (not just one of 5 coarse buckets).
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

// Pull the FIRST `[bracket]` token out of the line if present.  Callers
// emit `log::info("[shoutcast] foo")` so in practice each line has exactly
// one `[bracket]`.  Caps at 32 chars so a pathological "[…]" can't bloat
// the subsystem registry's string keys.  Lines with no `[bracket]` always
// pass the per-subsystem filter (UINT16_MAX = no subsystem).
std::string_view extract_subsystem(std::string_view line) {
    const auto open = line.find('[');
    if (open == std::string_view::npos) return {};
    const auto close = line.find(']', open + 1);
    if (close == std::string_view::npos) return {};
    if (close - open - 1 > 32) return {};
    return line.substr(open + 1, close - open - 1);
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
    ImGui::SetNextWindowBgAlpha(0.94f * eased);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
    bool local_open = true;
    if (ImGui::Begin("FRDX Console##frdx_console", &local_open, flags)) {
        draw_backdrop_blur(1.0f);

        // ── Refresh from the global logger (with optional dedup). ────
        // When log generation bumps OR the dedup toggle flips, we rebuild
        // console_lines / line_kinds / line_counts AND incrementally fold
        // the durable subsystem registry.  The registry never shrinks so
        // muted subsystems that fall out of the 500-line buffer stay
        // visible in the popup.
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

            // Walk the freshly-refreshed console_lines and assign each line
            // a stable subsystem id (UINT16_MAX = no subsystem).  New ids
            // get appended to console_subsystems (visible=true).  This runs
            // ONLY on refresh (typically <once per second-frame burst),
            // and is the only place a std::string allocation happens in the
            // subsystem pipeline — passes_filter() never allocates.
            auto& v  = g_ui.console_subsystems;
            auto& ix = g_ui.console_subsystem_index;
            auto& id = g_ui.console_subsystem_id;
            id.assign(g_ui.console_lines.size(), UINT16_MAX);
            for (size_t i = 0; i < g_ui.console_lines.size(); ++i) {
                const auto s = extract_subsystem(g_ui.console_lines[i]);
                if (s.empty()) continue;
                // One std::string per distinct subsystem per refresh.
                std::string key(s);
                auto it = ix.find(key);
                if (it == ix.end()) {
                    const uint16_t new_id = static_cast<uint16_t>(v.size());
                    v.push_back({std::move(key), true});
                    it = ix.emplace(v.back().name, new_id).first;
                }
                id[i] = it->second;
            }

            g_ui.console_log_generation = log_generation;
            s_last_dedup_state          = dedup;
        }

        // Per-kind category gate (the 7 coarse checkboxes in the filter
        // row).  Per-subsystem + text-search filters are evaluated by
        // passes_filter below because they're per-line and don't map to
        // a category enum.
        auto visible_kind = [](ConsoleLineKind k) {
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
        // Combined per-line filter: category ∧ subsystem ∧ text-search.
        // Zero heap allocation — subsystem is one index lookup, text is
        // one strchr/strstr over the cached buffer.
        auto passes_filter = [&](size_t line_idx) -> bool {
            const ConsoleLineKind k = line_idx < g_ui.console_line_kinds.size()
                                          ? g_ui.console_line_kinds[line_idx]
                                          : ConsoleLineKind::normal;
            if (!visible_kind(k)) return false;
            const uint16_t sid = line_idx < g_ui.console_subsystem_id.size()
                                     ? g_ui.console_subsystem_id[line_idx]
                                     : UINT16_MAX;
            if (sid != UINT16_MAX && !g_ui.console_subsystems[sid].visible) return false;
            if (g_ui.console_filter_text_search[0] != '\0' &&
                g_ui.console_lines[line_idx].find(g_ui.console_filter_text_search)
                    == std::string_view::npos) {
                return false;
            }
            return true;
        };

        // Pre-frame counts.  Drive the titlebar "X / Y recent" counter and
        // the right-click "Copy visible" label — uses passes_filter so
        // subsystem + text-search mutes out properly.
        const size_t total_lines = g_ui.console_lines.size();
        size_t shown_count      = 0;
        for (size_t i = 0; i < g_ui.console_lines.size(); ++i) {
            if (passes_filter(i)) ++shown_count;
        }

        // Compute subsystem-popup label state once per frame so we don't
        // walk the registry twice (once for the label and once for the
        // popup body).
        size_t visible_sub_count = 0;
        size_t muted_sub_count   = 0;
        for (const auto& s : g_ui.console_subsystems) {
            if (s.visible) ++visible_sub_count;
            else ++muted_sub_count;
        }

        // ── Filter row (under the native titlebar) ───────────────────
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
        ImGui::SameLine();

        // Subsystems (V / M muted) — durable table across frames; muted
        // subsystems that fall out of the 500-line buffer stay here so the
        // user can re-enable them after the scan-flood passes.  Sorted
        // alphabetically inside the popup for stable presentation order.
        char subsys_label[64];
        std::snprintf(subsys_label, sizeof(subsys_label),
                      muted_sub_count > 0
                          ? "Subsystems (%zu / %zu muted)##console_subsys_btn"
                          : "Subsystems (%zu)##console_subsys_btn",
                      visible_sub_count, muted_sub_count);
        if (ImGui::SmallButton(subsys_label)) ImGui::OpenPopup("##subsys_popup");
        if (ImGui::BeginPopup("##subsys_popup")) {
            if (g_ui.console_subsystems.empty()) {
                // Two distinct empty states so the user isn't confused
                // between an empty log buffer and a buffer of lines that
                // simply lack `[bracket]` prefixes.
                if (g_ui.console_lines.empty()) {
                    ImGui::TextDisabled("(no log lines)");
                } else {
                    ImGui::TextDisabled("(no subsystem-tagged lines)");
                }
            } else {
                // Sort pointers (not entries) so we don't disturb the
                // stable id ordering used by parallel `console_subsystem_id`.
                std::vector<SubsystemState*> sorted;
                sorted.reserve(g_ui.console_subsystems.size());
                for (auto& s : g_ui.console_subsystems) sorted.push_back(&s);
                std::sort(sorted.begin(), sorted.end(),
                          [](const SubsystemState* a, const SubsystemState* b) {
                              return a->name < b->name;
                          });
                for (auto* s : sorted) {
                    char item[48];
                    std::snprintf(item, sizeof(item), "[%s]%s", s->name.c_str(),
                                  s->visible ? "" : " (muted)");
                    ImGui::MenuItem(item, nullptr, &s->visible);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        // Substring search.  Width 180 px fits beside the rest of the
        // controls at the typical 760-wide console window.
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##console_search", "Search...", g_ui.console_filter_text_search,
                                 sizeof(g_ui.console_filter_text_search));
        ImGui::SameLine();
        // Pause auto-scroll — when ticked, new log lines still arrive but
        // scroll position is left alone.  Renamed from "Pause" so the
        // intent is obvious (we have other things that auto-scroll).
        ImGui::Checkbox("Pause auto-scroll", &g_ui.console_filter_pause_scroll);

        // Right-aligned counter + Clear button.
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
            g_ui.console_log_generation       = UINT64_MAX;
            g_ui.console_lines.clear();
            g_ui.console_line_kinds.clear();
            g_ui.console_line_counts.clear();
            g_ui.console_subsystem_id.assign(0, UINT16_MAX);
        }

        // ── Per-line copy helpers (right-click popup bodies) ─────────
        // copy_visible applies the same filter the user sees in the panel;
        // copy_all joins every line regardless of state.
        auto copy_visible_to_clipboard = [&] {
            std::string joined;
            size_t capacity = 0;
            for (size_t i = 0; i < g_ui.console_lines.size(); ++i) {
                if (passes_filter(i)) capacity += g_ui.console_lines[i].size() + 1;
            }
            joined.reserve(capacity);
            for (size_t i = 0; i < g_ui.console_lines.size(); ++i) {
                if (!passes_filter(i)) continue;
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
        const float wrap_width = ImGui::GetContentRegionAvail().x;
        for (size_t line_idx = 0; line_idx < g_ui.console_lines.size(); ++line_idx) {
            if (!passes_filter(line_idx)) continue;
            const ConsoleLineKind kind = line_idx < g_ui.console_line_kinds.size()
                                           ? g_ui.console_line_kinds[line_idx]
                                           : ConsoleLineKind::normal;
            const auto& line = g_ui.console_lines[line_idx];
            const size_t count = line_idx < g_ui.console_line_counts.size()
                                   ? g_ui.console_line_counts[line_idx]
                                   : 1u;
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
        // Pause auto-scroll: when ticked, do NOT snap to bottom even if the
        // user is currently at the end of the scroll.  Without this guard,
        // a new FMOD scan burst would steal the user's mid-scroll position.
        if (stick_to_bottom && !g_ui.console_filter_pause_scroll) {
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
