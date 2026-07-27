#include "frdx/overlay/overlay.hpp"
#include "frdx/overlay/overlay_priv.hpp"
#include <IconsKenney.h>
#include <array>
#include <fstream>
#include <utility>
#include "frdx/platform/windows/subprocess.hpp"

namespace frdx::overlay {
using namespace frdx;

// patch_dualsense template defined in overlay_priv.hpp

bool dualsense_toggle(const char* label, bool value, bool DualSenseConfig::* field) {
    const float s = g_ui.ui_scale;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * s, 2.0f * s));
    const bool changed = ImGui::Toggle(label, &value);
    ImGui::PopStyleVar();
    if (changed) {
        patch_dualsense([=](DualSenseConfig& c) { c.*field = value; });
        return true;
    }
    return false;
}

void dualsense_numeric_layout(float& row_x, float& label_w, float& slider_w, float& input_w) {
    const auto& style = ImGui::GetStyle();
    const float s     = g_ui.ui_scale;
    row_x             = ImGui::GetCursorPosX();
    const float row_r = ImGui::GetWindowContentRegionMax().x - 4.0f * s;
    const float avail = std::max(96.0f * s, row_r - row_x);
    input_w           = 62.0f * s;
    label_w           = std::min(215.0f * s, std::max(190.0f * s, avail * 0.58f));
    slider_w          = std::max(72.0f * s, avail - label_w - input_w - style.ItemSpacing.x * 2.0f);
}

void dualsense_draw_centered_value(const char* text) {
    if (ImGui::IsItemActive()) return;

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 sz  = ImGui::CalcTextSize(text);
    const float x    = min.x + (max.x - min.x - sz.x) * 0.5f;
    const float y    = min.y + (max.y - min.y - sz.y) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), text);
}

bool dualsense_slider_int(const char* label, int value, int min, int max,
                          int DualSenseConfig::* field) {
    float row_x    = 0.0f;
    float label_w  = 0.0f;
    float slider_w = 0.0f;
    float input_w  = 0.0f;
    dualsense_numeric_layout(row_x, label_w, slider_w, input_w);

    int edited   = value;
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetCursorPosX(row_x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(row_x + label_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.82f));
    ImGui::SetNextItemWidth(slider_w);
    changed |= ImGui::SliderInt("##slider", &edited, min, max, "");
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(input_w);
    const ImGuiID value_id  = ImGui::GetID("##value");
    const bool value_active = ImGui::GetStateStorage()->GetBool(value_id, false);
    if (!value_active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    changed |= ImGui::InputInt("##value", &edited, 0, 0);
    ImGui::GetStateStorage()->SetBool(value_id, ImGui::IsItemActive());
    if (!value_active) {
        ImGui::PopStyleColor();
        char value_buf[32];
        std::snprintf(value_buf, sizeof(value_buf), "%d", edited);
        dualsense_draw_centered_value(value_buf);
    }
    ImGui::PopID();

    if (changed) {
        edited = std::clamp(edited, min, max);
        patch_dualsense([=](DualSenseConfig& c) { c.*field = edited; });
        return true;
    }
    return false;
}

bool dualsense_slider_float(const char* label, float value, float min, float max,
                            float DualSenseConfig::* field, const char* fmt) {
    float row_x    = 0.0f;
    float label_w  = 0.0f;
    float slider_w = 0.0f;
    float input_w  = 0.0f;
    dualsense_numeric_layout(row_x, label_w, slider_w, input_w);

    float edited = value;
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetCursorPosX(row_x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(row_x + label_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.82f));
    ImGui::SetNextItemWidth(slider_w);
    changed |= ImGui::SliderFloat("##slider", &edited, min, max, "");
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(input_w);
    const ImGuiID value_id  = ImGui::GetID("##value");
    const bool value_active = ImGui::GetStateStorage()->GetBool(value_id, false);
    if (!value_active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    changed |= ImGui::InputFloat("##value", &edited, 0.0f, 0.0f, fmt);
    ImGui::GetStateStorage()->SetBool(value_id, ImGui::IsItemActive());
    if (!value_active) {
        ImGui::PopStyleColor();
        char value_buf[32];
        std::snprintf(value_buf, sizeof(value_buf), fmt, edited);
        dualsense_draw_centered_value(value_buf);
    }
    ImGui::PopID();

    if (changed) {
        edited = std::clamp(edited, min, max);
        patch_dualsense([=](DualSenseConfig& c) { c.*field = edited; });
        return true;
    }
    return false;
}

AccordionState dualsense_section_header(const char* label, ImGuiID exclusive_group) {
    ImGui::PushID(label);
    ImGuiID id      = ImGui::GetID("open");
    ImGuiID anim_id = ImGui::GetID("accordion_amount");
    bool open       = ImGui::GetStateStorage()->GetBool(id, false);

    // Mutual exclusion within a group: only the member whose ID matches the
    // group's active slot may stay open. Others are forced closed.
    if (exclusive_group != 0) {
        ImGuiID active_id = ImGui::GetStateStorage()->GetInt(exclusive_group, 0);
        if (open && active_id != 0 && active_id != id) {
            open = false;
            ImGui::GetStateStorage()->SetBool(id, false);
        }
    }

    const auto& style  = ImGui::GetStyle();
    const float s      = g_ui.ui_scale;
    const float inset  = 4.0f * s;
    const float width  = std::max(64.0f * s, ImGui::GetContentRegionAvail().x - inset);
    const float height = ImGui::GetFrameHeight();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inset * 0.5f);
    ImGui::InvisibleButton("##section", ImVec2(width, height));
    if (ImGui::IsItemClicked()) {
        open = !open;
        ImGui::GetStateStorage()->SetBool(id, open);
        if (exclusive_group != 0) {
            // Claim the group slot when opening; release it when closing so a
            // later-opened sibling can take over.
            ImGui::GetStateStorage()->SetInt(exclusive_group, open ? static_cast<int>(id) : 0);
        }
    }

    float raw_amount = ImGui::GetStateStorage()->GetFloat(anim_id, open ? 1.0f : 0.0f);
    const float dir  = open ? 1.0f : -1.0f;
    raw_amount       = std::clamp(raw_amount + dir * ImGui::GetIO().DeltaTime / 0.32f, 0.0f, 1.0f);
    ImGui::GetStateStorage()->SetFloat(anim_id, raw_amount);
    const float amount = iam_eval_preset(iam_ease_out_cubic, raw_amount);

    const bool hovered = ImGui::IsItemHovered();
    const bool held    = ImGui::IsItemActive();
    const ImU32 bg     = ImGui::GetColorU32(held      ? ImGuiCol_HeaderActive
                                            : hovered ? ImGuiCol_HeaderHovered
                                                      : ImGuiCol_Header);
    const ImU32 text   = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 arrow  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImVec2 min   = ImGui::GetItemRectMin();
    const ImVec2 max   = ImGui::GetItemRectMax();
    auto* dl           = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, bg, style.FrameRounding, ImDrawFlags_RoundCornersAll);

    const float ui_s2      = g_ui.ui_scale;
    const ImVec2 arrow_center(min.x + style.FramePadding.x + 8.0f * ui_s2, min.y + height * 0.5f);
    const float arrow_size  = 5.5f * ui_s2;
    const float angle       = amount * 1.57079632679f;
    const float sn          = std::sin(angle);
    const float c           = std::cos(angle);
    const auto rotate_point = [&](ImVec2 p) {
        return ImVec2(arrow_center.x + p.x * c - p.y * sn, arrow_center.y + p.x * sn + p.y * c);
    };
    dl->AddTriangleFilled(rotate_point(ImVec2(-arrow_size * 0.45f, -arrow_size)),
                          rotate_point(ImVec2(-arrow_size * 0.45f, arrow_size)),
                          rotate_point(ImVec2(arrow_size * 0.75f, 0.0f)), arrow);
    dl->AddText(ImVec2(min.x + style.FramePadding.x + 18.0f * ui_s2, min.y + style.FramePadding.y), text,
                label);

    AccordionState state;
    state.content_height_id = ImGui::GetID("content_height");
    state.open              = open;
    state.amount            = amount;
    state.visible           = open || amount > 0.01f;
    state.content_width     = width;
    ImGui::PopID();
    return state;
}

bool dualsense_section_content_begin(AccordionState& state) {
    if (!state.visible) return false;

    const auto& style        = ImGui::GetStyle();
    state.content_screen_pos = ImGui::GetCursorScreenPos();
    state.content_start_y    = ImGui::GetCursorPosY();

    const float cached_h =
        std::max(24.0f, ImGui::GetStateStorage()->GetFloat(state.content_height_id, 32.0f));
    const float clip_h = std::max(0.0f, cached_h * std::clamp(state.amount, 0.0f, 1.0f));
    if (state.amount < 0.995f) {
        ImGui::PushClipRect(state.content_screen_pos,
                            ImVec2(state.content_screen_pos.x + state.content_width,
                                   state.content_screen_pos.y + clip_h),
                            true);
        state.clipped = true;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                        ImGui::GetStyle().Alpha * std::clamp(state.amount, 0.0f, 1.0f));
    if (!state.open) {
        ImGui::BeginDisabled();
        state.disabled = true;
    }
    ImGui::Indent(style.FramePadding.x + 6.0f * g_ui.ui_scale);
    return true;
}

void dualsense_section_content_end(AccordionState& state) {
    const auto& style = ImGui::GetStyle();
    ImGui::Unindent(style.FramePadding.x + 6.0f);
    if (state.disabled) ImGui::EndDisabled();

    const float measured_h = std::max(0.0f, ImGui::GetCursorPosY() - state.content_start_y);
    if (measured_h > 1.0f) ImGui::GetStateStorage()->SetFloat(state.content_height_id, measured_h);
    const float visible_h = measured_h * std::clamp(state.amount, 0.0f, 1.0f);

    ImGui::PopStyleVar();
    if (state.clipped) ImGui::PopClipRect();
    if (state.amount < 0.995f) {
        ImGui::SetCursorPosY(state.content_start_y + visible_h);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
}

void refresh_dualsense_devices() {
    g_ui.dualsense_devices       = dualsense::enumerate_devices();
    g_ui.dualsense_devices_dirty = false;
}

std::array<std::string_view, 4> split_status_line(std::string_view line, size_t& count) {
    std::array<std::string_view, 4> parts{};
    count = 0;
    while (count < parts.size()) {
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos) {
            parts[count++] = line;
            break;
        }
        parts[count++] = line.substr(0, tab);
        line.remove_prefix(tab + 1);
    }
    return parts;
}

bool read_helper_status_file(dualsense::RuntimeStatus& status,
                             std::vector<dualsense::ControllerDevice>& devices,
                             std::filesystem::file_time_type* write_time = nullptr) {
    const auto status_path = module_directory() / "mods" / "frdx" / "dualsense_helper_status.tsv";
    std::error_code ec;
    if (!std::filesystem::exists(status_path, ec)) return false;
    const auto mtime = std::filesystem::last_write_time(status_path, ec);
    if (ec) return false;

    std::ifstream in{status_path, std::ios::binary};
    if (!in) return false;

    dualsense::RuntimeStatus next_status;
    std::vector<dualsense::ControllerDevice> next_devices;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t part_count = 0;
        const auto parts  = split_status_line(line, part_count);
        if (part_count < 2) continue;
        const auto& key = parts[0];
        if (key == "enabled") {
            next_status.enabled = parts[1] == "1";
        } else if (key == "connected") {
            next_status.controller_connected = parts[1] == "1";
        } else if (key == "telemetry") {
            next_status.telemetry_seen = parts[1] == "1";
        } else if (key == "controller") {
            next_status.controller_name = std::string{parts[1]};
        } else if (key == "error") {
            next_status.last_error = std::string{parts[1]};
        } else if (key == "device" && part_count >= 4) {
            next_devices.push_back({std::string{parts[1]}, std::string{parts[2]}, parts[3] == "1"});
        }
    }

    status  = std::move(next_status);
    devices = std::move(next_devices);
    if (write_time) *write_time = mtime;
    return true;
}

void refresh_helper_status(bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - g_ui.helper_status_checked < std::chrono::milliseconds(500)) return;
    g_ui.helper_status_checked = now;

    dualsense::RuntimeStatus status;
    std::vector<dualsense::ControllerDevice> devices;
    std::filesystem::file_time_type mtime{};
    if (read_helper_status_file(status, devices, &mtime)) {
        g_ui.helper_status           = std::move(status);
        g_ui.dualsense_devices       = std::move(devices);
        g_ui.helper_status_write     = mtime;
        g_ui.helper_status_available = true;
        g_ui.dualsense_devices_dirty = false;
    } else {
        g_ui.helper_status_available = false;
    }
}

void draw_dualsense_controller_picker(const DualSenseConfig& ds) {
    if (ds.enabled) {
        refresh_helper_status();
    } else if (g_ui.dualsense_devices_dirty) {
        refresh_dualsense_devices();
    }

    int selected = 0;
    for (size_t i = 0; i < g_ui.dualsense_devices.size(); ++i) {
        if (!g_ui.dualsense_devices[i].serial.empty() &&
            g_ui.dualsense_devices[i].serial == ds.controller_lock_serial) {
            selected = static_cast<int>(i + 1);
            break;
        }
    }

    const char* preview =
        selected == 0 ? "Auto (first found)"
                      : g_ui.dualsense_devices[static_cast<size_t>(selected - 1)].name.c_str();
    const float label_w = 82.0f * g_ui.ui_scale;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Controller");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##controller", preview)) {
        const bool auto_selected = selected == 0;
        if (ImGui::Selectable("Auto (first found)", auto_selected)) {
            patch_dualsense([](DualSenseConfig& c) { c.controller_lock_serial.clear(); });
        }
        if (auto_selected) ImGui::SetItemDefaultFocus();

        for (size_t i = 0; i < g_ui.dualsense_devices.size(); ++i) {
            const auto& dev     = g_ui.dualsense_devices[i];
            const bool disabled = dev.serial.empty();
            if (disabled) ImGui::BeginDisabled();
            const bool is_selected  = std::cmp_equal(selected, i + 1);
            const std::string label = disabled ? dev.name + " (no serial)" : dev.name;
            if (ImGui::Selectable(label.c_str(), is_selected) && !disabled) {
                patch_dualsense([serial = dev.serial](DualSenseConfig& c) {
                    c.controller_lock_serial = serial;
                });
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
            if (disabled) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
}

void draw_dualsense_controls_tab(const DualSenseConfig& ds) {
    if (!ds.enabled) ImGui::BeginDisabled();
    ImGui::Columns(2, "##dualsense_controls", false);
    const auto center_column_label = [](const char* text) {
        const float col_w  = ImGui::GetColumnWidth();
        const float text_w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (col_w - text_w) * 0.5f));
        ImGui::TextDisabled("%s", text);
    };

    // Use the larger Kenney font for L2/R2 button badge icons (loaded at
    // modern_icon_lg_size).  PushFont(font, size) sets the unscaled base size.
    if (font_mode_is_modern() && g_ui.font_kenney_lg) ImGui::PushFont(g_ui.font_kenney_lg, g_ui.modern_icon_lg_size);
    center_column_label(font_mode_is_modern() && g_ui.font_kenney_lg ? ICON_KI_BUTTON_L2 : "L2");
    if (font_mode_is_modern() && g_ui.font_kenney_lg) ImGui::PopFont();
    dualsense_toggle("L2 Shift Thump", ds.enable_gear_shift_brake,
                     &DualSenseConfig::enable_gear_shift_brake);
    dualsense_toggle("ABS Rumble", ds.enable_abs, &DualSenseConfig::enable_abs);
    dualsense_toggle("Static Brake Wall", ds.enable_brake_static_wall,
                     &DualSenseConfig::enable_brake_static_wall);
    dualsense_toggle("Brake Stiffness", ds.enable_brake_resistance,
                     &DualSenseConfig::enable_brake_resistance);
    dualsense_toggle("Handbrake Bonus", ds.enable_handbrake_bonus,
                     &DualSenseConfig::enable_handbrake_bonus);

    ImGui::NextColumn();
    if (font_mode_is_modern() && g_ui.font_kenney_lg) ImGui::PushFont(g_ui.font_kenney_lg, g_ui.modern_icon_lg_size);
    center_column_label(font_mode_is_modern() && g_ui.font_kenney_lg ? ICON_KI_BUTTON_R2 : "R2");
    if (font_mode_is_modern() && g_ui.font_kenney_lg) ImGui::PopFont();
    dualsense_toggle("R2 Shift Thump", ds.enable_gear_shift, &DualSenseConfig::enable_gear_shift);
    dualsense_toggle("Redline Buzz", ds.enable_rev_limiter, &DualSenseConfig::enable_rev_limiter);
    dualsense_toggle("Wheelspin Buzz", ds.enable_wheelspin_buzz,
                     &DualSenseConfig::enable_wheelspin_buzz);
    dualsense_toggle("Idle Buzz", ds.enable_idle_buzz, &DualSenseConfig::enable_idle_buzz);
    dualsense_toggle("Throttle Stiffness", ds.enable_throttle_resistance,
                     &DualSenseConfig::enable_throttle_resistance);
    ImGui::Columns(1);
    if (!ds.enabled) ImGui::EndDisabled();
}

bool draw_dualsense_tuning_tab(const DualSenseConfig& ds) {
    bool any_expanded = false;
    // Only one tuning accordion open at a time.
    const ImGuiID exclusive_group = ImGui::GetID("dualsense_tuning_exclusive");

    auto pedal_dead_zones = dualsense_section_header("Pedal Dead Zones", exclusive_group);
    if (dualsense_section_content_begin(pedal_dead_zones)) {
        any_expanded = any_expanded || pedal_dead_zones.open;
        ImGui::PushID("Pedal Dead Zones");
        dualsense_slider_int("Gas Trigger Dead Zone", ds.accel_deadzone, 0, 255,
                             &DualSenseConfig::accel_deadzone);
        dualsense_slider_int("Brake Trigger Dead Zone", ds.brake_deadzone, 0, 255,
                             &DualSenseConfig::brake_deadzone);
        ImGui::PopID();
        dualsense_section_content_end(pedal_dead_zones);
    }

    auto brake_force = dualsense_section_header("Left Trigger - Brake Force", exclusive_group);
    if (dualsense_section_content_begin(brake_force)) {
        any_expanded = any_expanded || brake_force.open;
        ImGui::PushID("Left Trigger - Brake Force");
        dualsense_slider_int("Resting Stiffness", ds.brake_baseline_force, 0, 255,
                             &DualSenseConfig::brake_baseline_force);
        dualsense_slider_int("Hard-Press Stiffness", ds.brake_max_force, 0, 255,
                             &DualSenseConfig::brake_max_force);
        dualsense_slider_float("Stiffness Curve Shape", ds.brake_curve, 0.1f, 20.0f,
                               &DualSenseConfig::brake_curve);
        dualsense_slider_int("Handbrake Extra Stiffness", ds.handbrake_bonus, 0, 255,
                             &DualSenseConfig::handbrake_bonus);
        ImGui::PopID();
        dualsense_section_content_end(brake_force);
    }

    auto brake_wall =
        dualsense_section_header("Left Trigger - Static Wall (optional)", exclusive_group);
    if (dualsense_section_content_begin(brake_wall)) {
        any_expanded = any_expanded || brake_wall.open;
        ImGui::PushID("Left Trigger - Static Wall (optional)");
        if (!ds.enable_brake_static_wall) ImGui::BeginDisabled();
        dualsense_slider_int("Wall Position", ds.brake_static_wall_at, 0, 255,
                             &DualSenseConfig::brake_static_wall_at);
        dualsense_slider_int("Wall Hardness", ds.brake_static_wall_force, 0, 255,
                             &DualSenseConfig::brake_static_wall_force);
        if (!ds.enable_brake_static_wall) ImGui::EndDisabled();
        ImGui::PopID();
        dualsense_section_content_end(brake_wall);
    }

    auto gas_force = dualsense_section_header("Right Trigger - Gas Force", exclusive_group);
    if (dualsense_section_content_begin(gas_force)) {
        any_expanded = any_expanded || gas_force.open;
        ImGui::PushID("Right Trigger - Gas Force");
        dualsense_slider_int("Resting Stiffness", ds.throttle_baseline_force, 0, 255,
                             &DualSenseConfig::throttle_baseline_force);
        dualsense_slider_int("Hard-Press Stiffness", ds.throttle_max_force, 0, 255,
                             &DualSenseConfig::throttle_max_force);
        dualsense_slider_float("Stiffness Curve Shape", ds.throttle_curve, 0.1f, 20.0f,
                               &DualSenseConfig::throttle_curve);
        ImGui::PopID();
        dualsense_section_content_end(gas_force);
    }

    auto abs_rumble = dualsense_section_header("ABS Rumble", exclusive_group);
    if (dualsense_section_content_begin(abs_rumble)) {
        any_expanded = any_expanded || abs_rumble.open;
        ImGui::PushID("ABS Rumble");
        dualsense_slider_int("Only When Braking Harder Than", ds.abs_brake_threshold, 0, 255,
                             &DualSenseConfig::abs_brake_threshold);
        dualsense_slider_float("Only When Faster Than", ds.abs_min_speed_kmh, 0.0f, 500.0f,
                               &DualSenseConfig::abs_min_speed_kmh, "%.0f km/h");
        dualsense_slider_float("Wheel Slip Sensitivity", ds.abs_slip_ratio_threshold, 0.0f, 10.0f,
                               &DualSenseConfig::abs_slip_ratio_threshold);
        dualsense_slider_float("Tire Grip Sensitivity", ds.abs_combined_slip_threshold, 0.0f, 10.0f,
                               &DualSenseConfig::abs_combined_slip_threshold);
        dualsense_slider_int("Rumble Speed", ds.abs_freq, 0, 255, &DualSenseConfig::abs_freq);
        dualsense_slider_int("Rumble Strength", ds.abs_amp, 0, 255, &DualSenseConfig::abs_amp);
        ImGui::PopID();
        dualsense_section_content_end(abs_rumble);
    }

    auto redline_buzz = dualsense_section_header("Redline Buzz", exclusive_group);
    if (dualsense_section_content_begin(redline_buzz)) {
        any_expanded = any_expanded || redline_buzz.open;
        ImGui::PushID("Redline Buzz");
        dualsense_slider_float("Fire Near Redline At", ds.rev_limit_ratio, 0.0f, 1.0f,
                               &DualSenseConfig::rev_limit_ratio);
        dualsense_slider_int("Buzz Speed", ds.rev_limit_freq, 0, 255,
                             &DualSenseConfig::rev_limit_freq);
        dualsense_slider_int("Buzz Strength", ds.rev_limit_amp, 0, 255,
                             &DualSenseConfig::rev_limit_amp);
        dualsense_slider_float("Buzz Hold Time", ds.rev_limit_hold_ms, 0.0f, 1000.0f,
                               &DualSenseConfig::rev_limit_hold_ms, "%.0f ms");
        ImGui::PopID();
        dualsense_section_content_end(redline_buzz);
    }

    auto wheelspin_buzz = dualsense_section_header("Wheelspin Buzz", exclusive_group);
    if (dualsense_section_content_begin(wheelspin_buzz)) {
        any_expanded = any_expanded || wheelspin_buzz.open;
        ImGui::PushID("Wheelspin Buzz");
        dualsense_slider_int("Buzz Strength", ds.wheelspin_amp, 0, 255,
                             &DualSenseConfig::wheelspin_amp);
        ImGui::PopID();
        dualsense_section_content_end(wheelspin_buzz);
    }

    auto idle_buzz = dualsense_section_header("Idle Buzz", exclusive_group);
    if (dualsense_section_content_begin(idle_buzz)) {
        any_expanded = any_expanded || idle_buzz.open;
        ImGui::PushID("Idle Buzz");
        dualsense_slider_int("Idle Strength", ds.idle_amp_high, 0, 255,
                             &DualSenseConfig::idle_amp_high);
        ImGui::PopID();
        dualsense_section_content_end(idle_buzz);
    }

    auto gear_shift = dualsense_section_header("Gear Shift Thump", exclusive_group);
    if (dualsense_section_content_begin(gear_shift)) {
        any_expanded = any_expanded || gear_shift.open;
        ImGui::PushID("Gear Shift Thump");
        dualsense_slider_int("Thump Speed", ds.gear_shift_freq, 0, 255,
                             &DualSenseConfig::gear_shift_freq);
        dualsense_slider_int("Thump Strength", ds.gear_shift_amp, 0, 255,
                             &DualSenseConfig::gear_shift_amp);
        dualsense_slider_float("Thump Length", ds.gear_shift_duration_ms, 0.0f, 2000.0f,
                               &DualSenseConfig::gear_shift_duration_ms, "%.0f ms");
        ImGui::PopID();
        dualsense_section_content_end(gear_shift);
    }

    return any_expanded;
}

void draw_dualsense_controller_tab(const DualSenseConfig& ds) {
    if (ds.enabled) refresh_helper_status();
    const dualsense::RuntimeStatus status =
        ds.enabled && g_ui.helper_status_available
            ? g_ui.helper_status
            : (g_ctx.dualsense ? g_ctx.dualsense->status() : dualsense::RuntimeStatus{});

    const ImVec4 green(0.36f, 0.85f, 0.46f, 1.0f);
    const ImVec4 yellow(0.95f, 0.77f, 0.32f, 1.0f);
    const ImVec4 red(0.95f, 0.35f, 0.35f, 1.0f);

    draw_dualsense_controller_picker(ds);

    if (status.controller_connected) {
        ImGui::TextColored(green, "Connected");
        ImGui::SameLine();
        ImGui::TextUnformatted(status.controller_name.c_str());
    } else if (ds.enabled && !g_ui.helper_status_available) {
        ImGui::TextColored(yellow, "Helper starting");
    } else {
        ImGui::TextColored(yellow, "Waiting for controller");
    }

    if (ImGui::Button("Rescan")) {
        if (ds.enabled) {
            refresh_helper_status(true);
        } else {
            refresh_dualsense_devices();
        }
    }
    ImGui::SameLine();
    if (ds.controller_lock_serial.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Identify")) {
        enqueue_action([serial = ds.controller_lock_serial, force = ds.startup_pulse_force] {
            (void)dualsense::identify_controller(serial, force);
        });
    }
    if (ds.controller_lock_serial.empty()) ImGui::EndDisabled();
    const float reset_w = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float reset_x = ImGui::GetWindowContentRegionMax().x - reset_w;
    if (ImGui::GetCursorPosX() < reset_x) ImGui::SameLine(reset_x);
    if (ImGui::Button("Reset")) {
        patch_dualsense([](DualSenseConfig& c) {
            DualSenseConfig defaults;
            c.udp_port             = defaults.udp_port;
            c.enable_startup_pulse = defaults.enable_startup_pulse;
            c.startup_pulse_force  = defaults.startup_pulse_force;
            c.enable_reconnect     = defaults.enable_reconnect;
            c.reconnect_interval_s = defaults.reconnect_interval_s;
            c.controller_lock_serial.clear();
        });
    }

    ImGui::Separator();

    char udp_buf[6] = {};
    std::snprintf(udp_buf, sizeof(udp_buf), "%d", ds.udp_port);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("UDP Port");
    ImGui::SameLine(82.0f * g_ui.ui_scale);
    ImGui::SetNextItemWidth(68.0f * g_ui.ui_scale);
    if (ImGui::InputText("##udp_port", udp_buf, sizeof(udp_buf),
                         ImGuiInputTextFlags_CharsDecimal)) {
        if (udp_buf[0] != '\0') {
            // atoi is safe here: ImGuiInputTextFlags_CharsDecimal restricts input to
            // digits, and std::clamp bounds the result to a valid port range.
            const int udp_port = std::clamp(std::atoi(udp_buf), 1, 65535); // NOLINT(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
            patch_dualsense([=](DualSenseConfig& c) { c.udp_port = udp_port; });
        }
    }
    ImGui::SameLine();
    if (!ds.enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Send Test")) {
        (void)dualsense::send_test_telemetry(ds.udp_port);
    }
    if (!ds.enabled) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!status.last_error.empty()) {
        ImGui::TextColored(red, "Error");
    } else if (status.telemetry_seen) {
        ImGui::TextColored(green, "Seen");
    } else if (ds.enabled) {
        ImGui::TextColored(yellow, "Helper");
    } else {
        ImGui::TextColored(yellow, "Waiting");
    }

    if (!status.last_error.empty()) {
        ImGui::TextWrapped("%s", status.last_error.c_str());
    }

    ImGui::Separator();

    auto startup = dualsense_section_header("Startup");
    if (dualsense_section_content_begin(startup)) {
        dualsense_toggle("Startup Pulse", ds.enable_startup_pulse,
                         &DualSenseConfig::enable_startup_pulse);
        dualsense_slider_int("Startup Force", ds.startup_pulse_force, 0, 255,
                             &DualSenseConfig::startup_pulse_force);
        dualsense_section_content_end(startup);
    }

    auto reconnect = dualsense_section_header("Reconnect");
    if (dualsense_section_content_begin(reconnect)) {
        dualsense_toggle("Auto Reconnect", ds.enable_reconnect, &DualSenseConfig::enable_reconnect);
        dualsense_slider_float("Reconnect Delay", ds.reconnect_interval_s, 0.1f, 60.0f,
                               &DualSenseConfig::reconnect_interval_s, "%.1f s");
        dualsense_section_content_end(reconnect);
    }
}

void draw_dualsense_reset_button() {
    const bool armed = g_ui.dualsense_reset_armed;
    if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.16f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
    }

    if (ImGui::Button(armed ? "Click Again To Reset" : "Reset DualSense Defaults")) {
        if (!armed) {
            g_ui.dualsense_reset_armed = true;
        } else {
            patch_dualsense([](DualSenseConfig& c) {
                DualSenseConfig defaults;
                defaults.enabled                = c.enabled;
                defaults.external_helper        = c.external_helper;
                defaults.udp_port               = c.udp_port;
                defaults.controller_lock_serial = c.controller_lock_serial;
                c                               = defaults;
            });
            g_ui.dualsense_reset_armed = false;
        }
    }

    if (armed) ImGui::PopStyleColor(3);
}

bool draw_dualsense_tab_strip() {
    constexpr int kTabCount       = 3;
    const char* labels[kTabCount] = {"Triggers", "Settings", "Controller"};

    const float dt         = ImGui::GetIO().DeltaTime;
    const float ui_s3      = g_ui.ui_scale;
    const float tab_h      = ImGui::GetFrameHeight() + 4.0f * ui_s3;
    const float text_pad_x = 9.0f * ui_s3;
    float tab_widths[kTabCount]{};
    float total_w = 0.0f;
    for (int i = 0; i < kTabCount; ++i) {
        tab_widths[i]  = ImGui::CalcTextSize(labels[i]).x + text_pad_x * 2.0f;
        total_w       += tab_widths[i];
    }

    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    auto* dl             = ImGui::GetWindowDrawList();
    const float rounding = 5.0f * ui_s3;
    dl->AddRectFilled(pos, ImVec2(pos.x + total_w, pos.y + tab_h),
                      ImGui::GetColorU32(ImGuiCol_FrameBg, 0.54f), rounding,
                      ImDrawFlags_RoundCornersTop);

    const int active_tab = std::clamp(g_ui.dualsense_tab, 0, kTabCount - 1);
    float target_offset  = 0.0f;
    for (int i = 0; i < active_tab; ++i) target_offset += tab_widths[i];
    const float target_w = tab_widths[active_tab];

    if (!g_ui.dualsense_tab_indicator_ready) {
        g_ui.dualsense_tab_indicator_ready  = true;
        g_ui.dualsense_tab_indicator_from_x = target_offset;
        g_ui.dualsense_tab_indicator_from_w = target_w;
        g_ui.dualsense_tab_indicator_to_x   = target_offset;
        g_ui.dualsense_tab_indicator_to_w   = target_w;
        g_ui.dualsense_tab_indicator_anim   = 1.0f;
    } else if (std::abs(g_ui.dualsense_tab_indicator_to_x - target_offset) > 0.5f ||
               std::abs(g_ui.dualsense_tab_indicator_to_w - target_w) > 0.5f) {
        const float t = iam_eval_preset(iam_ease_out_cubic, g_ui.dualsense_tab_indicator_anim);
        g_ui.dualsense_tab_indicator_from_x =
            g_ui.dualsense_tab_indicator_from_x +
            (g_ui.dualsense_tab_indicator_to_x - g_ui.dualsense_tab_indicator_from_x) * t;
        g_ui.dualsense_tab_indicator_from_w =
            g_ui.dualsense_tab_indicator_from_w +
            (g_ui.dualsense_tab_indicator_to_w - g_ui.dualsense_tab_indicator_from_w) * t;
        g_ui.dualsense_tab_indicator_to_x = target_offset;
        g_ui.dualsense_tab_indicator_to_w = target_w;
        g_ui.dualsense_tab_indicator_anim = 0.0f;
    }

    g_ui.dualsense_tab_indicator_anim =
        std::min(1.0f, g_ui.dualsense_tab_indicator_anim + dt / 0.38f);
    const float indicator_t =
        iam_eval_preset(iam_ease_out_cubic, g_ui.dualsense_tab_indicator_anim);
    const float indicator_x =
        pos.x + g_ui.dualsense_tab_indicator_from_x +
        (g_ui.dualsense_tab_indicator_to_x - g_ui.dualsense_tab_indicator_from_x) * indicator_t;
    const float indicator_w =
        g_ui.dualsense_tab_indicator_from_w +
        (g_ui.dualsense_tab_indicator_to_w - g_ui.dualsense_tab_indicator_from_w) * indicator_t;

    ImGui::PushID("dualsense_tab_strip");
    ImGui::InvisibleButton("##strip", ImVec2(total_w, tab_h));
    const bool strip_hovered = ImGui::IsItemHovered();
    bool changed             = false;
    if (ImGui::IsItemClicked()) {
        const float local_x = ImGui::GetMousePos().x - pos.x;
        float edge          = 0.0f;
        for (int i = 0; i < kTabCount; ++i) {
            edge += tab_widths[i];
            if (local_x <= edge) {
                changed            = g_ui.dualsense_tab != i;
                g_ui.dualsense_tab = i;
                break;
            }
        }
    }

    float x             = pos.x;
    const float mouse_x = ImGui::GetMousePos().x;
    for (int i = 0; i < kTabCount; ++i) {
        const ImVec2 tab_min(x, pos.y);
        const ImVec2 tab_size(tab_widths[i], tab_h);
        ImGui::PushID(i);
        const bool hovered =
            strip_hovered && mouse_x >= tab_min.x && mouse_x < tab_min.x + tab_size.x;
        const float target_alpha = i == g_ui.dualsense_tab ? 1.0f : (hovered ? 0.82f : 0.52f);
        const float text_alpha   = iam_tween_float(ImGui::GetID("label"), 0x61310003u, target_alpha,
                                                   0.15f, iam_ease_preset(iam_ease_out_cubic),
                                                   iam_policy_crossfade, dt, target_alpha);

        const ImVec2 text_size = ImGui::CalcTextSize(labels[i]);
        const ImVec2 text_pos(tab_min.x + (tab_size.x - text_size.x) * 0.5f,
                              tab_min.y + (tab_size.y - text_size.y) * 0.5f);
        ImVec4 text_col  = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        text_col.w      *= text_alpha;
        dl->AddText(text_pos, ImGui::GetColorU32(text_col), labels[i]);
        ImGui::PopID();
        x += tab_widths[i];
    }

    const float indicator_y = pos.y + tab_h - 3.0f * ui_s3;
    dl->AddRectFilled(ImVec2(indicator_x + 8.0f * ui_s3, indicator_y),
                      ImVec2(indicator_x + indicator_w - 8.0f * ui_s3, indicator_y + 3.0f * ui_s3),
                      ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.0f * ui_s3);
    ImGui::PopID();
    return changed;
}

void draw_dualsense_tab_header(const DualSenseConfig& ds) {
    const float ui_s    = g_ui.ui_scale;
    const float ls      = layout_scale();
    const float row_y    = ImGui::GetCursorPosY();
    const float toggle_w = 132.0f * ui_s * ls;
    const float tab_h    = ImGui::GetFrameHeight() + 4.0f * ui_s;
    const float toggle_h = ImGui::GetTextLineHeight() + 4.0f * ui_s;

    const bool tab_changed = draw_dualsense_tab_strip();
    if (tab_changed) g_ui.dualsense_tab_content_anim = 0.0f;
    const float after_tabs_y = ImGui::GetCursorPosY();

    // Place the enable checkbox on the same row as the tab strip when there's
    // room; otherwise it wraps to its own line.
    const float toggle_y = row_y + std::max(0.0f, (tab_h - toggle_h) * 0.5f);
    const float toggle_x = ImGui::GetWindowContentRegionMax().x - toggle_w;
    ImGui::SetCursorPos(ImVec2(toggle_x, toggle_y));

    bool enabled = ds.enabled;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * ui_s, 2.0f * ui_s));
    const bool enabled_changed = ImGui::Toggle("Enable Triggers", &enabled);
    ImGui::PopStyleVar();
    if (enabled_changed) {
        patch_dualsense([enabled](DualSenseConfig& c) {
            c.enabled = enabled;
            if (enabled) c.external_helper = true;
        });
    }
    const float after_toggle_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(std::max(after_tabs_y, after_toggle_y));
}

void draw_dualsense_settings(const DualSenseConfig& ds) {
    draw_dualsense_tab_header(ds);
    ImGui::Separator();

    g_ui.dualsense_tab_content_anim =
        std::min(1.0f, g_ui.dualsense_tab_content_anim + ImGui::GetIO().DeltaTime / 0.30f);
    const float content_alpha =
        iam_eval_preset(iam_ease_out_cubic, g_ui.dualsense_tab_content_anim);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * content_alpha);
    if (g_ui.dualsense_tab == 0) {
        g_ui.dualsense_settings_expanded = false;
        draw_dualsense_controls_tab(ds);
    } else if (g_ui.dualsense_tab == 1) {
        if (!ds.enabled) ImGui::BeginDisabled();
        g_ui.dualsense_settings_expanded = draw_dualsense_tuning_tab(ds);
        ImGui::Separator();
        draw_dualsense_reset_button();
        if (!ds.enabled) ImGui::EndDisabled();
    } else {
        g_ui.dualsense_tab               = 2;
        g_ui.dualsense_settings_expanded = false;
        if (!ds.enabled) ImGui::BeginDisabled();
        draw_dualsense_controller_tab(ds);
        if (!ds.enabled) ImGui::EndDisabled();
    }
    ImGui::PopStyleVar();
}

} // namespace frdx::overlay
