// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "layer.h"

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include "SDL3/SDL_log.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "imgui/imgui_std.h"
#include "imgui_internal.h"
#include "options.h"
#include "video_core/gr2_fsr3/gr2_fsr3.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "widget/frame_dump.h"
#include "widget/frame_graph.h"
#include "widget/gpu_memory.h"
#include "widget/memory_map.h"
#include "widget/module_list.h"
#include "widget/shader_list.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

using namespace ImGui;
using namespace ::Core::Devtools;
using L = ::Core::Devtools::Layer;

static bool shader_skip_open = false;
static bool predication_open = false;
static bool shader_skip_compute_only = true;

static bool show_simple_fps = false;
static bool visibility_toggled = false;
static bool fps_pinned = false;
static bool show_quit_window = false;

static bool show_volume = false;
static float volume_start_time;

static float fps_scale = 1.0f;
static int dump_frame_count = 1;

static Widget::FrameGraph frame_graph;
static std::vector<Widget::FrameDumpViewer> frame_viewers;

static float debug_popup_timing = 3.0f;

static bool just_opened_options = false;

static Widget::MemoryMapViewer memory_map;
static Widget::ShaderList shader_list;
static Widget::ModuleList module_list;
static Widget::GpuMemoryViewer gpu_memory;

// clang-format off
static std::string help_text =
#include "help.txt"
    ;
// clang-format on

void L::DrawMenuBar() {
    const auto& ctx = *GImGui;
    const auto& io = ctx.IO;

    auto isSystemPaused = DebugState.IsGuestThreadsPaused();

    bool open_popup_options = false;
    bool open_popup_help = false;

    if (BeginMainMenuBar()) {
        if (BeginMenu("Options")) {
            if (MenuItemEx("Emulator Paused", nullptr, nullptr, isSystemPaused)) {
                if (isSystemPaused) {
                    DebugState.ResumeGuestThreads();
                } else {
                    DebugState.PauseGuestThreads();
                }
            }
            ImGui::EndMenu();
        }
        if (BeginMenu("GPU Tools")) {
            MenuItem("Show frame info", nullptr, &frame_graph.is_open);
            MenuItem("Show loaded shaders", nullptr, &shader_list.open);
            MenuItem("Show GPU memory usage", nullptr, &gpu_memory.open);
            if (BeginMenu("Dump frames")) {
                SliderInt("Count", &dump_frame_count, 1, 5);
                if (MenuItem("Dump", "Ctrl+Alt+F9", nullptr, !DebugState.DumpingCurrentFrame())) {
                    DebugState.RequestFrameDump(dump_frame_count);
                }
                ImGui::EndMenu();
            }
            open_popup_options = MenuItem("Options");
            open_popup_help = MenuItem("Help & Tips");
            ImGui::EndMenu();
        }
        if (BeginMenu("Display")) {
            auto& pp_settings = presenter->GetPPSettingsRef();
            if (BeginMenu("Brightness")) {
                SliderFloat("Gamma", &pp_settings.gamma, 0.1f, 2.0f);
                ImGui::EndMenu();
            }
            if (BeginMenu("FSR")) {
                auto& fsr = presenter->GetFsrSettingsRef();
                Checkbox("FSR Enabled", &fsr.enable);
                BeginDisabled(!fsr.enable);
                {
                    Checkbox("RCAS", &fsr.use_rcas);
                    BeginDisabled(!fsr.use_rcas);
                    {
                        SliderFloat("RCAS Attenuation", &fsr.rcas_attenuation, 0.0, 3.0);
                    }
                    EndDisabled();
                }
                EndDisabled();

                if (Button("Save")) {
                    EmulatorSettings.SetFsrEnabled(fsr.enable);
                    EmulatorSettings.SetRcasEnabled(fsr.use_rcas);
                    EmulatorSettings.SetRcasAttenuation(
                        static_cast<int>(fsr.rcas_attenuation * 1000));
                    EmulatorSettings.Save();
                    CloseCurrentPopup();
                }

                ImGui::EndMenu();
            }
            Gr2Fsr3::DrawMenu();
            ImGui::EndMenu();
        }
        Gr2Fsr3::DrawDlssMenu();
        if (BeginMenu("Debug")) {
            MenuItem("Memory map", nullptr, &memory_map.open);
            MenuItem("Module list", nullptr, &module_list.open);
            MenuItem("Shader skip list", nullptr, &shader_skip_open);
            MenuItem("Predication", nullptr, &predication_open);
            ImGui::EndMenu();
        }

        SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (Button("X", ImVec2(25, 25))) {
            DebugState.IsShowingDebugMenuBar() = false;
        }

        EndMainMenuBar();
    }
    if (open_popup_options) {
        OpenPopup("GPU Tools Options");
        just_opened_options = true;
    }
    if (open_popup_help) {
        OpenPopup("HelpTips");
    }
}

static void DrawPredication() {
    auto& rasterizer = presenter->GetRasterizer();
    SetNextWindowSize({460.0f, 420.0f}, ImGuiCond_FirstUseEver);
    if (!Begin("Predication", &predication_open)) {
        End();
        return;
    }
    int mode = Vulkan::Rasterizer::predication_mode;
    bool mode_changed = RadioButton("Normal", &mode, 0);
    SameLine();
    mode_changed |= RadioButton("Never cull", &mode, 1);
    SameLine();
    mode_changed |= RadioButton("Always cull", &mode, 2);
    if (mode_changed) {
        Vulkan::Rasterizer::predication_mode = mode;
    }
    bool force_counting = Vulkan::Rasterizer::predication_force_counting;
    if (Checkbox("Force occlusion counting on", &force_counting)) {
        Vulkan::Rasterizer::predication_force_counting = force_counting;
    }
    const auto stats = rasterizer.GetPredicationStats();
    Separator();
    Text("Counting: %s", stats.counting ? "on" : "off");
    Text("Predicates evaluated: %llu", static_cast<unsigned long long>(stats.packets));
    Text("  culled: %llu  drawn: %llu  not final: %llu",
         static_cast<unsigned long long>(stats.culled),
         static_cast<unsigned long long>(stats.drawn),
         static_cast<unsigned long long>(stats.unknown));
    Text("Predicated packets skipped: %llu",
         static_cast<unsigned long long>(stats.skipped_packets));
    Text("Counter dumps: %llu  inexact windows: %llu", static_cast<unsigned long long>(stats.dumps),
         static_cast<unsigned long long>(stats.inexact_windows));
    Text("Queries issued: %llu  free slots: %u", static_cast<unsigned long long>(stats.queries),
         stats.free_slots);
    if (Button("Reset stats")) {
        rasterizer.ResetPredicationStats();
    }
    Separator();
    bool skip_dingdong_wait = DebugState.skip_dingdong_idle_wait;
    if (Checkbox("Skip GPU idle wait in DingDong", &skip_dingdong_wait)) {
        DebugState.skip_dingdong_idle_wait = skip_dingdong_wait;
    }
    bool skip_submit_wait = DebugState.skip_submit_idle_wait;
    if (Checkbox("Skip GPU idle wait in SubmitCommandBuffers", &skip_submit_wait)) {
        DebugState.skip_submit_idle_wait = skip_submit_wait;
    }
    Separator();
    {
        using VideoCore::BufferCache;
        static u64 last_requests = 0, last_stalls = 0, last_bytes = 0, last_fast = 0;
        static float rate_requests = 0, rate_stalls = 0, rate_kb = 0, rate_fast = 0;
        static double last_time = 0;
        const double now = GetTime();
        if (now - last_time >= 1.0) {
            const float span = static_cast<float>(now - last_time);
            const u64 requests = BufferCache::readback_requests;
            const u64 stalls = BufferCache::readback_stalls;
            const u64 bytes = BufferCache::readback_bytes;
            rate_requests = static_cast<float>(requests - last_requests) / span;
            rate_stalls = static_cast<float>(stalls - last_stalls) / span;
            const u64 fast = BufferCache::readback_fast;
            rate_fast = static_cast<float>(fast - last_fast) / span;
            last_fast = fast;
            rate_kb = static_cast<float>(bytes - last_bytes) / span / 1024.0f;
            last_requests = requests;
            last_stalls = stalls;
            last_bytes = bytes;
            last_time = now;
        }
        Text("Readbacks per second: %.0f requests, %.0f GPU stalls, %.0f fast, %.0f KB",
             rate_requests, rate_stalls, rate_fast, rate_kb);
        static constexpr u32 WindowSizesKb[] = {512, 2048, 8192, 32768, 131072, 524288};
        static constexpr const char* WindowLabels[] = {"512 KB", "2 MB",   "8 MB",
                                                       "32 MB",  "128 MB", "512 MB"};
        static constexpr const char* LiveModes[] = {
            "Precise (wait for GPU)", "Deferred (no wait, data arrives late)", "Skip (no data)",
            "Optimized precise (experimental)"};
        int live_mode = static_cast<int>(BufferCache::readback_live_mode.load());
        if (Combo("Readback mode", &live_mode, LiveModes, IM_ARRAYSIZE(LiveModes))) {
            BufferCache::readback_live_mode = static_cast<u32>(live_mode);
        }
        static int window_index = 0;
        if (Combo("Readback window", &window_index, WindowLabels, IM_ARRAYSIZE(WindowLabels))) {
            BufferCache::readback_window_kb = WindowSizesKb[window_index];
        }
    }
    Separator();
    if (BeginTable("##predication", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        TableSetupColumn("Address");
        TableSetupColumn("Counters");
        TableSetupColumn("Draw visible", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed);
        TableHeadersRow();
        for (const auto& decision : rasterizer.GetPredicationDecisions()) {
            TableNextRow();
            TableNextColumn();
            Text("%#llx", static_cast<unsigned long long>(decision.address));
            TableNextColumn();
            Text("%s", decision.state == 0   ? "not final"
                       : decision.state == 1 ? "visible"
                                             : "occluded");
            TableNextColumn();
            Text("%d", decision.draw_visible ? 1 : 0);
            TableNextColumn();
            Text("%s", decision.skipped ? "culled" : "drawn");
        }
        EndTable();
    }
    End();
}

static void DrawShaderSkipList() {
    auto& rasterizer = presenter->GetRasterizer();
    SetNextWindowSize({420.0f, 500.0f}, ImGuiCond_FirstUseEver);
    if (!Begin("Shader skip list", &shader_skip_open)) {
        End();
        return;
    }
    static int capture_images = 200;
    static int capture_min_size = 128;
    const u32 capture_remaining = rasterizer.PassCaptureRemaining();
    SliderInt("Images", &capture_images, 10, 1000);
    SliderInt("Min size", &capture_min_size, 16, 1920);
    static bool capture_compute_only = true;
    Checkbox("Compute passes only", &capture_compute_only);
    static u64 capture_buffer_hash = 0;
    char capture_preview[32];
    if (capture_buffer_hash == 0) {
        std::snprintf(capture_preview, sizeof(capture_preview), "none");
    } else {
        std::snprintf(capture_preview, sizeof(capture_preview), "%#010llx",
                      static_cast<unsigned long long>(capture_buffer_hash));
    }
    if (BeginCombo("Buffers of shader", capture_preview)) {
        if (Selectable("none", capture_buffer_hash == 0)) {
            capture_buffer_hash = 0;
        }
        for (const auto& entry : rasterizer.GetShaderSkipEntries()) {
            if (shader_skip_compute_only && entry.stage != Shader::SwStage::Compute) {
                continue;
            }
            char label[32];
            std::snprintf(label, sizeof(label), "%#010llx",
                          static_cast<unsigned long long>(entry.hash));
            if (Selectable(label, capture_buffer_hash == entry.hash)) {
                capture_buffer_hash = entry.hash;
            }
        }
        EndCombo();
    }
    if (capture_remaining != 0) {
        Text("Capturing... %u left", capture_remaining);
    } else if (Button("Capture passes")) {
        rasterizer.StartPassCapture(static_cast<u32>(capture_images),
                                    static_cast<u32>(capture_min_size), capture_buffer_hash,
                                    capture_compute_only);
    }
    Separator();
    std::vector<Vulkan::Rasterizer::ShaderSkipEntry> visible;
    for (const auto& entry : rasterizer.GetShaderSkipEntries()) {
        if (!shader_skip_compute_only || entry.stage == Shader::SwStage::Compute) {
            visible.push_back(entry);
        }
    }
    const auto set_range = [&](size_t first, size_t last, bool skipped) {
        for (size_t i = first; i < last; ++i) {
            rasterizer.SetShaderSkipped(visible[i].hash, skipped);
        }
    };
    const size_t half = visible.size() / 2;
    if (Button("Clear all")) {
        rasterizer.ClearShaderSkips();
    }
    SameLine();
    Checkbox("Compute only", &shader_skip_compute_only);
    bool ignore_stencil = Vulkan::Rasterizer::ignore_float_target_stencil;
    if (Checkbox("Ignore stencil on half-res lighting draws", &ignore_stencil)) {
        Vulkan::Rasterizer::ignore_float_target_stencil = ignore_stencil;
    }
    bool ignore_readers = Vulkan::Rasterizer::ignore_stencil_readers;
    if (Checkbox("Ignore stencil on all draws that only test it", &ignore_readers)) {
        Vulkan::Rasterizer::ignore_stencil_readers = ignore_readers;
    }
    if (Button("Skip 1st half")) {
        rasterizer.ClearShaderSkips();
        set_range(0, half, true);
    }
    SameLine();
    if (Button("Skip 2nd half")) {
        rasterizer.ClearShaderSkips();
        set_range(half, visible.size(), true);
    }
    SameLine();
    if (Button("Skip all")) {
        set_range(0, visible.size(), true);
    }
    Separator();
    if (BeginTable("##shader_skip", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        TableSetupColumn("Skip", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("Hash");
        TableSetupColumn("Stage");
        TableSetupColumn("Uses", ImGuiTableColumnFlags_WidthFixed);
        TableHeadersRow();
        for (const auto& entry : visible) {
            TableNextRow();
            TableNextColumn();
            bool skipped = entry.skipped;
            PushID(static_cast<int>(entry.hash));
            if (Checkbox("##skip", &skipped)) {
                rasterizer.SetShaderSkipped(entry.hash, skipped);
            }
            PopID();
            TableNextColumn();
            Text("%#010llx", static_cast<unsigned long long>(entry.hash));
            TableNextColumn();
            Text("%u", static_cast<u32>(entry.stage));
            TableNextColumn();
            Text("%llu", static_cast<unsigned long long>(entry.count));
        }
        EndTable();
    }
    End();
}

void L::DrawAdvanced() {
    DrawMenuBar();

    if (shader_skip_open) {
        DrawShaderSkipList();
    }
    if (predication_open) {
        DrawPredication();
    }

    const auto& ctx = *GImGui;
    const auto& io = ctx.IO;

    frame_graph.Draw();

    if (DebugState.should_show_frame_dump && DebugState.waiting_reg_dumps.empty()) {
        DebugState.should_show_frame_dump = false;
        std::unique_lock lock{DebugState.frame_dump_list_mutex};
        while (!DebugState.frame_dump_list.empty()) {
            const auto& frame_dump = DebugState.frame_dump_list.back();
            frame_viewers.emplace_back(frame_dump);
            DebugState.frame_dump_list.pop_back();
        }
        static bool first_time = true;
        if (first_time) {
            first_time = false;
            DebugState.ShowDebugMessage("Tip: You can shift+click any\n"
                                        "popup to open a new window");
        }
    }

    for (auto it = frame_viewers.begin(); it != frame_viewers.end();) {
        if (it->is_open) {
            it->Draw();
            ++it;
        } else {
            it = frame_viewers.erase(it);
        }
    }

    if (!DebugState.debug_message_popup.empty()) {
        if (debug_popup_timing > 0.0f) {
            debug_popup_timing -= io.DeltaTime;
            if (Begin("##devtools_msg", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove)) {
                BringWindowToDisplayFront(GetCurrentWindow());
                const auto display_size = io.DisplaySize;
                const auto& msg = DebugState.debug_message_popup.front();
                const auto padding = GetStyle().WindowPadding;
                const auto txt_size = CalcTextSize(&msg.front(), &msg.back() + 1, false, 250.0f);
                SetWindowPos({display_size.x - padding.x * 2.0f - txt_size.x, 50.0f});
                SetWindowSize({txt_size.x + padding.x * 2.0f, txt_size.y + padding.y * 2.0f});
                PushTextWrapPos(250.0f);
                TextEx(&msg.front(), &msg.back() + 1);
                PopTextWrapPos();
            }
            End();
        } else {
            DebugState.debug_message_popup.pop();
            debug_popup_timing = 3.0f;
        }
    }

    bool close_popup_options = true;
    if (BeginPopupModal("GPU Tools Options", &close_popup_options,
                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        static char disassembler_cli_isa[512];
        static char disassembler_cli_spv[512];
        static bool frame_dump_render_on_collapse;

        if (just_opened_options) {
            just_opened_options = false;
            auto s = Options.disassembler_cli_isa.copy(disassembler_cli_isa,
                                                       sizeof(disassembler_cli_isa) - 1);
            disassembler_cli_isa[s] = '\0';
            s = Options.disassembler_cli_spv.copy(disassembler_cli_spv,
                                                  sizeof(disassembler_cli_spv) - 1);
            disassembler_cli_spv[s] = '\0';
            frame_dump_render_on_collapse = Options.frame_dump_render_on_collapse;
        }

        InputText("Shader isa disassembler: ", disassembler_cli_isa, sizeof(disassembler_cli_isa));
        if (IsItemHovered()) {
            SetTooltip(R"(Command to disassemble shaders. Example: dis.exe --raw "{src}")");
        }
        InputText("Shader SPIRV disassembler: ", disassembler_cli_spv,
                  sizeof(disassembler_cli_spv));
        if (IsItemHovered()) {
            SetTooltip(R"(Command to disassemble shaders. Example: spirv-cross -V "{src}")");
        }
        Checkbox("Show frame dump popups even when collapsed", &frame_dump_render_on_collapse);
        if (IsItemHovered()) {
            SetTooltip("When a frame dump is collapsed, it will keep\n"
                       "showing all opened popups related to it");
        }

        if (Button("Save")) {
            Options.disassembler_cli_isa = disassembler_cli_isa;
            Options.disassembler_cli_spv = disassembler_cli_spv;
            Options.frame_dump_render_on_collapse = frame_dump_render_on_collapse;
            SaveIniSettingsToDisk(io.IniFilename);
            CloseCurrentPopup();
        }
        EndPopup();
    }

    if (BeginPopup("HelpTips", ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        CentralizeWindow();

        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0f});
        PushTextWrapPos(600.0f);

        const char* begin = help_text.data();
        TextUnformatted(begin, begin + help_text.size());

        PopTextWrapPos();
        PopStyleVar();

        EndPopup();
    }

    if (memory_map.open) {
        memory_map.Draw();
    }
    if (shader_list.open) {
        shader_list.Draw();
    }
    if (module_list.open) {
        module_list.Draw();
    }
    if (gpu_memory.open) {
        gpu_memory.Draw();
    }
}

void L::DrawSimple() {
    const float frameRate = DebugState.Framerate;
    if (frameRate < 10) {
        PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f)); // Red
    } else if (frameRate >= 10 && frameRate < 20) {
        PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f)); // Orange
    } else {
        PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)); // White
    }
    Text("%d FPS (%.1f ms)", static_cast<int>(std::round(frameRate)), 1000.0f / frameRate);
    PopStyleColor();
}

static void LoadSettings(const char* line) {
    int i;
    float f;
    if (sscanf(line, "fps_scale=%f", &f) == 1) {
        fps_scale = f;
        return;
    }
    if (sscanf(line, "show_advanced_debug=%d", &i) == 1) {
        // The debug menu bar starts hidden at every boot.
        return;
    }
    if (sscanf(line, "show_frame_graph=%d", &i) == 1) {
        frame_graph.is_open = i != 0;
        return;
    }
    if (sscanf(line, "show_shader_list=%d", &i) == 1) {
        shader_list.open = i != 0;
        return;
    }
    if (sscanf(line, "show_memory_map=%d", &i) == 1) {
        memory_map.open = i != 0;
        return;
    }
    if (sscanf(line, "show_module_list=%d", &i) == 1) {
        module_list.open = i != 0;
        return;
    }
    if (sscanf(line, "show_gpu_memory=%d", &i) == 1) {
        gpu_memory.open = i != 0;
        return;
    }
    if (sscanf(line, "dump_frame_count=%d", &i) == 1) {
        dump_frame_count = i;
        return;
    }
}

void L::SetupSettings() {
    frame_graph.is_open = true;
    show_simple_fps = EmulatorSettings.IsShowFpsCounter();
    visibility_toggled = show_simple_fps;

    using SettingLoader = void (*)(const char*);

    ImGuiSettingsHandler handler{};
    handler.TypeName = "DevtoolsLayer";
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
        if (std::string_view("Data") == name) {
            static_assert(std::is_same_v<decltype(&LoadSettings), SettingLoader>);
            return (void*)&LoadSettings;
        }
        if (std::string_view("CmdList") == name) {
            static_assert(
                std::is_same_v<decltype(&Widget::CmdListViewer::LoadConfig), SettingLoader>);
            return (void*)&Widget::CmdListViewer::LoadConfig;
        }
        if (std::string_view("Options") == name) {
            static_assert(std::is_same_v<decltype(&LoadOptionsConfig), SettingLoader>);
            return (void*)&LoadOptionsConfig;
        }
        return (void*)nullptr;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* handle, const char* line) {
        if (handle != nullptr) {
            reinterpret_cast<SettingLoader>(handle)(line);
        }
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        buf->appendf("[%s][Data]\n", handler->TypeName);
        buf->appendf("fps_scale=%f\n", fps_scale);
        buf->appendf("show_advanced_debug=%d\n", DebugState.IsShowingDebugMenuBar());
        buf->appendf("show_frame_graph=%d\n", frame_graph.is_open);
        buf->appendf("show_shader_list=%d\n", shader_list.open);
        buf->appendf("show_memory_map=%d\n", memory_map.open);
        buf->appendf("show_module_list=%d\n", module_list.open);
        buf->appendf("show_gpu_memory=%d\n", gpu_memory.open);
        buf->appendf("dump_frame_count=%d\n", dump_frame_count);
        buf->append("\n");
        buf->appendf("[%s][CmdList]\n", handler->TypeName);
        Widget::CmdListViewer::SerializeConfig(buf);
        buf->append("\n");
        buf->appendf("[%s][Options]\n", handler->TypeName);
        SerializeOptionsConfig(buf);
        buf->append("\n");
    };
    AddSettingsHandler(&handler);

    const ImGuiID dock_id = ImHashStr("FrameDumpDock");
    DockBuilderAddNode(dock_id, 0);
    DockBuilderSetNodePos(dock_id, ImVec2{450.0, 150.0});
    DockBuilderSetNodeSize(dock_id, ImVec2{400.0, 500.0});
    DockBuilderFinish(dock_id);
}

bool L::ShouldKeepDrawing() {
    return DebugState.IsShowingDebugMenuBar();
}

void L::Draw() {
    const auto io = GetIO();
    PushID("DevtoolsLayer");

    if (!DebugState.IsGuestThreadsPaused()) {
        const auto fn = DebugState.flip_frame_count.load();
        frame_graph.AddFrame(fn, DebugState.FrameDeltaTime);
    }

    if (IsKeyPressed(ImGuiKey_F10, false)) {
        if (io.KeyCtrl) {
            DebugState.IsShowingDebugMenuBar() ^= true;
        }
        visibility_toggled = true;
    }

    if (IsKeyPressed(ImGuiKey_F9, false)) {
        if (io.KeyCtrl && io.KeyAlt) {
            if (!DebugState.ShouldPauseInSubmit()) {
                DebugState.RequestFrameDump(dump_frame_count);
            }
        }
    }

    if (DebugState.IsGuestThreadsPaused()) {
        ImVec2 pos = ImVec2(10, 10);
        ImU32 color = IM_COL32(255, 255, 255, 255);
        ImGui::GetForegroundDrawList()->AddText(pos, color, "Emulation Paused");
    }

    if (show_simple_fps) {
        if (Begin("Video Info", nullptr,
                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
            if (visibility_toggled) {
                fps_pinned = true;
                visibility_toggled = false;
            }
            if (GetCurrentContext()->MovingWindow == GetCurrentWindowRead() &&
                IsMouseDragging(ImGuiMouseButton_Left)) {
                fps_pinned = false;
            }
            if (fps_pinned) {
                SetWindowPos({GetIO().DisplaySize.x - GetCurrentWindowRead()->SizeFull.x, 0.0f});
            }
            if (BeginPopupContextWindow()) {
#define M(label, value)                                                                            \
    if (MenuItem(label, nullptr, fps_scale == value))                                              \
    fps_scale = value
                M("0.5x", 0.5f);
                M("1.0x", 1.0f);
                M("1.5x", 1.5f);
                M("2.0x", 2.0f);
                M("2.5x", 2.5f);
                EndPopup();
#undef M
            }
            KeepWindowInside();
            SetWindowFontScale(fps_scale);
            DrawSimple();
        }
        End();
    }

    if (DebugState.IsShowingDebugMenuBar()) {
        PushFont(io.Fonts->Fonts[IMGUI_FONT_MONO]);
        PushID("DevtoolsLayer");
        DrawAdvanced();
        PopID();
        PopFont();
    }

    if (show_quit_window) {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (Begin("Quit Notification", nullptr,
                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
            SetWindowFontScale(1.5f);
            Overlay::TextCentered("Are you sure you want to quit?");
            NewLine();
            Text("Press Escape or Circle/B button to cancel");
            Text("Press Enter or Cross/A button to quit");

            if (IsKeyPressed(ImGuiKey_Escape, false) ||
                (IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
                show_quit_window = false;
            }

            if (IsKeyPressed(ImGuiKey_Enter, false) ||
                (IsKeyPressed(ImGuiKey_GamepadFaceDown, false))) {
                SDL_Event event;
                SDL_memset(&event, 0, sizeof(event));
                event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&event);
            }
        }
        End();
    }

    if (show_volume) {
        float current_time = ImGui::GetTime();

        // Show volume for 3 seconds
        if (current_time - volume_start_time >= 3.0) {
            show_volume = false;
        } else {
            SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x +
                                        ImGui::GetMainViewport()->WorkSize.x - 10,
                                    ImGui::GetMainViewport()->WorkPos.y + 10),
                             ImGuiCond_Always, ImVec2(1.0f, 0.0f));

            if (ImGui::Begin("Volume Window", &show_volume,
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
                Text("Volume: %d", EmulatorSettings.GetVolumeSlider());
            }
            End();
        }
    }

    PopID();
}

namespace Overlay {

void TextCentered(const std::string& text) {
    float window_width = GetContentRegionAvail().x;
    float text_width = CalcTextSize(text.c_str()).x;
    float text_indentation = (window_width - text_width) * 0.5f;

    SetCursorPosX(text_indentation);
    Text("%s", text.c_str());
}

void ToggleSimpleFps() {
    show_simple_fps = !show_simple_fps;
    visibility_toggled = true;
}

void SetSimpleFps(bool enabled) {
    show_simple_fps = enabled;
    visibility_toggled = true;
}

void ToggleQuitWindow() {
    show_quit_window = !show_quit_window;
}

void ShowVolume() {
    volume_start_time = ImGui::GetTime();
    show_volume = true;
}

} // namespace Overlay
