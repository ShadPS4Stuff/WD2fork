// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <mutex>
#include <thread>
#include <SDL3/SDL_keyboard.h>
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/libraries/vr_tracker/vr_tracker.h"

namespace Libraries::Hmd {

namespace {

struct UserEvent {
    Kernel::OrbisKernelEqueue queue = -1;
    s32 id = 0;
};

std::mutex g_reprojection_mutex;
UserEvent g_event_start;
UserEvent g_event_end;
std::jthread g_event_pump;
s32 g_display_handle = -1;
s32 g_preview_offset = 0;
bool g_preview_key_held = false;

void PresentPreviewBuffer(u64 frame) {
    if (!VrTracker::PsvrEmuEnabled() || g_display_handle < 0) {
        return;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool pressed = keys != nullptr && keys[SDL_SCANCODE_PAGEUP];
    if (pressed && !g_preview_key_held) {
        g_preview_offset = (g_preview_offset + 1) % 2;
        LOG_INFO(Lib_Hmd, "PSVR preview now shows the {}",
                 g_preview_offset == 0 ? "eye image" : "social screen");
    }
    g_preview_key_held = pressed;
    if (g_preview_offset == 0 && VideoOut::SubmitPsvrEyeFlip(g_display_handle)) {
        return;
    }
    VideoOut::sceVideoOutSubmitFlip(g_display_handle, static_cast<s32>(frame & 1), 1, 0);
}

void FireUserEvent(const UserEvent& event) {
    if (event.queue < 0) {
        return;
    }
    auto* equeue = Kernel::GetEqueue(event.queue);
    if (equeue == nullptr) {
        return;
    }
    if (!equeue->TriggerEvent(event.id, Kernel::OrbisKernelEvent::Filter::User, nullptr)) {
        Kernel::EqueueEvent user_event{};
        user_event.event.ident = event.id;
        user_event.event.filter = Kernel::OrbisKernelEvent::Filter::User;
        user_event.event.flags = Kernel::OrbisKernelEvent::Flags::Add;
        equeue->AddEvent(user_event);
        equeue->TriggerEvent(event.id, Kernel::OrbisKernelEvent::Filter::User, nullptr);
    }
}

void StartEventPump() {
    if (g_event_pump.joinable()) {
        return;
    }
    g_event_pump = std::jthread([](std::stop_token token) {
        Common::AccurateTimer timer{std::chrono::nanoseconds(1000000000 / 120)};
        while (!token.stop_requested()) {
            timer.Start();
            {
                std::scoped_lock lk{g_reprojection_mutex};
                FireUserEvent(g_event_start);
                FireUserEvent(g_event_end);
            }
            timer.End();
        }
    });
}

void StopEventPump() {
    if (g_event_pump.joinable()) {
        g_event_pump.request_stop();
        g_event_pump.join();
    }
}

void SetUserEvent(UserEvent& event, u64 queue, u64 id) {
    {
        std::scoped_lock lk{g_reprojection_mutex};
        event.queue = static_cast<Kernel::OrbisKernelEqueue>(queue);
        event.id = static_cast<s32>(id);
    }
    if (VrTracker::PsvrEmuEnabled()) {
        StartEventPump();
    }
}

void ClearUserEvent(UserEvent& event) {
    std::scoped_lock lk{g_reprojection_mutex};
    event = {};
}

} // namespace

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_TRACE(Lib_Hmd, "called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    PresentPreviewBuffer(a4);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionAddDisplayBuffer(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventEnd(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    ClearUserEvent(g_event_end);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventStart(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    ClearUserEvent(g_event_start);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfo() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfoMultilayer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalize(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    StopEventPump();
    ClearUserEvent(g_event_start);
    ClearUserEvent(g_event_end);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalizeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitialize(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffSize() {
    return 0x100000;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffSize() {
    return 0x810;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetCallback(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetDisplayBuffers(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    g_display_handle = static_cast<s32>(a0);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetOutputMinColor() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventEnd(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    SetUserEvent(g_event_end, a0, a1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventStart(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    SetUserEvent(g_event_start, a0, a1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer2(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNear(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNearWithOverlay(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWithOverlay(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStop(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called, args = {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", a0, a1, a2, a3,
              a4, a5);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetCallback() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetDisplayBuffers() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_A31A0320D80EAD99() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B9A6FA0735EC7E49() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterReprojection(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8gH1aLgty5I", "libsceHmdReprojectionMultilayer", 1, "libSceHmd",
                 sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("NTIbBpSH9ik", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionAddDisplayBuffer);
    LIB_FUNCTION("94+Ggm38KCg", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventEnd);
    LIB_FUNCTION("mdyFbaJj66M", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventStart);
    LIB_FUNCTION("MdV0akauNow", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionDebugGetLastInfo);
    LIB_FUNCTION("ymiwVjPB5+k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionDebugGetLastInfoMultilayer);
    LIB_FUNCTION("ZrV5YIqD09I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalize);
    LIB_FUNCTION("utHD2Ab-Ixo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalizeCapture);
    LIB_FUNCTION("OuygGEWkins", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitialize);
    LIB_FUNCTION("BTrQnC6fcAk", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitializeCapture);
    LIB_FUNCTION("TkcANcGM0s8", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionQueryGarlicBuffAlign);
    LIB_FUNCTION("z0KtN1vqF2E", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryGarlicBuffSize);
    LIB_FUNCTION("IWybWbR-xvA", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffAlign);
    LIB_FUNCTION("kLUAkN6a1e8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffSize);
    LIB_FUNCTION("6CRWGc-evO4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetCallback);
    LIB_FUNCTION("E+dPfjeQLHI", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetDisplayBuffers);
    LIB_FUNCTION("LjdLRysHU6Y", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetOutputMinColor);
    LIB_FUNCTION("knyIhlkpLgE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventEnd);
    LIB_FUNCTION("7as0CjXW1B8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventStart);
    LIB_FUNCTION("dntZTJ7meIU", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart);
    LIB_FUNCTION("q3e8+nEguyE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart2dVr);
    LIB_FUNCTION("RrvyU1pjb9A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartCapture);
    LIB_FUNCTION("XZ5QUzb4ae0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartLiveCapture);
    LIB_FUNCTION("8gH1aLgty5I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("gqAG7JYeE7A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer2);
    LIB_FUNCTION("3JyuejcNhC0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWideNear);
    LIB_FUNCTION("mKa8scOc4-k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionStartWideNearWithOverlay);
    LIB_FUNCTION("kcldQ7zLYQQ", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWithOverlay);
    LIB_FUNCTION("vzMEkwBQciM", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStop);
    LIB_FUNCTION("F7Sndm5teWw", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopCapture);
    LIB_FUNCTION("PAa6cUL5bR4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopLiveCapture);
    LIB_FUNCTION("0wnZViigP9o", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetCallback);
    LIB_FUNCTION("iGNNpDDjcwo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetDisplayBuffers);
    LIB_FUNCTION("oxoDINgOrZk", "libSceHmd", 1, "libSceHmd", Func_A31A0320D80EAD99);
    LIB_FUNCTION("uab6BzXsfkk", "libSceHmd", 1, "libSceHmd", Func_B9A6FA0735EC7E49);
}
} // namespace Libraries::Hmd