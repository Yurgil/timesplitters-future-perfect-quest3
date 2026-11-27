#include "xr.h"
#include "recomp_pad.h"
#include "rhi.h"

#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <recompvr/android.h>
#include <recompvr/openxr.h>
#include <recompvr/vulkan.h>
#include "projection_probe.h"
#include "sniper_scope.h"
#include "stereo_backend.h"
#include "stereo_math.h"
#include "stereo_probe.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { LEFT = TSFP_AVATAR_LEFT, RIGHT = TSFP_AVATAR_RIGHT, HANDS = TSFP_AVATAR_HANDS };
enum { STICK, TRIGGER, GRIP, CLICK, A, B, X, Y, MENU, GRIP_POSE, AIM_POSE, ACTIONS };
enum { TOUCH_GRIP, TOUCH_AIM, TOUCH_POSES };
static const int pose_actions[TOUCH_POSES] = { GRIP_POSE, AIM_POSE };

static int enabled;
static int projection_requested;
static int geometry_requested, geometry_disabled;
static unsigned msaa_samples = 1, anisotropic_filtering = 1;
static atomic_int snap_degrees;
static atomic_int pending_snap;
static atomic_uint_fast64_t scene_thread;
#ifndef TSFP_RELEASE
static const char *capture_prefix;
static char capture_request[4096], on_demand_prefix[4096];
static char avatar_capture_request[4096];
static char shield_capture_request[4096];
static int shield_capture;
#endif
static char hand_calibration_request[4096], hand_calibration_file[4096];
static char legacy_hand_calibration_file[4096];
static atomic_int pending_hand_calibration;
static struct {
    int running, acquired, frame_open, positioned, eyes_valid, early_frame;
    int world_scene, world_active, reuse_world, stereo_committed, projection_restart;
    float world_near, world_far;
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSessionState state;
    XrSpace local, view;
    XrSwapchain swapchain;
    XrSwapchainImageVulkan2KHR *images;
    XrFrameState frame;
    XrCompositionLayerQuad screen;
    XrPosef anchor;
    VkExtent2D geometry_extent;
    XrView eyes[TSFP_XR_EYES];
    XrVector4f object_planes[TSFP_XR_EYES + 1][TSFP_FRUSTUM_PLANES];
    unsigned object_views;
    uint64_t pose_ticks;
    XrPath hands[HANDS];
    XrSpace touch_space[TOUCH_POSES][HANDS];
    XrPosef touch_pose[TOUCH_POSES][HANDS];
    unsigned touch_valid;
    TsfpRightStickState right_stick;
    struct {
        uint32_t weapon, previous_buttons;
        unsigned zoom;
        int active;
    } scope;
    int last_snap_degrees;
    XrActionSet controls;
    XrAction actions[ACTIONS];
    PFN_xrCreateVulkanDeviceKHR create_device;
    uint32_t frames, projection_frames, early_frames;
} xr;

static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static RecompPadState pad;
static int16_t cart_turn_axis;

#ifndef TSFP_RELEASE
static struct {
    uint32_t frame, missed, waited_for_fresh;
    int source_ready, copy_recorded, early_acquire;
    XrDuration wait_budget;
} stereo_diagnostic;

/* Written by the renderer thread. The intervals include the guest work
 * between presents, while the wait counters cover the named host calls. */
static struct {
    uint64_t previous_wall, previous_cpu, xr_wait, gpu_wait;
    uint64_t wall_sum, wall_max, cpu_sum, xr_sum, gpu_sum;
    unsigned frames;
} frame_timing;

void tsfp_xr_add_gpu_wait(uint64_t elapsed)
{
    frame_timing.gpu_wait += elapsed;
}

void tsfp_xr_report_frame_timing(void)
{
    if (!xr.world_active || xr.state != XR_SESSION_STATE_FOCUSED) {
        memset(&frame_timing, 0, sizeof frame_timing);
        return;
    }
    struct timespec thread_time;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread_time) != 0)
        return;
    const uint64_t wall = SDL_GetTicksNS();
    const uint64_t cpu = (uint64_t)thread_time.tv_sec * UINT64_C(1000000000) + thread_time.tv_nsec;
    if (frame_timing.previous_wall && cpu >= frame_timing.previous_cpu) {
        const uint64_t interval = wall - frame_timing.previous_wall;
        frame_timing.wall_sum += interval;
        if (interval > frame_timing.wall_max)
            frame_timing.wall_max = interval;
        frame_timing.cpu_sum += cpu - frame_timing.previous_cpu;
        frame_timing.xr_sum += frame_timing.xr_wait;
        frame_timing.gpu_sum += frame_timing.gpu_wait;
        if (++frame_timing.frames == 120) {
            const double to_average_ms = 1.0e-6 / frame_timing.frames;
            fprintf(stderr, "[XR-TIME] %u frames: %.2f ms interval (worst %.2f), "
                    "%.2f ms game-thread CPU, %.2f ms xrWaitFrame, %.2f ms Vulkan semaphore\n",
                    frame_timing.frames, frame_timing.wall_sum * to_average_ms, frame_timing.wall_max / 1.0e6,
                    frame_timing.cpu_sum * to_average_ms, frame_timing.xr_sum * to_average_ms,
                    frame_timing.gpu_sum * to_average_ms);
            frame_timing.frames = 0;
            frame_timing.wall_sum = frame_timing.wall_max = frame_timing.cpu_sum = 0;
            frame_timing.xr_sum = frame_timing.gpu_sum = 0;
        }
    }
    frame_timing.previous_wall = wall;
    frame_timing.previous_cpu = cpu;
    frame_timing.xr_wait = frame_timing.gpu_wait = 0;
}
#else
void tsfp_xr_add_gpu_wait(uint64_t elapsed) { (void)elapsed; }
void tsfp_xr_report_frame_timing(void) {}
#endif

static int ok(XrResult result, const char *operation)
{
    if (XR_SUCCEEDED(result))
        return 1;
    fprintf(stderr, "[XR] %s failed (%d)\n", operation, (int)result);
    return 0;
}

#define CHECK(call) ok((call), #call)
#define PROC(instance, name, pointer) \
    CHECK(xrGetInstanceProcAddr((instance), #name, (PFN_xrVoidFunction *)&(pointer)))

void tsfp_xr_enable(const char *prefix, const char *probe, const char *cache,
                    const char *hand_calibration,
                    unsigned msaa, unsigned anisotropy)
{
    enabled = 1;
#ifndef TSFP_RELEASE
    capture_prefix = prefix;
    if (snprintf(capture_request, sizeof capture_request, "%s/frame-xr-eyes.request", cache) >=
        (int)sizeof capture_request)
        capture_request[0] = '\0';
    if (snprintf(avatar_capture_request, sizeof avatar_capture_request, "%s/frame-xr-avatar.request", cache) >=
        (int)sizeof avatar_capture_request)
        avatar_capture_request[0] = '\0';
    if (snprintf(shield_capture_request, sizeof shield_capture_request, "%s/frame-xr-shield.request", cache) >=
        (int)sizeof shield_capture_request)
        shield_capture_request[0] = '\0';
    snprintf(on_demand_prefix, sizeof on_demand_prefix, "%s/frame", cache);
    if (snprintf(hand_calibration_request, sizeof hand_calibration_request, "%s/hands-calibrate.request", cache) >=
        (int)sizeof hand_calibration_request)
        hand_calibration_request[0] = '\0';
#else
    (void)prefix;
#endif
    if (snprintf(hand_calibration_file, sizeof hand_calibration_file, "%s", hand_calibration) >=
        (int)sizeof hand_calibration_file)
        hand_calibration_file[0] = '\0';
    if (snprintf(legacy_hand_calibration_file, sizeof legacy_hand_calibration_file,
                 "%s/hand-calibration.txt", cache) >= (int)sizeof legacy_hand_calibration_file)
        legacy_hand_calibration_file[0] = '\0';
    projection_requested = strcmp(probe, "projection_probe") == 0;
    geometry_requested = strcmp(probe, "stereo_geometry") == 0;
    geometry_disabled = 0;
    msaa_samples = msaa;
    anisotropic_filtering = anisotropy;
    atomic_store(&pending_snap, 0);
    tsfp_stereo_probe_enable(projection_requested || geometry_requested || strcmp(probe, "stereo_probe") == 0);
    if (*probe)
        fprintf(stderr, "[M4] Diagnostic mode %s\n", probe);
}
int tsfp_xr_enabled(void) { return enabled; }
void tsfp_xr_set_snap_degrees(int degrees)
{
    atomic_store(&snap_degrees, degrees);
    atomic_store(&pending_snap, 0);
}
int tsfp_xr_geometry_enabled(void) { return geometry_requested; }
unsigned tsfp_xr_msaa(void) { return msaa_samples; }
unsigned tsfp_xr_anisotropy(void) { return anisotropic_filtering; }
VkExtent2D tsfp_xr_geometry_extent(void) { return xr.geometry_extent; }
int tsfp_xr_snap_turn_pending(void)
{
    return atomic_load_explicit(&pending_snap, memory_order_relaxed) != 0;
}

int tsfp_xr_take_snap_turn(void)
{
    const int turn = atomic_exchange(&pending_snap, 0);
    return abs(turn) == atomic_load(&snap_degrees) ? turn : 0;
}

int tsfp_xr_take_hand_calibration(void)
{
    static unsigned calls;
    if (atomic_exchange_explicit(&pending_hand_calibration, 0, memory_order_relaxed))
        return 1;
    if (++calls % 36u || !*hand_calibration_request || access(hand_calibration_request, F_OK) != 0)
        return 0;
    unlink(hand_calibration_request);
    return 1;
}

const char *tsfp_xr_hand_calibration_file(void)
{
    return hand_calibration_file;
}

const char *tsfp_xr_legacy_hand_calibration_file(void)
{
    return legacy_hand_calibration_file;
}

/* Holding both thumbsticks down for two seconds requests hand calibration. */
static void watch_hand_calibration(int held)
{
    static XrTime since;
    static int fired;
    if (!held) {
        since = 0;
        fired = 0;
        return;
    }
    if (!since)
        since = xr.frame.predictedDisplayTime;
    if (!fired && xr.frame.predictedDisplayTime - since >= 2000000000) {
        fired = 1;
        atomic_store_explicit(&pending_hand_calibration, 1, memory_order_relaxed);
    }
}

static void log_touch_poses(void)
{
    for (int kind = 0; kind < TOUCH_POSES; ++kind)
        for (int hand = 0; hand < HANDS; ++hand) {
            if (!(xr.touch_valid & (1u << (kind * HANDS + hand))))
                continue;
            const XrPosef *p = &xr.touch_pose[kind][hand];
            fprintf(stderr, "[6DoF] %s %s position %.3f %.3f %.3f orientation %.3f %.3f %.3f %.3f\n",
                    hand == LEFT ? "left" : "right", kind == TOUCH_GRIP ? "grip" : "aim",
                    p->position.x, p->position.y, p->position.z,
                    p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w);
        }
}

#ifndef TSFP_RELEASE
/* One-eye snapshots from the same frame isolate the avatar pass from the
 * remainder of the scene. Mode 'd' ignores depth/stencil for this pass only. */
static int capture_eye(RhiImage *world, const char *name, const char *stage)
{
    RhiImageDesc desc;
    rhi_image_get_desc(world, &desc);
    if (desc.format != RHI_FORMAT_R8G8B8A8_UNORM || desc.mip_levels != 1)
        return 0;
    char file[4096];
    if (snprintf(file, sizeof file, "%s-xr-%s-%s.bmp", on_demand_prefix, name, stage) >= (int)sizeof file)
        return 0;
    SDL_Surface *surface = SDL_CreateSurface((int)desc.width, (int)desc.height, SDL_PIXELFORMAT_RGBA32);
    if (!surface)
        return 0;
    const int saved = rhi_image_readback(world, 0, surface->pixels, (uint32_t)surface->pitch) == 0 &&
        SDL_SaveBMP(surface, file);
    SDL_DestroySurface(surface);
    return saved;
}

int tsfp_xr_avatar_capture_begin(RhiImage *world)
{
    static unsigned calls;
    if (!world || !*avatar_capture_request || ++calls % 20)
        return 0;
    FILE *request = fopen(avatar_capture_request, "r");
    if (!request)
        return 0;
    const int mode = fgetc(request) == 'd' ? 'd' : 'n';
    fclose(request);
    if (capture_eye(world, "avatar", "before"))
        return mode;
    unlink(avatar_capture_request);
    return 0;
}

void tsfp_xr_avatar_capture_end(RhiImage *world, int mode)
{
    if (world && capture_eye(world, "avatar", "after"))
        fprintf(stderr, "[6DoF] Captured avatar pass before/after, depth %s\n", mode == 'd' ? "off" : "normal");
    unlink(avatar_capture_request);
}

void tsfp_xr_capture_frame(RhiImage *world)
{
    if (shield_capture)
        tsfp_xr_shield_capture(world, 3);
    static unsigned frames;
    if (++frames % 60)
        return;
    const int requested = *capture_request && access(capture_request, F_OK) == 0;
    const char *prefix = requested ? on_demand_prefix : capture_prefix;
    if (!prefix || !*prefix)
        return;

    uint32_t width, height;
    if (world) {
        RhiImageDesc desc;
        rhi_image_get_desc(world, &desc);
        if (desc.format != RHI_FORMAT_R8G8B8A8_UNORM || desc.mip_levels != 1)
            return;
        width = desc.width;
        height = desc.height;
    } else if (rhi_swapchain_readback(NULL, 0, &width, &height) != 0) {
        return;
    }
    SDL_Surface *surface = SDL_CreateSurface((int)(world ? 2 * width : width), (int)height,
                                             SDL_PIXELFORMAT_RGBA32);
    if (!surface)
        return;
    int captured;
    if (world) {
        uint8_t *right_eye = (uint8_t *)surface->pixels + width * 4u;
        captured = rhi_image_readback(world, 0, surface->pixels, (uint32_t)surface->pitch) == 0 &&
                   rhi_image_readback(world, 1, right_eye, (uint32_t)surface->pitch) == 0;
    } else {
        captured = rhi_swapchain_readback(surface->pixels, (uint32_t)surface->pitch, &width, &height) == 0;
    }
    if (captured && world) {
        SDL_Surface *rgb = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
        SDL_DestroySurface(surface);
        surface = rgb;
        captured = surface != NULL;
    }
    if (captured) {
        const char *suffix = world ? "xr-eyes" : "xr";
        char file[4096], temp[4096];
        if (snprintf(file, sizeof file, "%s-%s.bmp", prefix, suffix) < (int)sizeof file &&
            snprintf(temp, sizeof temp, "%s-%s.tmp", prefix, suffix) < (int)sizeof temp &&
            SDL_SaveBMP(surface, temp) && rename(temp, file) == 0 && requested) {
            unlink(capture_request);
            fprintf(stderr, "[M4] Captured %ux%u %s\n", width, height, world ? "per eye" : "game quad");
            log_touch_poses();
        }
    }
    SDL_DestroySurface(surface);
}

void tsfp_xr_shield_capture(RhiImage *world, unsigned stage)
{
    static const char *stages[] = {"world", "before-weapon", "after-weapon", "final"};
    if (stage == 0)
        shield_capture = world && *shield_capture_request &&
            tsfp_stereo_sniper_shield_active() && access(shield_capture_request, F_OK) == 0;
    if (!shield_capture || stage >= sizeof stages / sizeof stages[0])
        return;
    if (world)
        capture_eye(world, "shield", stages[stage]);
    if (stage == 3) {
        unlink(shield_capture_request);
        shield_capture = 0;
        fprintf(stderr, "[6DoF] Captured the Future Sniper shield across render passes\n");
    }
}
#else
int tsfp_xr_avatar_capture_begin(RhiImage *world) { (void)world; return 0; }
void tsfp_xr_avatar_capture_end(RhiImage *world, int mode) { (void)world; (void)mode; }
void tsfp_xr_capture_frame(RhiImage *world) { (void)world; }
void tsfp_xr_shield_capture(RhiImage *world, unsigned stage) { (void)world; (void)stage; }
#endif

int tsfp_xr_create_instance(PFN_vkGetInstanceProcAddr get_proc, const VkInstanceCreateInfo *info,
                            VkInstance *instance)
{
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jobject activity = SDL_GetAndroidActivity();
    JavaVM *vm = NULL;
    if (!env || !activity || (*env)->GetJavaVM(env, &vm) != JNI_OK) {
        fprintf(stderr, "[XR] Android activity or VM unavailable\n");
        if (env && activity)
            (*env)->DeleteLocalRef(env, activity);
        return 0;
    }
    XrInstanceCreateInfoAndroidKHR android = { .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
        .applicationVM = vm, .applicationActivity = activity };
    const char *extensions[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                 XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo create = { .type = XR_TYPE_INSTANCE_CREATE_INFO, .next = &android,
        .applicationInfo = { .applicationName = "TimeSplitters Quest",
            .engineName = "xboxrecomp", .apiVersion = XR_API_VERSION_1_0 },
        .enabledExtensionCount = 2, .enabledExtensionNames = extensions };
    int created = CHECK(recompvr_initialize_android_loader(vm, activity)) &&
        CHECK(xrCreateInstance(&create, &xr.instance));
    (*env)->DeleteLocalRef(env, activity);
    if (!created)
        return 0;

    XrSystemGetInfo system = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements;
    PFN_xrCreateVulkanInstanceKHR create_vulkan;
    XrGraphicsRequirementsVulkan2KHR requirements = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    if (!CHECK(xrGetSystem(xr.instance, &system, &xr.system)) ||
        !PROC(xr.instance, xrGetVulkanGraphicsRequirements2KHR, get_requirements) ||
        !CHECK(get_requirements(xr.instance, xr.system, &requirements)) ||
        !PROC(xr.instance, xrCreateVulkanInstanceKHR, create_vulkan) ||
        !PROC(xr.instance, xrCreateVulkanDeviceKHR, xr.create_device))
        return 0;
    fprintf(stderr, "[XR] Vulkan requirements %u.%u to %u.%u\n",
            XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
            XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported));
    XrVulkanInstanceCreateInfoKHR vulkan = { .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId = xr.system, .pfnGetInstanceProcAddr = get_proc, .vulkanCreateInfo = info };
    VkResult result;
    if (!CHECK(create_vulkan(xr.instance, &vulkan, instance, &result)))
        return 0;
    if (result != VK_SUCCESS)
        fprintf(stderr, "[XR] Vulkan instance failed (%d)\n", (int)result);
    return result == VK_SUCCESS;
}

int tsfp_xr_physical_device(VkInstance instance, VkPhysicalDevice *physical)
{
    return CHECK(recompvr_vulkan_graphics_device(xr.instance, xr.system, instance, physical));
}

int tsfp_xr_create_device(PFN_vkGetInstanceProcAddr get_proc, VkPhysicalDevice physical,
                          const VkDeviceCreateInfo *info, VkDevice *device)
{
    XrVulkanDeviceCreateInfoKHR vulkan = { .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
        .systemId = xr.system, .pfnGetInstanceProcAddr = get_proc,
        .vulkanPhysicalDevice = physical, .vulkanCreateInfo = info };
    VkResult result;
    if (!CHECK(xr.create_device(xr.instance, &vulkan, device, &result)))
        return 0;
    if (result != VK_SUCCESS)
        fprintf(stderr, "[XR] Vulkan device failed (%d)\n", (int)result);
    return result == VK_SUCCESS;
}

static int create_actions(void)
{
    static const struct { const char *name, *label; XrActionType type; } definitions[ACTIONS] = {
        [STICK] = { "stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT },
        [TRIGGER] = { "trigger", "Index trigger", XR_ACTION_TYPE_FLOAT_INPUT },
        [GRIP] = { "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT },
        [CLICK] = { "stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT },
        [A] = { "confirm", "A", XR_ACTION_TYPE_BOOLEAN_INPUT },
        [B] = { "cancel", "B", XR_ACTION_TYPE_BOOLEAN_INPUT },
        [X] = { "x_button", "X", XR_ACTION_TYPE_BOOLEAN_INPUT },
        [Y] = { "y_button", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT },
        [MENU] = { "pause", "Start or pause", XR_ACTION_TYPE_BOOLEAN_INPUT },
        [GRIP_POSE] = { "hand_pose", "Hand grip pose", XR_ACTION_TYPE_POSE_INPUT },
        [AIM_POSE] = { "weapon_aim", "Weapon aim pose", XR_ACTION_TYPE_POSE_INPUT },
    };
    static const char *const hand_paths[HANDS] = { "/user/hand/left", "/user/hand/right" };
    XrActionSetCreateInfo set = { .type = XR_TYPE_ACTION_SET_CREATE_INFO,
        .actionSetName = "gamepad", .localizedActionSetName = "Gamepad" };
    if (!CHECK(xrCreateActionSet(xr.instance, &set, &xr.controls)))
        return 0;
    for (int hand = LEFT; hand < HANDS; ++hand)
        if (!CHECK(xrStringToPath(xr.instance, hand_paths[hand], &xr.hands[hand])))
            return 0;
    for (int action = 0; action < ACTIONS; ++action) {
        XrActionCreateInfo info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = definitions[action].type,
            .countSubactionPaths = HANDS, .subactionPaths = xr.hands };
        snprintf(info.actionName, sizeof info.actionName, "%s", definitions[action].name);
        snprintf(info.localizedActionName, sizeof info.localizedActionName, "%s", definitions[action].label);
        if (!CHECK(xrCreateAction(xr.controls, &info, &xr.actions[action])))
            return 0;
    }
    static const struct { int hand, action; const char *suffix; } bindings[] = {
        { LEFT, STICK, "thumbstick" }, { RIGHT, STICK, "thumbstick" },
        { LEFT, CLICK, "thumbstick/click" }, { RIGHT, CLICK, "thumbstick/click" },
        { LEFT, TRIGGER, "trigger/value" }, { RIGHT, TRIGGER, "trigger/value" },
        { LEFT, GRIP, "squeeze/value" }, { RIGHT, GRIP, "squeeze/value" },
        { RIGHT, A, "a/click" }, { RIGHT, B, "b/click" },
        { LEFT, X, "x/click" }, { LEFT, Y, "y/click" }, { LEFT, MENU, "menu/click" },
        { LEFT, GRIP_POSE, "grip/pose" }, { RIGHT, GRIP_POSE, "grip/pose" },
        { LEFT, AIM_POSE, "aim/pose" }, { RIGHT, AIM_POSE, "aim/pose" },
    };
    XrActionSuggestedBinding suggested[sizeof bindings / sizeof bindings[0]];
    for (unsigned i = 0; i < sizeof bindings / sizeof bindings[0]; ++i) {
        char path[XR_MAX_PATH_LENGTH];
        snprintf(path, sizeof path, "%s/input/%s", hand_paths[bindings[i].hand], bindings[i].suffix);
        suggested[i].action = xr.actions[bindings[i].action];
        if (!CHECK(xrStringToPath(xr.instance, path, &suggested[i].binding)))
            return 0;
    }
    XrInteractionProfileSuggestedBinding profile = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .countSuggestedBindings = sizeof suggested / sizeof suggested[0], .suggestedBindings = suggested };
    XrSessionActionSetsAttachInfo attach = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1, .actionSets = &xr.controls };
    if (!CHECK(xrStringToPath(xr.instance, "/interaction_profiles/oculus/touch_controller",
                             &profile.interactionProfile)) ||
        !CHECK(xrSuggestInteractionProfileBindings(xr.instance, &profile)) ||
        !CHECK(xrAttachSessionActionSets(xr.session, &attach)))
        return 0;
    if (geometry_requested)
        for (int kind = 0; kind < TOUCH_POSES; ++kind)
            for (int hand = 0; hand < HANDS; ++hand) {
                const XrActionSpaceCreateInfo space = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
                    .action = xr.actions[pose_actions[kind]], .subactionPath = xr.hands[hand],
                    .poseInActionSpace.orientation.w = 1 };
                CHECK(xrCreateActionSpace(xr.session, &space, &xr.touch_space[kind][hand]));
            }
    return 1;
}

int tsfp_xr_create_screen(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          uint32_t family, uint32_t width, uint32_t height, VkFormat *format)
{
    XrGraphicsBindingVulkan2KHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = instance, .physicalDevice = physical, .device = device, .queueFamilyIndex = family };
    XrSessionCreateInfo create = { .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = xr.system };
    if (!CHECK(xrCreateSession(xr.instance, &create, &xr.session)) || !create_actions() ||
        !CHECK(recompvr_create_reference_space(xr.session, XR_REFERENCE_SPACE_TYPE_LOCAL, &xr.local)) ||
        !CHECK(recompvr_create_reference_space(xr.session, XR_REFERENCE_SPACE_TYPE_VIEW, &xr.view)))
        return 0;

    uint32_t count;
    if (!CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &count, NULL)) || !count)
        return 0;
    int64_t *formats = calloc(count, sizeof *formats);
    if (!formats)
        return 0;
    const VkFormat candidates[] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    *format = VK_FORMAT_UNDEFINED;
    if (CHECK(xrEnumerateSwapchainFormats(xr.session, count, &count, formats)))
        for (unsigned i = 0; i < sizeof candidates / sizeof candidates[0] && !*format; ++i)
            for (uint32_t j = 0; j < count; ++j)
                if (formats[j] == candidates[i])
                    *format = candidates[i];
    free(formats);
    if (!*format) {
        fprintf(stderr, "[XR] No compatible RGBA swapchain format\n");
        return 0;
    }
    XrSwapchainCreateInfo swap = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO, .format = *format,
        .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .sampleCount = 1, .width = width, .height = height, .faceCount = 1, .arraySize = 1, .mipCount = 1 };
    if (!CHECK(xrCreateSwapchain(xr.session, &swap, &xr.swapchain)) ||
        !CHECK(xrEnumerateSwapchainImages(xr.swapchain, 0, &count, NULL)) || !count)
        return 0;
    xr.images = calloc(count, sizeof *xr.images);
    if (!xr.images)
        return 0;
    for (uint32_t i = 0; i < count; ++i)
        xr.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    if (!CHECK(xrEnumerateSwapchainImages(xr.swapchain, count, &count, (XrSwapchainImageBaseHeader *)xr.images)))
        return 0;
    xr.screen = (XrCompositionLayerQuad){ .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = xr.local,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = { .swapchain = xr.swapchain, .imageRect.extent = { width, height } },
        .pose = { .orientation.w = 1, .position.z = -2.0f },
        .size = { 2.0f, 2.0f * height / width } };
    fprintf(stderr, "[XR] Quad %ux%u, %u swapchain images, Vulkan format %d\n", width, height, count, *format);
    if (projection_requested || geometry_requested) {
        XrSystemProperties properties = { .type = XR_TYPE_SYSTEM_PROPERTIES };
        const int supported = CHECK(xrGetSystemProperties(xr.instance, xr.system, &properties)) &&
            properties.graphicsProperties.maxLayerCount >= (geometry_requested ? 1u : 2u);
        int available = 0;
        if (supported && geometry_requested) {
            const VkExtent2D sizes[] = { {1920, 1080}, {1280, 720}, {width, height} };
            for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
                const VkExtent2D extent = sizes[i];
                if (extent.width <= properties.graphicsProperties.maxSwapchainImageWidth &&
                    extent.height <= properties.graphicsProperties.maxSwapchainImageHeight &&
                    tsfp_projection_create(xr.session, device, *format, extent.width, extent.height)) {
                    xr.geometry_extent = extent;
                    available = 1;
                    break;
                }
            }
        } else if (supported) {
            available = tsfp_projection_create(xr.session, device, *format, 0, 0);
        }
        projection_requested &= available;
        geometry_requested &= available;
        if (!available)
            fprintf(stderr, "[M4] Projection probe unavailable, using the game quad\n");
    }
    atomic_store_explicit(&scene_thread, SDL_GetCurrentThreadID(), memory_order_relaxed);
    return 1;
}

static void publish_pad(RecompPadState state, int cart_rider)
{
    pthread_mutex_lock(&input_lock);
    /* The guest also turns the cart on RX. Send the axis only to the
     * VR cart camera so the guest cannot feed its yaw back into the turn. */
    cart_turn_axis = cart_rider ? state.rx : 0;
    if (cart_rider)
        state.rx = 0;
    pad = state;
    pthread_mutex_unlock(&input_lock);
}

int tsfp_xr_read_pad(int slot, RecompPadState *out)
{
    if (!enabled || slot != 0)
        return 0;
    pthread_mutex_lock(&input_lock);
    *out = pad;
    pthread_mutex_unlock(&input_lock);
    return 1;
}

int16_t tsfp_xr_cart_turn_axis(void)
{
    pthread_mutex_lock(&input_lock);
    const int16_t axis = cart_turn_axis;
    pthread_mutex_unlock(&input_lock);
    return axis;
}

static XrActionStateGetInfo action_info(int action, int hand)
{
    return (XrActionStateGetInfo){ .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = xr.actions[action], .subactionPath = xr.hands[hand] };
}

static int button(int action, int hand)
{
    XrActionStateGetInfo info = action_info(action, hand);
    return recompvr_read_button(xr.session, &info);
}

static float analog(int action, int hand)
{
    XrActionStateGetInfo info = action_info(action, hand);
    return fminf(1.0f, fmaxf(0.0f, recompvr_read_float(xr.session, &info)));
}

static XrVector2f stick(int hand)
{
    XrActionStateGetInfo info = action_info(STICK, hand);
    return recompvr_read_stick(xr.session, &info);
}

static int16_t axis(float value)
{
    const float dead_zone = 0.12f;
    if (fabsf(value) <= dead_zone)
        return 0;
    float magnitude = (fminf(1.0f, fabsf(value)) - dead_zone) / (1.0f - dead_zone);
    return (int16_t)(copysignf(magnitude, value) * 32767.0f);
}

static XrQuaternionf head_orientation(void)
{
    XrQuaternionf orientation;
    XrQuaternionf_Lerp(&orientation, &xr.eyes[LEFT].pose.orientation,
                       &xr.eyes[RIGHT].pose.orientation, 0.5f);
    return orientation;
}

static XrVector3f head_position(void)
{
    const XrVector3f left = xr.eyes[LEFT].pose.position, right = xr.eyes[RIGHT].pose.position;
    return (XrVector3f){ (left.x + right.x) * 0.5f, (left.y + right.y) * 0.5f,
                        (left.z + right.z) * 0.5f };
}

static void locate_touch_poses(void)
{
    /* The runtime can supply a valid estimated pose during brief occlusion. */
    const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    for (int kind = 0; kind < TOUCH_POSES; ++kind)
        for (int hand = 0; hand < HANDS; ++hand) {
            if (!xr.touch_space[kind][hand])
                continue;
            const XrActionStateGetInfo info = action_info(pose_actions[kind], hand);
            if (recompvr_locate_action(xr.session, &info, xr.touch_space[kind][hand], xr.local,
                                      xr.frame.predictedDisplayTime, valid, &xr.touch_pose[kind][hand]))
                xr.touch_valid |= 1u << (kind * HANDS + hand);
        }
    static unsigned reported;
    if (xr.touch_valid & ~reported) {
        reported |= xr.touch_valid;
        log_touch_poses();
    }
}

static void reset_vr_controls(void)
{
    xr.right_stick = (TsfpRightStickState){0};
    xr.scope.active = 0;
    atomic_store_explicit(&pending_snap, 0, memory_order_relaxed);
}

static void vr_right_stick(RecompPadState *state, XrVector2f stick, int mounted)
{
    const int degrees = atomic_load(&snap_degrees);
    if (degrees != xr.last_snap_degrees) {
        xr.right_stick.snap_armed = 0;
        xr.last_snap_degrees = degrees;
    }
    const TsfpRightStickInput input = tsfp_right_stick(stick, mounted, degrees, &xr.right_stick);
    state->rx = axis(input.look.x);
    state->ry = axis(input.look.y);
    if (input.weapon)
        state->buttons |= input.weapon > 0 ? RECOMP_PAD_DPAD_RIGHT : RECOMP_PAD_DPAD_LEFT;
    if (mounted || input.snap_degrees)
        atomic_store(&pending_snap, input.snap_degrees);
}

static void sync_scope_controls(RecompPadState *state, int immersive, int calibrating)
{
    const uint32_t pressed = state->buttons & ~xr.scope.previous_buttons;
    xr.scope.previous_buttons = state->buttons;
    unsigned hand;
    const uint32_t weapon = immersive && tsfp_vk_scope_supported() ?
        tsfp_stereo_scope_weapon(&hand) : UINT32_MAX;
    if (weapon != xr.scope.weapon || weapon == UINT32_MAX || !tsfp_xr_touch_tracked(hand))
        xr.scope.active = 0;
    xr.scope.weapon = weapon;
    if (weapon == UINT32_MAX)
        return;

    /* The default Xbox aim button normally zooms the entire game camera. */
    state->buttons &= ~RECOMP_PAD_RIGHT_THUMB;
    if (!calibrating && (pressed & RECOMP_PAD_RIGHT_THUMB) && tsfp_xr_touch_tracked(hand)) {
        xr.scope.active = !xr.scope.active;
        xr.scope.zoom = 0;
    }
    if (xr.scope.active) {
        if ((pressed & RECOMP_PAD_DPAD_UP) && xr.scope.zoom + 1 < TSFP_SCOPE_ZOOM_STEPS)
            ++xr.scope.zoom;
        if ((pressed & RECOMP_PAD_DPAD_DOWN) && xr.scope.zoom)
            --xr.scope.zoom;
        state->buttons &= ~(RECOMP_PAD_DPAD_UP | RECOMP_PAD_DPAD_DOWN);
    }
}

static void sync_controls(int immersive)
{
    RecompPadState state = {0};
    XrActiveActionSet set = { xr.controls, XR_NULL_PATH };
    XrActionsSyncInfo sync = { .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &set };
    xr.touch_valid = 0;
    if (xr.state != XR_SESSION_STATE_FOCUSED || xrSyncActions(xr.session, &sync) != XR_SUCCESS) {
        reset_vr_controls();
        xr.scope.previous_buttons = UINT32_MAX;
        publish_pad(state, 0);
        return;
    }
    XrVector2f left = stick(LEFT), right = stick(RIGHT);
    const TsfpVehicleLook vehicle_look = immersive ? tsfp_stereo_vehicle_look() : TSFP_VEHICLE_LOOK_DEFAULT;
    const int mounted = vehicle_look == TSFP_VEHICLE_LOOK_GUNNER;
    if (immersive) {
        if (xr.eyes_valid) {
            locate_touch_poses();
            if (vehicle_look != TSFP_VEHICLE_LOOK_TRACKED_DRIVER)
                left = tsfp_head_relative_stick(left, head_orientation(), xr.anchor.orientation);
        }
        vr_right_stick(&state, right, vehicle_look != TSFP_VEHICLE_LOOK_DEFAULT);
    } else {
        reset_vr_controls();
        state.rx = axis(right.x);
        state.ry = axis(right.y);
    }
    int left_grip = analog(GRIP, LEFT) > 0.6f;
    int right_grip = analog(GRIP, RIGHT) > 0.6f;
    if (!immersive && left_grip && right_grip) {
        if (left.y > 0.65f) state.buttons |= RECOMP_PAD_DPAD_UP;
        if (left.y < -0.65f) state.buttons |= RECOMP_PAD_DPAD_DOWN;
        if (left.x > 0.65f) state.buttons |= RECOMP_PAD_DPAD_RIGHT;
        if (left.x < -0.65f) state.buttons |= RECOMP_PAD_DPAD_LEFT;
    } else {
        state.lx = axis(left.x);
        state.ly = axis(left.y);
        if (left_grip) state.buttons |= RECOMP_PAD_LEFT_SHOULDER;
        if (right_grip) state.buttons |= RECOMP_PAD_RIGHT_SHOULDER;
    }
    state.left_trigger = (uint8_t)lroundf(analog(TRIGGER, LEFT) * 255.0f);
    state.right_trigger = (uint8_t)lroundf(analog(TRIGGER, RIGHT) * 255.0f);
    if (button(A, RIGHT)) state.buttons |= RECOMP_PAD_A;
    if (button(B, RIGHT)) state.buttons |= immersive ? RECOMP_PAD_X : RECOMP_PAD_B;
    if (button(X, LEFT)) {
        if (!immersive) state.buttons |= RECOMP_PAD_X;
        else if (mounted) state.buttons |= RECOMP_PAD_B;
        else state.buttons |= RECOMP_PAD_DPAD_DOWN; /* Grenade type. */
    }
    if (button(Y, LEFT)) {
        /* The Xbox vehicle update uses Y to request getting out of any seat. */
        state.buttons |= immersive && !mounted && !tsfp_stereo_in_vehicle() ?
            RECOMP_PAD_DPAD_UP : RECOMP_PAD_Y;
    }
    if (button(MENU, LEFT)) state.buttons |= RECOMP_PAD_START;
    if (button(CLICK, LEFT)) state.buttons |= RECOMP_PAD_LEFT_THUMB;
    if (button(CLICK, RIGHT)) state.buttons |= RECOMP_PAD_RIGHT_THUMB;
    const int calibrating = immersive && (state.buttons & RECOMP_PAD_LEFT_THUMB) &&
        (state.buttons & RECOMP_PAD_RIGHT_THUMB);
    watch_hand_calibration(calibrating);
    sync_scope_controls(&state, immersive, calibrating);
    static int logged;
    if (!logged && (state.buttons || state.lx || state.ly || state.rx || state.ry ||
                    state.left_trigger || state.right_trigger)) {
        fprintf(stderr, "[XR] First Touch input delivered to player 1\n");
        logged = 1;
    }
    publish_pad(state, immersive && geometry_requested && tsfp_stereo_cart_riding());
}

static void poll_events(void)
{
    XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *change = (const XrEventDataSessionStateChanged *)&event;
            xr.state = change->state;
            fprintf(stderr, "[XR] Session state %d\n", xr.state);
            if (xr.state == XR_SESSION_STATE_READY) {
                if (xr.projection_restart) {
                    xr.projection_restart = 0;
                    const int restarted = tsfp_projection_restart(xr.session);
#ifndef TSFP_RELEASE
                    fprintf(stderr, "[XR-STEREO] Stereo swapchain restart %s\n",
                            restarted ? "complete" : "failed");
#else
                    (void)restarted;
#endif
                }
                xr.running = CHECK(recompvr_begin_stereo_session(xr.session));
            } else if (xr.state == XR_SESSION_STATE_STOPPING) {
                xr.running = 0;
#ifndef TSFP_RELEASE
                fprintf(stderr, "[XR-STEREO] Cache invalidated before frame %u scene=%d reason=session-stopping\n",
                        stereo_diagnostic.frame + 1u, xr.world_scene);
#endif
                tsfp_projection_forget_geometry();
                if (CHECK(xrEndSession(xr.session)))
                    xr.projection_restart = geometry_requested && !geometry_disabled;
            } else if (xr.state == XR_SESSION_STATE_EXITING || xr.state == XR_SESSION_STATE_LOSS_PENDING) {
                xr.running = 0;
            } else if (xr.state == XR_SESSION_STATE_FOCUSED) {
                xr.positioned = 0;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            for (int hand = LEFT; hand < HANDS; ++hand) {
                XrInteractionProfileState profile = { .type = XR_TYPE_INTERACTION_PROFILE_STATE };
                char path[XR_MAX_PATH_LENGTH];
                uint32_t count;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(xr.session, xr.hands[hand], &profile)) &&
                    profile.interactionProfile &&
                    XR_SUCCEEDED(xrPathToString(xr.instance, profile.interactionProfile, sizeof path, &count, path)))
                    fprintf(stderr, "[XR] %s profile %s\n", hand == LEFT ? "Left" : "Right", path);
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const XrEventDataReferenceSpaceChangePending *change =
                (const XrEventDataReferenceSpaceChangePending *)&event;
            const int applies = change->session == xr.session &&
                change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL;
#ifndef TSFP_RELEASE
            fprintf(stderr, "[XR-STEREO] Reference-space change before frame %u scene=%d type=%d invalidated=%d\n",
                    stereo_diagnostic.frame + 1u, xr.world_scene, change->referenceSpaceType, applies);
#endif
            if (applies) {
                xr.positioned = 0;
                tsfp_projection_forget_geometry();
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            xr.running = 0;
        }
        event = (XrEventDataBuffer){ .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static void position_screen(void)
{
    if (xr.positioned)
        return;
    XrSpaceLocation location = { .type = XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(xr.view, xr.local, xr.frame.predictedDisplayTime, &location)) ||
        (location.locationFlags & valid) != valid)
        return;
    XrQuaternionf q = location.pose.orientation;
    float yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    xr.anchor = (XrPosef){
        .orientation = { .y = sinf(yaw * 0.5f), .w = cosf(yaw * 0.5f) },
        .position = location.pose.position
    };
    xr.screen.pose = xr.anchor;
    xr.screen.pose.position.x -= 2.0f * sinf(yaw);
    xr.screen.pose.position.z -= 2.0f * cosf(yaw);
    xr.positioned = 1;
}

static void update_world_scene(int scene)
{
    if (scene == xr.world_scene)
        return;
    xr.world_scene = scene;
    xr.stereo_committed = 0;
#ifndef TSFP_RELEASE
    stereo_diagnostic.missed = 0;
#endif
    tsfp_projection_forget_geometry();
}

/* Called under frame_lock. The world camera and present share this frame
 * state; only the first one reached can wait for and begin an XR frame. */
static int open_frame(int early)
{
    if (xr.frame_open)
        return 1;
    poll_events();
    if (!xr.running) {
        publish_pad((RecompPadState){0}, 0);
        return 0;
    }
#ifndef TSFP_RELEASE
    const uint64_t wait_started = SDL_GetTicksNS();
#endif
    const XrResult waited = recompvr_wait_frame(xr.session, &xr.frame);
#ifndef TSFP_RELEASE
    frame_timing.xr_wait += SDL_GetTicksNS() - wait_started;
#endif
    if (!CHECK(waited) || !CHECK(recompvr_begin_frame(xr.session)))
        return 0;
    xr.frame_open = 1;
    xr.early_frame = early;
#ifndef TSFP_RELEASE
    ++stereo_diagnostic.frame;
    stereo_diagnostic.source_ready = stereo_diagnostic.copy_recorded = 0;
    stereo_diagnostic.early_acquire = 0;
    stereo_diagnostic.wait_budget = 0;
    tsfp_projection_reset_diagnostic();
#endif
    tsfp_stereo_begin_frame();
    xr.eyes_valid = 0;
    xr.world_active = xr.reuse_world = 0;
    update_world_scene(geometry_requested && !geometry_disabled ? tsfp_stereo_world_scene() : -1);
    if (xr.frame.shouldRender) {
        position_screen();
        xr.eyes_valid = (projection_requested || (geometry_requested && !geometry_disabled)) && xr.positioned &&
            tsfp_xr_locate_stereo(xr.session, xr.local, xr.frame.predictedDisplayTime, xr.eyes);
        if (xr.eyes_valid && geometry_requested && !geometry_disabled) {
            /* Leave samples beyond the optical edges for late reprojection. */
            for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
                XrFovf *fov = &xr.eyes[eye].fov;
                fov->angleLeft = atanf(1.10f * tanf(fov->angleLeft));
                fov->angleRight = atanf(1.10f * tanf(fov->angleRight));
                fov->angleUp = atanf(1.10f * tanf(fov->angleUp));
                fov->angleDown = atanf(1.10f * tanf(fov->angleDown));
            }
        }
        if (xr.eyes_valid)
            xr.pose_ticks = SDL_GetTicksNS();
    }
    float game_projection[16];
    sync_controls(xr.world_scene >= 0 && tsfp_stereo_world_projection(game_projection));
    return 1;
}

void tsfp_xr_prepare_scene_frame(void)
{
    if (SDL_GetCurrentThreadID() != atomic_load_explicit(&scene_thread, memory_order_relaxed))
        return;
    pthread_mutex_lock(&frame_lock);
    if (projection_requested || (geometry_requested && !geometry_disabled))
        open_frame(1);
    pthread_mutex_unlock(&frame_lock);
}

static int eye_rows(float rows[32], const XrMatrix4x4f *game, const XrPosef *anchor,
                    float meters_per_unit, float near_z, float far_z)
{
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        XrMatrix4x4f transform;
        if (!tsfp_xbox_eye_transform_depth(&transform, game, anchor, &xr.eyes[eye],
                                          meters_per_unit, near_z, far_z))
            return 0;
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c)
                rows[eye * 16 + r * 4 + c] = transform.m[c * 4 + r];
    }
    return 1;
}

int tsfp_xr_world_matrices(float rows[32])
{
    if (!geometry_requested || SDL_GetCurrentThreadID() != atomic_load_explicit(&scene_thread, memory_order_relaxed))
        return 0;
    pthread_mutex_lock(&frame_lock);
    XrMatrix4x4f game;
    const int have_camera = !geometry_disabled && tsfp_stereo_world_projection(game.m);
    int valid = have_camera && open_frame(1) && xr.eyes_valid;
    if (have_camera)
        update_world_scene(tsfp_stereo_world_scene());
    static int logged_pose, logged_lens;
    if (have_camera && !valid && !logged_pose) {
        logged_pose = 1;
        fprintf(stderr, "[M4] Game camera ready, waiting for a renderable XR frame with two valid eye poses\n");
    }
    if (valid) {
        valid = tsfp_xbox_camera_planes(&game, &xr.world_near, &xr.world_far);
        if (valid) {
            xr.world_near = fminf(xr.world_near, 0.06f);
            valid = eye_rows(rows, &game, &xr.anchor, 1.0f, xr.world_near, xr.world_far);
        }
        if (!valid && !logged_lens) {
            logged_lens = 1;
            fprintf(stderr, "[M4] Game lens could not be transformed for stereo\n");
        }
    }
    xr.world_active = valid;
    if (valid) {
        /* Reserve the image when the game starts its stereo pass. Wait for
         * write access at present, after the source frame has been rendered. */
        const int acquired = tsfp_projection_acquire();
#ifndef TSFP_RELEASE
        stereo_diagnostic.early_acquire = acquired;
#else
        (void)acquired;
#endif
    }
    pthread_mutex_unlock(&frame_lock);
    return valid;
}

int tsfp_xr_world_active(void)
{
    return xr.world_active;
}

void tsfp_xr_ghost_camera(const float game_view[16], float eyes[TSFP_XR_EYES][TSFP_GHOST_CAMERA_MATRICES][16])
{
    XrMatrix4x4f game;
    memcpy(game.m, game_view, sizeof game.m);
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        XrMatrix4x4f view, projection, view_projection;
        tsfp_xbox_eye_camera(&game, &xr.anchor, &xr.eyes[eye], xr.world_near, xr.world_far,
                            &view, &projection, &view_projection);
        memcpy(eyes[eye][TSFP_GHOST_CAMERA_VIEW], view.m, sizeof view.m);
        memcpy(eyes[eye][TSFP_GHOST_CAMERA_PROJECTION], projection.m, sizeof projection.m);
        memcpy(eyes[eye][TSFP_GHOST_CAMERA_VIEW_PROJECTION], view_projection.m, sizeof view_projection.m);
    }
}

static int scope_view(XrView *view)
{
    TsfpScopeGeometry geometry;
    if (!xr.scope.active || !tsfp_stereo_scope_camera(xr.scope.weapon, &geometry))
        return 0;
    *view = tsfp_scope_view(&geometry, xr.scope.zoom, &xr.anchor);
    return 1;
}

int tsfp_xr_scope_matrices(float rows[16])
{
    XrView view;
    XrMatrix4x4f game, clip;
    if (!xr.world_active || !scope_view(&view) || !tsfp_stereo_world_projection(game.m))
        return 0;
    if (!tsfp_xbox_eye_transform_depth(&clip, &game, &xr.anchor, &view, 1, xr.world_near, xr.world_far))
        return 0;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            rows[row * 4 + col] = clip.m[col * 4 + row];
    return 1;
}

int tsfp_xr_scope_lens(TsfpScopeGeometry *geometry, float rows[32])
{
    XrVector3f body_offset;
    if (!xr.world_active || !xr.scope.active ||
        !tsfp_stereo_scope_lens(xr.scope.weapon, geometry, &body_offset))
        return 0;
    XrPosef anchor = xr.anchor;
    XrVector3f_Add(&anchor.position, &anchor.position, &body_offset);
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        const XrMatrix4x4f clip = tsfp_eye_clip_for_api(&anchor, &xr.eyes[eye],
                                                      xr.world_near, xr.world_far, GRAPHICS_D3D);
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned col = 0; col < 4; ++col)
                rows[eye * 16 + row * 4 + col] = clip.m[col * 4 + row];
    }
    return 1;
}

int tsfp_xr_visibility_matrix(const float game_view[16], const float game_lens[16], float result[16], int objects)
{
    if (!tsfp_xr_avatar_ready())
        return 0;
    XrMatrix4x4f view, lens, visibility, clip;
    memcpy(view.m, game_view, sizeof view.m);
    memcpy(lens.m, game_lens, sizeof lens.m);
    float near_z, far_z;
    if (!tsfp_xbox_camera_planes(&lens, &near_z, &far_z))
        return 0;
    near_z = fminf(near_z, 0.06f);
    const XrPosef head = { head_orientation(), head_position() };
    XrView views[TSFP_XR_EYES + 1] = {xr.eyes[LEFT], xr.eyes[RIGHT]};
    unsigned count = TSFP_XR_EYES;
    if (scope_view(&views[count]) &&
        tsfp_stereo_visibility_clip(&visibility, &xr.anchor, &head, views, count + 1, near_z, far_z))
        ++count;
    else if (!tsfp_stereo_visibility_clip(&visibility, &xr.anchor, &head, views, count, near_z, far_z))
        return 0;
    XrMatrix4x4f_Multiply(&clip, &visibility, &view);
    memcpy(result, clip.m, sizeof clip.m);
    if (objects) {
        xr.object_views = count;
        for (unsigned eye = 0; eye < count; ++eye) {
            const XrMatrix4x4f eye_clip = tsfp_eye_clip_for_api(&xr.anchor, &views[eye], near_z, far_z, GRAPHICS_D3D);
            XrMatrix4x4f_Multiply(&clip, &eye_clip, &view);
            tsfp_clip_planes(xr.object_planes[eye], &clip);
        }
    }
    return 1;
}

int tsfp_xr_object_outside(const float minimum[3], const float maximum[3])
{
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!(isfinite(minimum[axis]) && isfinite(maximum[axis]) && minimum[axis] <= maximum[axis]))
            return 0;
    /* Leave room for animated vertices and game-side quantization. */
    const float margin = 0.10f;
    const XrVector3f lo = { minimum[0] - margin, minimum[1] - margin, minimum[2] - margin };
    const XrVector3f hi = { maximum[0] + margin, maximum[1] + margin, maximum[2] + margin };
    for (unsigned eye = 0; eye < xr.object_views; ++eye)
        if (!tsfp_box_outside_frustum(xr.object_planes[eye], lo, hi))
            return 0;
    return xr.object_views != 0;
}

int tsfp_xr_weapon_matrices(const float projection[16], float rows[32])
{
    if (!xr.world_active)
        return 0;
    XrPosef head = { .position = head_position(), .orientation = head_orientation() };
    /* Give the forearms more clearance from the eyes. Apply the same
     * translation to the model before each eye projects it. */
    const XrVector3f offset = { .y = -0.04f, .z = -0.10f };
    XrVector3f model_position;
    XrPosef_TransformVector3f(&model_position, &head, &offset);
    head.position = model_position;
    XrMatrix4x4f game;
    memcpy(game.m, projection, sizeof game.m);
    /* The original gun sits roughly 0.1 units from its camera. At world
     * scale it spans too much disparity for comfortable binocular fusion. */
    float near_z, far_z;
    return tsfp_xbox_camera_planes(&game, &near_z, &far_z) &&
        eye_rows(rows, &game, &head, 4.0f, near_z * 4.0f, far_z * 4.0f);
}

static XrPosef touch_aim_pose(unsigned hand)
{
    XrPosef pose = xr.touch_pose[TOUCH_GRIP][hand];
    pose.orientation = xr.touch_valid & (1u << (TOUCH_AIM * HANDS + hand)) ?
        xr.touch_pose[TOUCH_AIM][hand].orientation : tsfp_touch_aim_from_grip(pose.orientation, hand);
    return pose;
}

int tsfp_xr_hand_pose(unsigned hand, XrPosef *anchor, XrPosef *pose)
{
    /* Weapon simulation may run after xrEndFrame and before the next sample. */
    if (!tsfp_xr_touch_tracked(hand))
        return 0;
    *anchor = xr.anchor;
    *pose = touch_aim_pose(hand);
    return 1;
}

unsigned tsfp_xr_avatar_poses(XrPosef *anchor, XrVector3f *body_offset,
                             XrPosef hands[TSFP_AVATAR_HANDS])
{
    if (!tsfp_xr_avatar_ready())
        return 0;
    *anchor = xr.anchor;
    const XrPosef head = { xr.eyes[LEFT].pose.orientation, head_position() };
    *body_offset = tsfp_avatar_room_offset(tsfp_head_pivot(anchor), tsfp_head_pivot(&head));
    unsigned tracked = 0;
    for (unsigned hand = 0; hand < HANDS; ++hand) {
        if (!(xr.touch_valid & (1u << (TOUCH_GRIP * HANDS + hand))))
            continue;
        tracked |= 1u << hand;
        hands[hand] = touch_aim_pose(hand);
    }
    return tracked;
}

int tsfp_xr_avatar_ready(void)
{
    return geometry_requested && !geometry_disabled && xr.frame_open && xr.eyes_valid;
}

int tsfp_xr_touch_tracked(unsigned hand)
{
    return geometry_requested && !geometry_disabled && xr.state == XR_SESSION_STATE_FOCUSED &&
        hand < HANDS && (xr.touch_valid & (1u << (TOUCH_GRIP * HANDS + hand)));
}

int tsfp_xr_uplink_trigger(unsigned hand)
{
    RecompPadState state;
    return tsfp_xr_read_pad(0, &state) &&
        (state.right_trigger > 96 || (hand == LEFT && state.left_trigger > 96));
}

int tsfp_xr_uplink_clip(float rows[32])
{
    if (!xr.world_active)
        return 0;
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        const XrMatrix4x4f clip = tsfp_eye_clip_for_api(&xr.anchor, &xr.eyes[eye],
                                                      xr.world_near, xr.world_far, GRAPHICS_D3D);
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned col = 0; col < 4; ++col)
                rows[eye * 16 + row * 4 + col] = clip.m[col * 4 + row];
    }
    return 1;
}

int tsfp_xr_body_matrices(XrVector3f body_offset, float rows[32])
{
    XrMatrix4x4f game;
    if (!xr.world_active || !tsfp_stereo_world_projection(game.m))
        return 0;
    XrPosef body = xr.anchor;
    XrVector3f_Add(&body.position, &body.position, &body_offset);
    return eye_rows(rows, &game, &body, 1.0f, xr.world_near, xr.world_far);
}

/* The guest HUD keeps its screen layout on a panel that follows the head.
 * Screen X spans 64 degrees; Y keeps the 4:3 layout of the Xbox HUD. */
int tsfp_xr_hud_matrices(float rows[32])
{
    if (!xr.world_active)
        return 0;
    const float distance = 1.5f, half_width = 0.625f; /* tan(32 degrees) */
    const float half_height = half_width * 0.75f;
    /* Clip (x, y, z, w) to the panel point (x/w, y/w) at -distance. */
    XrMatrix4x4f panel_from_clip = {{0}};
    panel_from_clip.m[0] = distance * half_width;
    panel_from_clip.m[5] = distance * half_height;
    panel_from_clip.m[14] = -distance;
    panel_from_clip.m[15] = 1.0f;
    const XrPosef head = { head_orientation(), head_position() };
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        const XrMatrix4x4f eye_from_head = tsfp_eye_clip_for_api(&head, &xr.eyes[eye], 0.1f, 100.0f, GRAPHICS_D3D);
        XrMatrix4x4f transform;
        XrMatrix4x4f_Multiply(&transform, &eye_from_head, &panel_from_clip);
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c)
                rows[eye * 16 + r * 4 + c] = transform.m[c * 4 + r];
    }
    return 1;
}

int tsfp_xr_turret_reticle_matrices(float rows[32])
{
    XrMatrix4x4f game;
    if (!xr.world_active || !tsfp_stereo_world_projection(game.m))
        return 0;
    XrVector2f center;
    const XrVector2f *aim = tsfp_stereo_mech_reticle_center(&center) ? &center : NULL;
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        const XrMatrix4x4f transform = tsfp_turret_reticle_transform(&game, &xr.anchor, &xr.eyes[eye], aim);
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c)
                rows[eye * 16 + r * 4 + c] = transform.m[c * 4 + r];
    }
    return 1;
}

static int moved_body_rows(float rows[32], const XrMatrix4x4f *game, XrVector3f body_offset,
                           const XrMatrix4x4f *camera_move)
{
    XrMatrix4x4f camera_from_clip, moved;
    XrMatrix4x4f_Invert(&camera_from_clip, game);
    XrMatrix4x4f_Multiply(&moved, camera_move, &camera_from_clip);
    XrPosef body = xr.anchor;
    XrVector3f_Add(&body.position, &body.position, &body_offset);
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        const XrMatrix4x4f eye_from_body = tsfp_eye_clip_for_api(&body, &xr.eyes[eye],
                                                               xr.world_near, xr.world_far, GRAPHICS_D3D);
        XrMatrix4x4f transform;
        XrMatrix4x4f_Multiply(&transform, &eye_from_body, &moved);
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c)
                rows[eye * 16 + r * 4 + c] = transform.m[c * 4 + r];
    }
    return 1;
}

XrVector3f tsfp_xr_body_eye_position(XrVector3f body_offset)
{
    XrPosef body = xr.anchor, camera_from_local;
    XrVector3f_Add(&body.position, &body.position, &body_offset);
    XrPosef_Invert(&camera_from_local, &body);
    const XrVector3f head = head_position();
    XrVector3f eyes;
    XrPosef_TransformVector3f(&eyes, &camera_from_local, &head);
    return eyes;
}

int tsfp_xr_held_matrices(XrVector3f body_offset, const float camera_correction[16], float rows[32])
{
    XrMatrix4x4f game, move;
    if (!xr.world_active || !tsfp_stereo_world_projection(game.m))
        return 0;
    memcpy(move.m, camera_correction, sizeof move.m);
    return moved_body_rows(rows, &game, body_offset, &move);
}

int tsfp_xr_tracked_matrices(const float projection[16], TsfpModelRole role, float rows[32])
{
    if (!xr.world_active)
        return 0;
    XrMatrix4x4f game;
    memcpy(game.m, projection, sizeof game.m);
    if (role == TSFP_MODEL_MUZZLE_PARTICLES) {
        float offset, scale;
        if (!tsfp_stereo_weapon_depth_range(&offset, &scale))
            return 0;
        tsfp_xbox_pack_depth(&game, offset, scale);
        return eye_rows(rows, &game, &xr.anchor, 1.0f, xr.world_near, xr.world_far);
    }
    if (role == TSFP_MODEL_LEFT_SHIELD || role == TSFP_MODEL_RIGHT_SHIELD ||
        role == TSFP_MODEL_LEFT_WEAPON_PARTICLES || role == TSFP_MODEL_RIGHT_WEAPON_PARTICLES ||
        role == TSFP_MODEL_UPLINK_MAP) {
        float offset, scale;
        XrVector3f body_offset;
        XrMatrix4x4f camera_move;
        if (!tsfp_stereo_weapon_depth_range(&offset, &scale) ||
            !tsfp_stereo_weapon_effect_attachment(role, &body_offset, &camera_move))
            return 0;
        tsfp_xbox_pack_depth(&game, offset, scale);
        return moved_body_rows(rows, &game, body_offset, &camera_move);
    }
    if (role == TSFP_MODEL_LEFT_WEAPON || role == TSFP_MODEL_RIGHT_WEAPON) {
        float offset, scale;
        if (!tsfp_stereo_weapon_depth_range(&offset, &scale))
            return 0;
        tsfp_xbox_pack_depth(&game, offset, scale);
        XrVector3f body_offset;
        XrMatrix4x4f camera_move;
        if (tsfp_stereo_weapon_attachment(role, &body_offset, &camera_move))
            return moved_body_rows(rows, &game, body_offset, &camera_move);
    }
    XrPosef model;
    return tsfp_stereo_weapon_pose(role, &model) &&
        eye_rows(rows, &game, &model, 1.0f, xr.world_near, xr.world_far);
}

static XrDuration stereo_image_wait(XrDuration display_period)
{
    /* Give the compositor one display interval to return a buffer even
     * when a previous pair exists. An already writable image returns at once. */
    return display_period > 0 ? SDL_min(display_period, 20000000) : 2000000;
}

VkImage tsfp_xr_begin_frame(int geometry_ready)
{
    pthread_mutex_lock(&frame_lock);
    const int opened = open_frame(0);
    pthread_mutex_unlock(&frame_lock);
    if (!opened)
        return VK_NULL_HANDLE;
    tsfp_stereo_probe_frame(xr.session, xr.local, xr.frame.predictedDisplayTime,
                            xr.frame.shouldRender && xr.positioned ? &xr.anchor : NULL,
                            xr.eyes_valid ? xr.eyes : NULL, xr.frames);
    if (!xr.frame.shouldRender)
        return VK_NULL_HANDLE;
    const int fresh_stereo = geometry_ready && xr.world_active;
    const int cached_stereo = xr.world_scene >= 0 && tsfp_projection_has_geometry();
    const int stereo_scene = xr.world_scene >= 0 && (fresh_stereo || xr.stereo_committed);
    const XrDuration wait_budget = fresh_stereo ? stereo_image_wait(xr.frame.predictedDisplayPeriod) : 0;
#ifndef TSFP_RELEASE
    stereo_diagnostic.source_ready = geometry_ready;
#endif
    if (xr.eyes_valid && (projection_requested || fresh_stereo)) {
#ifndef TSFP_RELEASE
        stereo_diagnostic.wait_budget = wait_budget;
#endif
        tsfp_projection_begin(xr.local, &xr.anchor, xr.eyes, wait_budget);
    }
    if (geometry_ready && tsfp_xr_geometry_image())
        return VK_NULL_HANDLE;
    /* Gameplay can miss the stereo render pass altogether. Keep the last
     * complete pair from this scene instead of showing the mono back buffer. */
    if (cached_stereo) {
        xr.reuse_world = 1;
        return VK_NULL_HANDLE;
    }
    if (stereo_scene)
        return VK_NULL_HANDLE;

    XrSwapchainImageAcquireInfo acquire = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo image_wait = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
    uint32_t index;
    if (!CHECK(xrAcquireSwapchainImage(xr.swapchain, &acquire, &index)) ||
        !CHECK(xrWaitSwapchainImage(xr.swapchain, &image_wait)))
        return VK_NULL_HANDLE;
    xr.acquired = 1;
    return xr.images[index].image;
}

VkImage tsfp_xr_geometry_image(void)
{
    return xr.world_active ? tsfp_projection_geometry_image() : VK_NULL_HANDLE;
}

void tsfp_xr_geometry_submitted(void)
{
    tsfp_projection_geometry_submitted();
#ifndef TSFP_RELEASE
    stereo_diagnostic.copy_recorded = 1;
#endif
}

void tsfp_xr_draw_projection(VkCommandBuffer commands)
{
    if (projection_requested)
        tsfp_projection_draw(commands);
}

#ifndef TSFP_RELEASE
static void report_stereo_frame(int projection, unsigned layers, XrResult submitted)
{
    if (xr.world_scene < 0 || !xr.frame.shouldRender)
        return;
    const TsfpProjectionDiagnostic image = tsfp_projection_diagnostic();
    if (XR_SUCCEEDED(submitted) && projection && stereo_diagnostic.copy_recorded && !xr.reuse_world) {
        if (image.wait_ns >= 1000000u &&
            (++stereo_diagnostic.waited_for_fresh <= 12u || stereo_diagnostic.waited_for_fresh % 120u == 0))
            fprintf(stderr, "[XR-STEREO] Frame %u scene=%d fresh image after %.2fms wait (budget %.2fms early=%d)\n",
                    stereo_diagnostic.frame, xr.world_scene, image.wait_ns / 1e6,
                    stereo_diagnostic.wait_budget / 1e6, stereo_diagnostic.early_acquire);
        if (stereo_diagnostic.missed) {
            fprintf(stderr, "[XR-STEREO] Frame %u scene=%d fresh stereo restored after %u frames\n",
                    stereo_diagnostic.frame, xr.world_scene, stereo_diagnostic.missed);
            stereo_diagnostic.missed = 0;
        }
        return;
    }
    const uint32_t missed = ++stereo_diagnostic.missed;
    if (missed > 8u && missed % 120u != 0)
        return;

    const char *outcome = "no-layer";
    if (XR_FAILED(submitted))
        outcome = "submit-error";
    else if (projection)
        outcome = xr.reuse_world ? "reused" : "projection";
    else if (layers)
        outcome = "mono";
    fprintf(stderr, "[XR-STEREO] Frame %u scene=%d session=%d gap=%u outcome=%s "
            "eyes=%d matrices=%d source=%d copy=%d early=%d image=%s xrResult=%d wait=%.2fms budget=%.2fms endFrame=%d\n",
            stereo_diagnostic.frame, xr.world_scene, xr.state, missed, outcome,
            xr.eyes_valid, xr.world_active, stereo_diagnostic.source_ready, stereo_diagnostic.copy_recorded,
            stereo_diagnostic.early_acquire, image.stage, image.result, image.wait_ns / 1e6,
            stereo_diagnostic.wait_budget / 1e6, submitted);
}
#endif

int tsfp_xr_end_frame(void)
{
    pthread_mutex_lock(&frame_lock);
    if (!xr.frame_open) {
        pthread_mutex_unlock(&frame_lock);
        return 1;
    }
    const XrCompositionLayerProjection *projection = tsfp_projection_end(xr.reuse_world);
    const XrCompositionLayerBaseHeader *layers[2];
    XrFrameEndInfo end = { .type = XR_TYPE_FRAME_END_INFO, .displayTime = xr.frame.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
    if (projection)
        layers[end.layerCount++] = (const XrCompositionLayerBaseHeader *)projection;
    if (xr.acquired) {
        XrSwapchainImageReleaseInfo release = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        if (CHECK(xrReleaseSwapchainImage(xr.swapchain, &release)) && (!projection || !xr.world_active))
            layers[end.layerCount++] = (const XrCompositionLayerBaseHeader *)&xr.screen;
    }
    if (end.layerCount)
        end.layers = layers;
    xr.acquired = 0;
    xr.frame_open = 0;
    const XrResult submitted = xrEndFrame(xr.session, &end);
#ifndef TSFP_RELEASE
    report_stereo_frame(projection != NULL, end.layerCount, submitted);
#endif
    if (!CHECK(submitted)) {
        if (projection && end.layerCount) {
            projection_requested = 0;
            geometry_disabled = 1;
            fprintf(stderr, "[M4] Projection submission failed, returning to the game quad\n");
        }
        pthread_mutex_unlock(&frame_lock);
        return -1;
    }
    if (projection && xr.world_active && !xr.reuse_world && xr.world_scene >= 0)
        xr.stereo_committed = 1;
    if (projection && xr.reuse_world) {
        static unsigned reused_frames;
        if (++reused_frames == 1 || reused_frames % 300 == 0)
            fprintf(stderr, "[M4] Reused the last complete stereo pair (%u frames)\n", reused_frames);
    } else if (projection && end.layerCount) {
        xr.early_frames += xr.early_frame;
        if (++xr.projection_frames == 1 || xr.projection_frames % 300 == 0)
            fprintf(stderr, "[M4] Submitted two eyes %u frames, %u poses before geometry; last pose age %.2f ms\n",
                    xr.projection_frames, xr.early_frames, (SDL_GetTicksNS() - xr.pose_ticks) / 1e6);
    }
    if (end.layerCount && (++xr.frames == 1 || xr.frames % 300 == 0))
        fprintf(stderr, "[XR] Submitted %s %u (%u layers)\n",
                projection && (xr.world_active || xr.reuse_world) ? "game geometry" : "game quad",
                xr.frames, end.layerCount);
#ifndef TSFP_RELEASE
    if (end.layerCount) {
        static int last_scene = -1, last_stereo = -1;
        static unsigned transitions;
        const int stereo = projection && (xr.world_active || xr.reuse_world);
        if (last_scene != xr.world_scene || last_stereo != stereo) {
            if (transitions < 120u) {
                ++transitions;
                fprintf(stderr, "[XR-FRAME] Present %u scene=%d stereo=%d reuse=%d layers=%u\n",
                        xr.frames, xr.world_scene, stereo, xr.reuse_world, end.layerCount);
            }
            last_scene = xr.world_scene;
            last_stereo = stereo;
        }
    }
#endif
    pthread_mutex_unlock(&frame_lock);
    return end.layerCount ? 0 : 1;
}

void tsfp_xr_destroy(void)
{
    atomic_store_explicit(&scene_thread, 0, memory_order_relaxed);
    pthread_mutex_lock(&frame_lock);
    if (xr.frame_open) {
        XrFrameEndInfo end = { .type = XR_TYPE_FRAME_END_INFO,
            .displayTime = xr.frame.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE };
        CHECK(xrEndFrame(xr.session, &end));
    }
    tsfp_projection_destroy();
    if (xr.swapchain) xrDestroySwapchain(xr.swapchain);
    for (int kind = 0; kind < TOUCH_POSES; ++kind)
        for (int hand = 0; hand < HANDS; ++hand)
            if (xr.touch_space[kind][hand])
                xrDestroySpace(xr.touch_space[kind][hand]);
    if (xr.view) xrDestroySpace(xr.view);
    if (xr.local) xrDestroySpace(xr.local);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.controls) xrDestroyActionSet(xr.controls);
    if (xr.instance) xrDestroyInstance(xr.instance);
    free(xr.images);
    memset(&xr, 0, sizeof xr);
#ifndef TSFP_RELEASE
    memset(&stereo_diagnostic, 0, sizeof stereo_diagnostic);
#endif
    pthread_mutex_unlock(&frame_lock);
    publish_pad((RecompPadState){0}, 0);
}
