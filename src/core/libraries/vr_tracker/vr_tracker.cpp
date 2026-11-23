// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <iterator>
#include <string>
#include <vector>
#include <mutex>
#include <SDL3/SDL_keyboard.h>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include "../../../../externals/sdl3/src/video/khronos/openxr/openxr.h"
#include "../../../../externals/sdl3/src/video/khronos/openxr/openxr_platform.h"
#endif

namespace Libraries::VrTracker {

static bool g_library_initialized = false;

// Internal memory
static void* g_garlic_memory_pointer = nullptr;
static u32 g_garlic_size = 0;
static void* g_onion_memory_pointer = nullptr;
static u32 g_onion_size = 0;
static void* g_work_memory_pointer = nullptr;
static u32 g_work_size = 0;

// Registered handles
static s32 g_pad_handle = -1;
static s32 g_move_handle = -1;
static s32 g_gun_handle = -1;
static s32 g_hmd_handle = -1;
static s32 g_move_handle2 = -1;

namespace {

struct Vec3 {
    float x, y, z;
};

struct EmuPose {
    Vec3 position;
    float qx, qy, qz, qw;
};

struct EmuState {
    std::mutex mutex;
    std::chrono::steady_clock::time_point last{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    Vec3 head{0.0f, 0.0f, 0.0f};
    PsvrEmuMoveState moves[2]{};
};

EmuState g_emu;

constexpr float LookSpeed = 1.6f;
constexpr float WalkSpeed = 1.0f;
constexpr float MaxPitch = 1.4f;
constexpr float EyeHalfDistance = 0.0315f;
constexpr Vec3 HandRest{0.2f, -0.55f, -0.3f};

Vec3 RotateYaw(Vec3 v, float yaw) {
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

float Axis(const bool* keys, SDL_Scancode positive, SDL_Scancode negative) {
    return static_cast<float>(keys[positive]) - static_cast<float>(keys[negative]);
}

PsvrEmuMoveState ReadMoveKeys(const bool* keys, SDL_Scancode trigger, SDL_Scancode move,
                              SDL_Scancode cross, SDL_Scancode circle, SDL_Scancode square,
                              SDL_Scancode triangle, SDL_Scancode start) {
    PsvrEmuMoveState state{};
    state.trigger = keys[trigger] ? 255 : 0;
    state.buttons |= keys[trigger] ? PsvrEmuButtonT : 0;
    state.buttons |= keys[move] ? PsvrEmuButtonMove : 0;
    state.buttons |= keys[cross] ? PsvrEmuButtonCross : 0;
    state.buttons |= keys[circle] ? PsvrEmuButtonCircle : 0;
    state.buttons |= keys[square] ? PsvrEmuButtonSquare : 0;
    state.buttons |= keys[triangle] ? PsvrEmuButtonTriangle : 0;
    state.buttons |= keys[start] ? PsvrEmuButtonStart : 0;
    state.reach = keys[SDL_SCANCODE_LCTRL];
    state.raise = keys[SDL_SCANCODE_LSHIFT];
    return state;
}

#ifdef _WIN32
using PFN_ConvertPerfCounterToTime = XrResult(XRAPI_PTR*)(XrInstance, const LARGE_INTEGER*,
                                                          XrTime*);

struct OpenXrBackend {
    bool tried = false;
    bool ready = false;
    std::atomic<bool> running = false;
    HMODULE library = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE;
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction pose = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE;
    XrAction grip = XR_NULL_HANDLE;
    XrAction lower = XR_NULL_HANDLE;
    XrAction upper = XR_NULL_HANDLE;
    XrAction stick = XR_NULL_HANDLE;
    XrAction stick_click = XR_NULL_HANDLE;
    XrPath hands[2]{};
    XrSpace hand_spaces[2]{};
    bool has_vulkan = false;
    bool has_headless = false;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> swap_images;
    u32 swap_width = 0;
    u32 swap_height = 0;
    s64 swap_format = 0;
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool frame_begun = false;
    bool image_acquired = false;
    std::vector<std::string> vk_instance_extensions;
    std::vector<std::string> vk_device_extensions;

    PFN_xrGetInstanceProcAddr GetProc = nullptr;
    PFN_xrCreateInstance CreateInstance = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrGetSystem GetSystem = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrEndSession EndSession = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrCreateActionSpace CreateActionSpace = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;
    PFN_xrStringToPath StringToPath = nullptr;
    PFN_xrCreateActionSet CreateActionSet = nullptr;
    PFN_xrCreateAction CreateAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings SuggestBindings = nullptr;
    PFN_xrAttachSessionActionSets AttachActionSets = nullptr;
    PFN_xrSyncActions SyncActions = nullptr;
    PFN_xrGetActionStateFloat GetFloat = nullptr;
    PFN_xrGetActionStateBoolean GetBool = nullptr;
    PFN_xrGetActionStateVector2f GetVector2 = nullptr;
    PFN_ConvertPerfCounterToTime ConvertTime = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties EnumerateExtensions = nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR GetVkInstanceExtensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR GetVkDeviceExtensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR GetVkGraphicsDevice = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR GetVkRequirements = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
};

OpenXrBackend g_xr;

struct OpenXrPoses {
    EmuPose head{{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 1.0f};
    EmuPose hands[2]{{{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 1.0f},
                     {{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 1.0f}};
    bool hand_tracked[2]{};
};

OpenXrPoses g_xr_poses;

template <typename T>
bool LoadXrFunction(XrInstance instance, const char* name, T& out) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(g_xr.GetProc(instance, name, &function)) || function == nullptr) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: missing {}", name);
        return false;
    }
    out = reinterpret_cast<T>(function);
    return true;
}

XrAction MakeXrAction(const char* name, XrActionType type) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = g_xr.hands;
    XrAction action = XR_NULL_HANDLE;
    g_xr.CreateAction(g_xr.action_set, &info, &action);
    return action;
}

XrPath XrPathOf(const char* path) {
    XrPath out = XR_NULL_PATH;
    g_xr.StringToPath(g_xr.instance, path, &out);
    return out;
}

std::vector<std::string> SplitNames(const std::string& names) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < names.size()) {
        const size_t end = names.find(' ', start);
        const size_t stop = end == std::string::npos ? names.size() : end;
        if (stop > start) {
            out.emplace_back(names.substr(start, stop - start));
        }
        start = stop + 1;
    }
    return out;
}

template <typename F>
std::vector<std::string> QueryVulkanExtensions(F query) {
    u32 size = 0;
    if (query == nullptr || XR_FAILED(query(g_xr.instance, g_xr.system, 0, &size, nullptr))) {
        return {};
    }
    std::string names(size, '\0');
    query(g_xr.instance, g_xr.system, size, &size, names.data());
    names.resize(std::strlen(names.c_str()));
    return SplitNames(names);
}

std::mutex g_xr_init_mutex;
bool g_xr_instance_tried = false;
bool g_xr_instance_ready = false;

bool CreateXrInstance() {
    g_xr.library = LoadLibraryA("openxr_loader.dll");
    if (g_xr.library == nullptr) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: openxr_loader.dll not found, using keyboard");
        return false;
    }
    g_xr.GetProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(g_xr.library, "xrGetInstanceProcAddr"));
    if (g_xr.GetProc == nullptr ||
        !LoadXrFunction(XR_NULL_HANDLE, "xrCreateInstance", g_xr.CreateInstance) ||
        !LoadXrFunction(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                        g_xr.EnumerateExtensions)) {
        return false;
    }

    u32 count = 0;
    g_xr.EnumerateExtensions(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    g_xr.EnumerateExtensions(nullptr, count, &count, available.data());
    const auto has = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const auto& ext) {
            return std::strcmp(ext.extensionName, name) == 0;
        });
    };
    if (!has("XR_KHR_win32_convert_performance_counter_time")) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: runtime lacks win32 time conversion");
        return false;
    }
    g_xr.has_vulkan = has(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    g_xr.has_headless = has(XR_MND_HEADLESS_EXTENSION_NAME);

    std::vector<const char*> extensions{
        "XR_KHR_win32_convert_performance_counter_time"};
    if (g_xr.has_vulkan) {
        extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    }
    if (g_xr.has_headless) {
        extensions.push_back(XR_MND_HEADLESS_EXTENSION_NAME);
    }
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(instance_info.applicationInfo.applicationName, "shadPS4",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    instance_info.enabledExtensionNames = extensions.data();
    if (const XrResult res = g_xr.CreateInstance(&instance_info, &g_xr.instance); XR_FAILED(res)) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: xrCreateInstance failed ({})", static_cast<s32>(res));
        return false;
    }

    const XrInstance inst = g_xr.instance;
    if (!LoadXrFunction(inst, "xrDestroyInstance", g_xr.DestroyInstance) ||
        !LoadXrFunction(inst, "xrGetSystem", g_xr.GetSystem) ||
        !LoadXrFunction(inst, "xrCreateSession", g_xr.CreateSession) ||
        !LoadXrFunction(inst, "xrBeginSession", g_xr.BeginSession) ||
        !LoadXrFunction(inst, "xrEndSession", g_xr.EndSession) ||
        !LoadXrFunction(inst, "xrPollEvent", g_xr.PollEvent) ||
        !LoadXrFunction(inst, "xrCreateReferenceSpace", g_xr.CreateReferenceSpace) ||
        !LoadXrFunction(inst, "xrCreateActionSpace", g_xr.CreateActionSpace) ||
        !LoadXrFunction(inst, "xrLocateSpace", g_xr.LocateSpace) ||
        !LoadXrFunction(inst, "xrStringToPath", g_xr.StringToPath) ||
        !LoadXrFunction(inst, "xrCreateActionSet", g_xr.CreateActionSet) ||
        !LoadXrFunction(inst, "xrCreateAction", g_xr.CreateAction) ||
        !LoadXrFunction(inst, "xrSuggestInteractionProfileBindings", g_xr.SuggestBindings) ||
        !LoadXrFunction(inst, "xrAttachSessionActionSets", g_xr.AttachActionSets) ||
        !LoadXrFunction(inst, "xrSyncActions", g_xr.SyncActions) ||
        !LoadXrFunction(inst, "xrGetActionStateFloat", g_xr.GetFloat) ||
        !LoadXrFunction(inst, "xrGetActionStateBoolean", g_xr.GetBool) ||
        !LoadXrFunction(inst, "xrGetActionStateVector2f", g_xr.GetVector2) ||
        !LoadXrFunction(inst, "xrConvertWin32PerformanceCounterToTimeKHR", g_xr.ConvertTime) ||
        !LoadXrFunction(inst, "xrWaitFrame", g_xr.WaitFrame) ||
        !LoadXrFunction(inst, "xrBeginFrame", g_xr.BeginFrame) ||
        !LoadXrFunction(inst, "xrEndFrame", g_xr.EndFrame) ||
        !LoadXrFunction(inst, "xrLocateViews", g_xr.LocateViews) ||
        !LoadXrFunction(inst, "xrEnumerateSwapchainFormats", g_xr.EnumerateSwapchainFormats) ||
        !LoadXrFunction(inst, "xrCreateSwapchain", g_xr.CreateSwapchain) ||
        !LoadXrFunction(inst, "xrDestroySwapchain", g_xr.DestroySwapchain) ||
        !LoadXrFunction(inst, "xrEnumerateSwapchainImages", g_xr.EnumerateSwapchainImages) ||
        !LoadXrFunction(inst, "xrAcquireSwapchainImage", g_xr.AcquireSwapchainImage) ||
        !LoadXrFunction(inst, "xrWaitSwapchainImage", g_xr.WaitSwapchainImage) ||
        !LoadXrFunction(inst, "xrReleaseSwapchainImage", g_xr.ReleaseSwapchainImage)) {
        return false;
    }
    if (g_xr.has_vulkan &&
        (!LoadXrFunction(inst, "xrGetVulkanInstanceExtensionsKHR",
                         g_xr.GetVkInstanceExtensions) ||
         !LoadXrFunction(inst, "xrGetVulkanDeviceExtensionsKHR", g_xr.GetVkDeviceExtensions) ||
         !LoadXrFunction(inst, "xrGetVulkanGraphicsDeviceKHR", g_xr.GetVkGraphicsDevice) ||
         !LoadXrFunction(inst, "xrGetVulkanGraphicsRequirementsKHR", g_xr.GetVkRequirements))) {
        g_xr.has_vulkan = false;
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (const XrResult res = g_xr.GetSystem(inst, &system_info, &g_xr.system); XR_FAILED(res)) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: no headset found ({})", static_cast<s32>(res));
        return false;
    }

    if (g_xr.has_vulkan) {
        g_xr.vk_instance_extensions = QueryVulkanExtensions(g_xr.GetVkInstanceExtensions);
        g_xr.vk_device_extensions = QueryVulkanExtensions(g_xr.GetVkDeviceExtensions);
    }
    return true;
}

bool EnsureXrInstance() {
    std::scoped_lock lk{g_xr_init_mutex};
    if (!g_xr_instance_tried) {
        g_xr_instance_tried = true;
        g_xr_instance_ready = CreateXrInstance();
    }
    return g_xr_instance_ready;
}

bool UseVulkanBinding() {
    if (!g_xr.has_vulkan || g_xr.binding.device == VK_NULL_HANDLE) {
        return false;
    }
    VkPhysicalDevice xr_device = VK_NULL_HANDLE;
    g_xr.GetVkGraphicsDevice(g_xr.instance, g_xr.system, g_xr.binding.instance, &xr_device);
    if (xr_device != g_xr.binding.physicalDevice) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: headset is on a different GPU, display disabled");
        return false;
    }
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    g_xr.GetVkRequirements(g_xr.instance, g_xr.system, &requirements);
    return true;
}

bool InitOpenXr() {
    if (!EnsureXrInstance()) {
        return false;
    }
    const XrInstance inst = g_xr.instance;

    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.systemId = g_xr.system;
    const bool vulkan = UseVulkanBinding();
    if (vulkan) {
        session_info.next = &g_xr.binding;
    } else if (!g_xr.has_headless) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: no Vulkan binding and no headless support");
        return false;
    }
    if (const XrResult res = g_xr.CreateSession(inst, &session_info, &g_xr.session);
        XR_FAILED(res)) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: xrCreateSession failed ({})", static_cast<s32>(res));
        return false;
    }
    g_xr.has_vulkan = vulkan;
    LOG_INFO(Lib_VrTracker, "OpenXR: session created, headset display {}",
             vulkan ? "enabled" : "disabled");

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    g_xr.CreateReferenceSpace(g_xr.session, &space_info, &g_xr.local);
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    g_xr.CreateReferenceSpace(g_xr.session, &space_info, &g_xr.view);

    g_xr.hands[0] = XrPathOf("/user/hand/right");
    g_xr.hands[1] = XrPathOf("/user/hand/left");

    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(set_info.actionSetName, "psvr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "PSVR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    g_xr.CreateActionSet(inst, &set_info, &g_xr.action_set);

    g_xr.pose = MakeXrAction("move_pose", XR_ACTION_TYPE_POSE_INPUT);
    g_xr.trigger = MakeXrAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    g_xr.grip = MakeXrAction("move_button", XR_ACTION_TYPE_FLOAT_INPUT);
    g_xr.lower = MakeXrAction("cross", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_xr.upper = MakeXrAction("circle", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_xr.stick = MakeXrAction("triangle_square", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_xr.stick_click = MakeXrAction("start_select", XR_ACTION_TYPE_BOOLEAN_INPUT);

    const XrActionSuggestedBinding bindings[] = {
        {g_xr.pose, XrPathOf("/user/hand/right/input/aim/pose")},
        {g_xr.pose, XrPathOf("/user/hand/left/input/aim/pose")},
        {g_xr.trigger, XrPathOf("/user/hand/right/input/trigger/value")},
        {g_xr.trigger, XrPathOf("/user/hand/left/input/trigger/value")},
        {g_xr.grip, XrPathOf("/user/hand/right/input/squeeze/value")},
        {g_xr.grip, XrPathOf("/user/hand/left/input/squeeze/value")},
        {g_xr.lower, XrPathOf("/user/hand/right/input/a/click")},
        {g_xr.lower, XrPathOf("/user/hand/left/input/x/click")},
        {g_xr.upper, XrPathOf("/user/hand/right/input/b/click")},
        {g_xr.upper, XrPathOf("/user/hand/left/input/y/click")},
        {g_xr.stick, XrPathOf("/user/hand/right/input/thumbstick")},
        {g_xr.stick, XrPathOf("/user/hand/left/input/thumbstick")},
        {g_xr.stick_click, XrPathOf("/user/hand/right/input/thumbstick/click")},
        {g_xr.stick_click, XrPathOf("/user/hand/left/input/thumbstick/click")},
    };
    XrInteractionProfileSuggestedBinding suggested{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = XrPathOf("/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = static_cast<u32>(std::size(bindings));
    suggested.suggestedBindings = bindings;
    if (const XrResult res = g_xr.SuggestBindings(inst, &suggested); XR_FAILED(res)) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: binding suggestion failed ({})", static_cast<s32>(res));
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_xr.action_set;
    g_xr.AttachActionSets(g_xr.session, &attach);

    for (s32 i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo action_space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space.action = g_xr.pose;
        action_space.subactionPath = g_xr.hands[i];
        action_space.poseInActionSpace.orientation.w = 1.0f;
        g_xr.CreateActionSpace(g_xr.session, &action_space, &g_xr.hand_spaces[i]);
    }

    return true;
}

void PumpXrEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (g_xr.PollEvent(g_xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_xr.running = XR_SUCCEEDED(g_xr.BeginSession(g_xr.session, &begin));
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                g_xr.EndSession(g_xr.session);
                g_xr.running = false;
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

EmuPose ToEmuPose(const XrPosef& pose) {
    return {{pose.position.x, pose.position.y, pose.position.z},
            pose.orientation.x,
            pose.orientation.y,
            pose.orientation.z,
            pose.orientation.w};
}

bool XrBool(XrAction action, s32 hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = g_xr.hands[hand];
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(g_xr.GetBool(g_xr.session, &info, &state)) && state.currentState;
}

float XrFloat(XrAction action, s32 hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = g_xr.hands[hand];
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(g_xr.GetFloat(g_xr.session, &info, &state)) ? state.currentState : 0.0f;
}

XrVector2f XrStick(s32 hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = g_xr.stick;
    info.subactionPath = g_xr.hands[hand];
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(g_xr.GetVector2(g_xr.session, &info, &state)) ? state.currentState
                                                                       : XrVector2f{};
}

PsvrEmuMoveState ReadXrMove(s32 hand) {
    PsvrEmuMoveState state{};
    const float trigger = XrFloat(g_xr.trigger, hand);
    const XrVector2f stick = XrStick(hand);
    state.trigger = static_cast<u16>(std::fmin(trigger, 1.0f) * 255.0f);
    state.buttons |= trigger > 0.1f ? PsvrEmuButtonT : 0;
    state.buttons |= XrFloat(g_xr.grip, hand) > 0.5f ? PsvrEmuButtonMove : 0;
    state.buttons |= XrBool(g_xr.lower, hand) ? PsvrEmuButtonCross : 0;
    state.buttons |= XrBool(g_xr.upper, hand) ? PsvrEmuButtonCircle : 0;
    state.buttons |= stick.y > 0.6f ? PsvrEmuButtonTriangle : 0;
    state.buttons |= stick.y < -0.6f ? PsvrEmuButtonSquare : 0;
    if (XrBool(g_xr.stick_click, hand)) {
        state.buttons |= hand == 0 ? PsvrEmuButtonStart : PsvrEmuButtonSelect;
    }
    return state;
}

bool XrLocate(XrSpace space, XrTime time, EmuPose& out) {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(g_xr.LocateSpace(space, g_xr.local, time, &location))) {
        return false;
    }
    constexpr XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((location.locationFlags & valid) != valid) {
        return false;
    }
    out = ToEmuPose(location.pose);
    return true;
}

bool PollOpenXr() {
    if (!EmulatorSettings.IsPsvrOpenXr()) {
        return false;
    }
    if (!g_xr.tried) {
        g_xr.tried = true;
        g_xr.ready = InitOpenXr();
    }
    if (!g_xr.ready) {
        return false;
    }
    PumpXrEvents();
    if (!g_xr.running) {
        return true;
    }

    XrActiveActionSet active{g_xr.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    g_xr.SyncActions(g_xr.session, &sync);

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    XrTime time = 0;
    g_xr.ConvertTime(g_xr.instance, &counter, &time);

    XrLocate(g_xr.view, time, g_xr_poses.head);
    for (s32 i = 0; i < 2; i++) {
        g_xr_poses.hand_tracked[i] = XrLocate(g_xr.hand_spaces[i], time, g_xr_poses.hands[i]);
        g_emu.moves[i] = ReadXrMove(i);
    }
    return true;
}

bool EnsureXrSwapchain(u32 width, u32 height, s64 format) {
    if (g_xr.swapchain != XR_NULL_HANDLE && g_xr.swap_width == width &&
        g_xr.swap_height == height && g_xr.swap_format == format) {
        return true;
    }
    if (g_xr.swapchain != XR_NULL_HANDLE) {
        g_xr.DestroySwapchain(g_xr.swapchain);
        g_xr.swapchain = XR_NULL_HANDLE;
    }

    u32 count = 0;
    g_xr.EnumerateSwapchainFormats(g_xr.session, 0, &count, nullptr);
    std::vector<s64> formats(count);
    g_xr.EnumerateSwapchainFormats(g_xr.session, count, &count, formats.data());
    if (std::find(formats.begin(), formats.end(), format) == formats.end()) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: swapchain format {} unsupported", format);
        return false;
    }

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (const XrResult res = g_xr.CreateSwapchain(g_xr.session, &info, &g_xr.swapchain);
        XR_FAILED(res)) {
        LOG_ERROR(Lib_VrTracker, "OpenXR: xrCreateSwapchain failed ({})", static_cast<s32>(res));
        g_xr.swapchain = XR_NULL_HANDLE;
        return false;
    }
    g_xr.EnumerateSwapchainImages(g_xr.swapchain, 0, &count, nullptr);
    g_xr.swap_images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    g_xr.EnumerateSwapchainImages(
        g_xr.swapchain, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(g_xr.swap_images.data()));
    g_xr.swap_width = width;
    g_xr.swap_height = height;
    g_xr.swap_format = format;
    LOG_INFO(Lib_VrTracker, "OpenXR: swapchain {}x{} with {} images", width, height, count);
    return true;
}
#else
bool PollOpenXr() {
    return false;
}
#endif

Vec3 RotateByQuat(Vec3 v, const EmuPose& q) {
    const Vec3 u{q.qx, q.qy, q.qz};
    const Vec3 t{2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z),
                 2.0f * (u.x * v.y - u.y * v.x)};
    return {v.x + q.qw * t.x + (u.y * t.z - u.z * t.y),
            v.y + q.qw * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.qw * t.z + (u.x * t.y - u.y * t.x)};
}

bool g_xr_active = false;

void PollKeyboard() {
    std::scoped_lock lk{g_emu.mutex};
    g_xr_active = PollOpenXr();
    if (g_xr_active) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool first = g_emu.last == std::chrono::steady_clock::time_point{};
    const float dt =
        first ? 0.0f : std::fmin(std::chrono::duration<float>(now - g_emu.last).count(), 0.1f);
    g_emu.last = now;

    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys == nullptr) {
        return;
    }

    if (keys[SDL_SCANCODE_BACKSPACE]) {
        g_emu.yaw = 0.0f;
        g_emu.pitch = 0.0f;
        g_emu.head = {0.0f, 0.0f, 0.0f};
    }

    const float look_x = Axis(keys, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT) +
                         Axis(keys, SDL_SCANCODE_F, SDL_SCANCODE_H);
    const float look_y =
        Axis(keys, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN) + Axis(keys, SDL_SCANCODE_T, SDL_SCANCODE_G);
    g_emu.yaw += std::fmax(-1.0f, std::fmin(1.0f, look_x)) * LookSpeed * dt;
    g_emu.pitch += std::fmax(-1.0f, std::fmin(1.0f, look_y)) * LookSpeed * dt;
    g_emu.pitch = std::fmax(-MaxPitch, std::fmin(MaxPitch, g_emu.pitch));

    const Vec3 walk = RotateYaw({Axis(keys, SDL_SCANCODE_D, SDL_SCANCODE_A),
                                 Axis(keys, SDL_SCANCODE_O, SDL_SCANCODE_U),
                                 Axis(keys, SDL_SCANCODE_S, SDL_SCANCODE_W)},
                                g_emu.yaw);
    g_emu.head.x += walk.x * WalkSpeed * dt;
    g_emu.head.y += walk.y * WalkSpeed * dt;
    g_emu.head.z += walk.z * WalkSpeed * dt;

    g_emu.moves[1] = ReadMoveKeys(keys, SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_Z,
                                  SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_TAB);
    g_emu.moves[0] =
        ReadMoveKeys(keys, SDL_SCANCODE_SPACE, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_J,
                     SDL_SCANCODE_B, SDL_SCANCODE_Y, SDL_SCANCODE_RETURN);
}

EmuPose HeadPose() {
    std::scoped_lock lk{g_emu.mutex};
#ifdef _WIN32
    if (g_xr_active) {
        return g_xr_poses.head;
    }
#endif
    const float sy = std::sin(g_emu.yaw * 0.5f);
    const float cy = std::cos(g_emu.yaw * 0.5f);
    const float sp = std::sin(g_emu.pitch * 0.5f);
    const float cp = std::cos(g_emu.pitch * 0.5f);
    return {g_emu.head, cy * sp, sy * cp, -sy * sp, cy * cp};
}

EmuPose EyePose(float side) {
    EmuPose pose = HeadPose();
    std::scoped_lock lk{g_emu.mutex};
    const Vec3 offset = g_xr_active ? RotateByQuat({side, 0.0f, 0.0f}, pose)
                                    : RotateYaw({side, 0.0f, 0.0f}, g_emu.yaw);
    pose.position = {pose.position.x + offset.x, pose.position.y + offset.y,
                     pose.position.z + offset.z};
    return pose;
}

EmuPose HandPose(s32 index) {
    std::scoped_lock lk{g_emu.mutex};
#ifdef _WIN32
    if (g_xr_active) {
        return g_xr_poses.hands[index];
    }
#endif
    const auto& move = g_emu.moves[index];
    Vec3 offset{index == 0 ? HandRest.x : -HandRest.x, HandRest.y, HandRest.z};
    if (move.reach) {
        offset.z -= 0.3f;
    }
    if (move.raise) {
        offset.y += 0.45f;
    }
    const Vec3 world = RotateYaw(offset, g_emu.yaw);
    return {{g_emu.head.x + world.x, g_emu.head.y + world.y, g_emu.head.z + world.z},
            0.0f,
            std::sin(g_emu.yaw * 0.5f),
            0.0f,
            std::cos(g_emu.yaw * 0.5f)};
}

bool HandTracked(s32 index) {
    std::scoped_lock lk{g_emu.mutex};
#ifdef _WIN32
    if (g_xr_active) {
        return g_xr_poses.hand_tracked[index];
    }
#endif
    return true;
}

void WritePose(OrbisVrTrackerPoseData& out, const EmuPose& pose) {
    out.position_x = pose.position.x;
    out.position_y = pose.position.y;
    out.position_z = pose.position.z;
    out.orientation_x = pose.qx;
    out.orientation_y = pose.qy;
    out.orientation_z = pose.qz;
    out.orientation_w = pose.qw;
}

} // namespace

bool PsvrEmuEnabled() {
    return EmulatorSettings.IsPsvrEmuTest();
}

#ifdef _WIN32
const std::vector<std::string>& PsvrXrVulkanInstanceExtensions() {
    static const std::vector<std::string> none;
    if (!EmulatorSettings.IsPsvrOpenXr() || !EnsureXrInstance()) {
        return none;
    }
    return g_xr.vk_instance_extensions;
}

const std::vector<std::string>& PsvrXrVulkanDeviceExtensions() {
    static const std::vector<std::string> none;
    if (!EmulatorSettings.IsPsvrOpenXr() || !EnsureXrInstance()) {
        return none;
    }
    return g_xr.vk_device_extensions;
}

void PsvrXrSetVulkan(void* instance, void* physical_device, void* device, u32 queue_family,
                     u32 queue_index) {
    g_xr.binding.instance = static_cast<VkInstance>(instance);
    g_xr.binding.physicalDevice = static_cast<VkPhysicalDevice>(physical_device);
    g_xr.binding.device = static_cast<VkDevice>(device);
    g_xr.binding.queueFamilyIndex = queue_family;
    g_xr.binding.queueIndex = queue_index;
}

bool PsvrXrBeginFrame(u32 width, u32 height, s64 format, PsvrXrImage& out) {
    if (!g_xr.ready || !g_xr.has_vulkan || !g_xr.running) {
        return false;
    }
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    g_xr.frame_state = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(g_xr.WaitFrame(g_xr.session, &wait, &g_xr.frame_state))) {
        return false;
    }
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(g_xr.BeginFrame(g_xr.session, &begin))) {
        return false;
    }
    g_xr.frame_begun = true;
    if (!g_xr.frame_state.shouldRender || !EnsureXrSwapchain(width, height, format)) {
        return false;
    }

    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g_xr.frame_state.predictedDisplayTime;
    locate.space = g_xr.local;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    u32 view_count = 0;
    g_xr.views[0] = {XR_TYPE_VIEW};
    g_xr.views[1] = {XR_TYPE_VIEW};
    if (XR_FAILED(g_xr.LocateViews(g_xr.session, &locate, &view_state, 2, &view_count,
                                   g_xr.views)) ||
        view_count != 2) {
        return false;
    }

    u32 index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(g_xr.AcquireSwapchainImage(g_xr.swapchain, &acquire, &index))) {
        return false;
    }
    XrSwapchainImageWaitInfo wait_image{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_image.timeout = XR_INFINITE_DURATION;
    g_xr.WaitSwapchainImage(g_xr.swapchain, &wait_image);
    g_xr.image_acquired = true;
    out.image = g_xr.swap_images[index].image;
    out.width = width;
    out.height = height;
    return true;
}

void PsvrXrEndFrame() {
    if (!g_xr.frame_begun) {
        return;
    }
    g_xr.frame_begun = false;

    XrCompositionLayerProjectionView views[2]{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1]{};
    u32 layer_count = 0;
    if (g_xr.image_acquired) {
        g_xr.image_acquired = false;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        g_xr.ReleaseSwapchainImage(g_xr.swapchain, &release);
        const s32 half = static_cast<s32>(g_xr.swap_width / 2);
        for (s32 i = 0; i < 2; i++) {
            views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[i].pose = g_xr.views[i].pose;
            views[i].fov = g_xr.views[i].fov;
            views[i].subImage.swapchain = g_xr.swapchain;
            views[i].subImage.imageRect.offset = {i * half, 0};
            views[i].subImage.imageRect.extent = {half, static_cast<s32>(g_xr.swap_height)};
        }
        layer.space = g_xr.local;
        layer.viewCount = 2;
        layer.views = views;
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        layer_count = 1;
    }

    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = g_xr.frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = layer_count;
    end.layers = layers;
    g_xr.EndFrame(g_xr.session, &end);
}
#else
const std::vector<std::string>& PsvrXrVulkanInstanceExtensions() {
    static const std::vector<std::string> none;
    return none;
}

const std::vector<std::string>& PsvrXrVulkanDeviceExtensions() {
    static const std::vector<std::string> none;
    return none;
}

void PsvrXrSetVulkan(void*, void*, void*, u32, u32) {}

bool PsvrXrBeginFrame(u32, u32, s64, PsvrXrImage&) {
    return false;
}

void PsvrXrEndFrame() {}
#endif

PsvrEmuMoveState PsvrEmuGetMove(s32 index) {
    PollKeyboard();
    std::scoped_lock lk{g_emu.mutex};
    return g_emu.moves[index & 1];
}

s32 PS4_SYSV_ABI sceVrTrackerQueryMemory(const OrbisVrTrackerQueryMemoryParam* param,
                                         OrbisVrTrackerQueryMemoryResult* result) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (param == nullptr || result == nullptr ||
        param->size != sizeof(OrbisVrTrackerQueryMemoryParam) ||
        (param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_000 &&
         param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_100) ||
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        // Hmd doesn't support auto calibration
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Setting move_position to ORBIS_VR_TRACKER_CALIBRATION_AUTO doubles required onion memory.
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position ==
        OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    result->direct_memory_onion_size = required_onion_size;
    result->direct_memory_onion_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    result->direct_memory_garlic_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    result->work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerInit(const OrbisVrTrackerInitParam* param) {
    LOG_INFO(Lib_VrTracker,
             "called, size = {}, garlic = {:#x} ({:#x}), onion = {:#x} ({:#x}), work = {:#x} ({:#x}), "
             "pipe = {}, queue = {}",
             param->size, reinterpret_cast<uintptr_t>(param->direct_memory_garlic), param->direct_memory_garlic_size,
             reinterpret_cast<uintptr_t>(param->direct_memory_onion), param->direct_memory_onion_size,
             reinterpret_cast<uintptr_t>(param->work_memory), param->work_memory_size, param->gpu_pipe_id, param->gpu_queue_id);
    if (g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_ALREADY_INITIALIZED;
    }

    // Calculate correct onion size for parameter checks
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position == ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    // Parameter checks are fairly thorough here.
    if (param->size != sizeof(OrbisVrTrackerInitParam) ||
        // Check garlic memory parameters
        param->direct_memory_garlic == nullptr ||
        param->direct_memory_garlic_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_garlic_size != ORBIS_VR_TRACKER_GARLIC_SIZE ||
        // Check onion memory parameters
        param->direct_memory_onion == nullptr ||
        param->direct_memory_onion_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_onion_size != required_onion_size ||
        // Check work memory parameters
        param->work_memory == nullptr ||
        param->work_memory_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->work_memory_size != ORBIS_VR_TRACKER_WORK_SIZE ||
        // Check compute queue parameters
        param->gpu_pipe_id >= AmdGpu::Liverpool::NumComputePipes ||
        param->gpu_queue_id >= AmdGpu::Liverpool::NumQueuesPerPipe ||
        // Check calibration settings
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Real hardware will segfault if any of the supplied mappings aren't long enough,
    // Validate each of them to ensure nothing weird can occur when this library is implemented.
    auto* memory = Core::Memory::Instance();
    Libraries::Kernel::OrbisVirtualQueryInfo info;
    // The memory type for the whole range should be the same here,
    // so the memory should be contained in one VMA.
    VAddr addr_to_check = std::bit_cast<VAddr>(param->direct_memory_garlic);
    s32 result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_garlic_size,
               "Insufficient garlic memory provided");

    g_garlic_memory_pointer = param->direct_memory_garlic;
    g_garlic_size = param->direct_memory_garlic_size;

    addr_to_check = std::bit_cast<VAddr>(param->direct_memory_onion);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_onion_size,
               "Insufficient onion memory provided");

    g_onion_memory_pointer = param->direct_memory_onion;
    g_onion_size = param->direct_memory_onion_size;

    addr_to_check = std::bit_cast<VAddr>(param->work_memory);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->work_memory_size,
               "Insufficient work memory provided");

    g_work_memory_pointer = param->work_memory;
    g_work_size = param->work_memory_size;

    // All initialization checks passed.
    LOG_WARNING(Lib_VrTracker, "PSVR headsets are not supported yet");
    g_library_initialized = true;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice(const OrbisVrTrackerDeviceType device_type,
                                            const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 0);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice2(const OrbisVrTrackerDeviceType device_type,
                                             const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDeviceInternal(const OrbisVrTrackerDeviceType device_type,
                                                    const s32 handle, s32 unk0, s32 unk1) {
    LOG_WARNING(Lib_VrTracker, "(STUBBED) called, device_type = {}, handle = {}",
                static_cast<u32>(device_type), handle);
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN || unk0 > 4) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Ignore handle handle validation for now, since most of that logic isn't really handled.
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        if (g_hmd_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_hmd_handle = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        if (g_pad_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_pad_handle = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        if (g_move_handle != -1) {
            if (!PsvrEmuEnabled() || g_move_handle2 != -1) {
                return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
            }
            g_move_handle2 = handle;
            break;
        }
        g_move_handle = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_gun_handle = handle;
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuProcess(const OrbisVrTrackerCpuProcessParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetPlayAreaWarningInfo(OrbisVrTrackerPlayAreaWarningInfo* info) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (PsvrEmuEnabled() && info != nullptr) {
        const u32 size = info->size;
        std::memset(info, 0, sizeof(*info));
        info->size = size;
        info->is_out_of_play_area = false;
        info->is_distance_data_valid = true;
        info->distance_from_vertical_boundary = 1.0f;
        info->distance_from_horizontal_boundary = 1.0f;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetResult(const OrbisVrTrackerGetResultParam* param,
                                       OrbisVrTrackerResultData* result) {
    if (!PsvrEmuEnabled()) {
        LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
        return ORBIS_OK;
    }
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || result == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    const s32 handle = param->handle;
    LOG_TRACE(Lib_VrTracker, "called, handle = {}, hmd = {}, move = {}/{}, pad = {}", handle,
              g_hmd_handle, g_move_handle, g_move_handle2, g_pad_handle);
    if (handle < 0 || (handle != g_hmd_handle && handle != g_move_handle &&
                       handle != g_move_handle2 && handle != g_pad_handle)) {
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
    }

    PollKeyboard();
    std::memset(result, 0, sizeof(*result));
    result->handle = handle;
    result->connected = 1;
    result->timestamp = Libraries::Kernel::sceKernelGetProcessTime();
    result->device_timestamp = result->timestamp;
    result->status = ORBIS_VR_TRACKER_STATUS_TRACKING;
    result->position_quality = ORBIS_VR_TRACKER_QUALITY_FULL;
    result->orientation_quality = ORBIS_VR_TRACKER_QUALITY_FULL;
    result->camera_orientation_w = 1.0f;
    result->user_frame_number = param->user_frame_number;

    if (handle == g_hmd_handle) {
        auto& hmd = result->hmd_info;
        const EmuPose head = HeadPose();
        WritePose(hmd.device_pose, head);
        WritePose(hmd.head_pose, head);
        WritePose(hmd.left_eye_pose, EyePose(-EyeHalfDistance));
        WritePose(hmd.right_eye_pose, EyePose(EyeHalfDistance));
        hmd.rear_tracking_status = ORBIS_VR_TRACKER_REAR_TRACKING_READY;
        hmd.sensor_read_system_timestamp = result->timestamp;
    } else if (handle == g_pad_handle) {
        WritePose(result->pad_info.device_pose, HandPose(1));
    } else {
        const s32 index = handle == g_move_handle2 ? 1 : 0;
        WritePose(result->move_info.device_pose, HandPose(index));
        if (!HandTracked(index)) {
            result->status = ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
            result->position_quality = ORBIS_VR_TRACKER_QUALITY_NOT_VISIBLE;
            result->orientation_quality = ORBIS_VR_TRACKER_QUALITY_NOT_VISIBLE;
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetTime(u64* time) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (time == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    *time = Libraries::Kernel::sceKernelGetProcessTime();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuSubmit(const OrbisVrTrackerGpuSubmitParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }

    if (PsvrEmuEnabled()) {
        return ORBIS_OK;
    }
    // Impossible to submit valid data here since sceCameraGetFrameData returns an error.
    return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWait(const OrbisVrTrackerGpuWaitParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerGpuWaitParam)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    if (PsvrEmuEnabled()) {
        return ORBIS_OK;
    }
    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWaitAndCpuProcess() {
    LOG_TRACE(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }

    if (PsvrEmuEnabled()) {
        return ORBIS_OK;
    }
    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI
sceVrTrackerNotifyEndOfCpuProcess(const OrbisVrTrackerNotifyEndOfCpuProcessParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRecalibrate(const OrbisVrTrackerRecalibrateParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerRecalibrateParam) ||
        param->device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    OrbisVrTrackerDeviceType device_type = param->device_type;
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        // Seems like the lack of a connected hmd results in this?
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        if (g_pad_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        if (g_move_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    // TODO: handle internal recalibration behaviors.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetAll() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetOrientationRelative(const OrbisVrTrackerDeviceType device_type,
                                                      const s32 handle) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSaveInternalBuffers() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetDurationUntilStatusNotTracking(
    const OrbisVrTrackerDeviceType device_type, const u32 duration_camera_frames) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Seems to unconditionally return 0 when parameters are valid.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetExtendedMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetLEDBrightness() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetRestingMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceVrTrackerUpdateMotionSensorData(const OrbisVrTrackerUpdateMotionSensorDataParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_0FA4C949F8D3024E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_285C6AFC09C42F7E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_9A6CDB2103664F8A() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B4D26B7D8B18DF06() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetDeviceRejection() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_1119B0BE399F37E7() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_4928B43816BC440D() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_863EF32EFCB0FA9C() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_E6E726CBC85C48F9() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_F6407E46C66DF383() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPopMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPushMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetLiveCaptureId() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStartLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStopLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerUnregisterDevice(const s32 handle) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    // Since this function only takes a handle, compare the handle to registered handles.
    if (handle == g_hmd_handle) {
        g_hmd_handle = -1;
    } else if (handle == g_pad_handle) {
        g_pad_handle = -1;
    } else if (handle == g_move_handle) {
        g_move_handle = -1;
    } else if (handle == g_move_handle2) {
        g_move_handle2 = -1;
    } else if (handle == g_gun_handle) {
        g_gun_handle = -1;
    } else {
        // If none of the handles match up, then return an error.
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerTerm() {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    g_library_initialized = false;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("24kDA+A0Ox0", "libSceVrTrackerFourDeviceAllowed", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice2);
    LIB_FUNCTION("5IFOAYv-62g", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerCpuProcess);
    LIB_FUNCTION("zvyKP0Z3UvU", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGetPlayAreaWarningInfo);
    LIB_FUNCTION("76OBvrrQXUc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetResult);
    LIB_FUNCTION("XoeWzXlrnMw", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetTime);
    LIB_FUNCTION("TVegDMLaBB8", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuSubmit);
    LIB_FUNCTION("gkGuO9dd57M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuWait);
    LIB_FUNCTION("ARhgpXvwoR0", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGpuWaitAndCpuProcess);
    LIB_FUNCTION("QkRl7pART9M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerInit);
    LIB_FUNCTION("VItTwN8DmS8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerNotifyEndOfCpuProcess);
    LIB_FUNCTION("K7yhYrsIBPc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerQueryMemory);
    LIB_FUNCTION("EUCaQtXXYNI", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerRecalibrate);
    LIB_FUNCTION("sIh8GwcevaQ", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice);
    LIB_FUNCTION("ufexf4aNiwg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDeviceInternal);
    LIB_FUNCTION("CtWUbFgmq+I", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerResetAll);
    LIB_FUNCTION("E0P0sN-wy+4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerResetOrientationRelative);
    LIB_FUNCTION("bDGZVTwwZ1A", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSaveInternalBuffers);
    LIB_FUNCTION("qBjnR0HtMYI", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetDurationUntilStatusNotTracking);
    LIB_FUNCTION("NhPkY3V8E+8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetExtendedMode);
    LIB_FUNCTION("vpsLLotiSUg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetLEDBrightness);
    LIB_FUNCTION("lgWSHQ8p4i4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetRestingMode);
    LIB_FUNCTION("IBv4P3q1pQ0", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerTerm);
    LIB_FUNCTION("Q8skQqEwn5c", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUnregisterDevice);
    LIB_FUNCTION("9fvHMUbsom4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUpdateMotionSensorData);
    LIB_FUNCTION("D6TJSfjTAk4", "libSceVrTracker", 1, "libSceVrTracker", Func_0FA4C949F8D3024E);
    LIB_FUNCTION("KFxq-AnEL34", "libSceVrTracker", 1, "libSceVrTracker", Func_285C6AFC09C42F7E);
    LIB_FUNCTION("mmzbIQNmT4o", "libSceVrTracker", 1, "libSceVrTracker", Func_9A6CDB2103664F8A);
    LIB_FUNCTION("tNJrfYsY3wY", "libSceVrTracker", 1, "libSceVrTracker", Func_B4D26B7D8B18DF06);
    LIB_FUNCTION("jGqEkPy0iLU", "libSceVrTrackerDeviceRejection", 1, "libSceVrTracker",
                 sceVrTrackerSetDeviceRejection);
    LIB_FUNCTION("ERmwvjmfN+c", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_1119B0BE399F37E7);
    LIB_FUNCTION("SSi0OBa8RA0", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_4928B43816BC440D);
    LIB_FUNCTION("hj7zLvyw+pw", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_863EF32EFCB0FA9C);
    LIB_FUNCTION("5ucmy8hcSPk", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_E6E726CBC85C48F9);
    LIB_FUNCTION("9kB+RsZt84M", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_F6407E46C66DF383);
    LIB_FUNCTION("sBkAqyF5Gns", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPopMarker);
    LIB_FUNCTION("rvCywCbc7Pk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPushMarker);
    LIB_FUNCTION("lm6T1Ur6JRk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerGetLiveCaptureId);
    LIB_FUNCTION("qa1+CeXKDPc", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStartLiveCapture);
    LIB_FUNCTION("3YCwwpHkHIg", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStopLiveCapture);
};

} // namespace Libraries::VrTracker