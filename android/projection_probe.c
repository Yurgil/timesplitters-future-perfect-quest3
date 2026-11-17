#include "volk.h"
#include "projection_probe.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>
#include "stereo_math.h"
#include "stereo_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EYE_SIZE = 320, MARKER_RADIUS = 3 };
enum ImageState { IMAGE_FREE, IMAGE_ACQUIRED, IMAGE_WAITED };

static struct {
    VkDevice device;
    XrSwapchain swapchain;
    VkImageView (*images)[TSFP_XR_EYES];
    XrSwapchainImageVulkan2KHR *geometry_images;
    XrExtent2Di size;
    uint32_t count, index;
    enum ImageState state;
    int prepared, drawn, released_geometry;
    XrCompositionLayerProjection layer;
    XrCompositionLayerProjectionView views[TSFP_XR_EYES];
    XrMatrix4x4f clip[TSFP_XR_EYES];
} probe;

void tsfp_projection_destroy(void)
{
    if (probe.images) {
        for (uint32_t i = 0; i < probe.count; ++i)
            for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye)
                if (probe.images[i][eye])
                    vkDestroyImageView(probe.device, probe.images[i][eye], NULL);
        free(probe.images);
    }
    free(probe.geometry_images);
    if (probe.swapchain)
        xrDestroySwapchain(probe.swapchain);
    memset(&probe, 0, sizeof probe);
}

int tsfp_projection_create(XrSession session, VkDevice device, VkFormat format,
                           uint32_t geometry_width, uint32_t geometry_height)
{
    const int geometry = geometry_width && geometry_height;
    const uint32_t width = geometry ? geometry_width : EYE_SIZE;
    const uint32_t height = geometry ? geometry_height : EYE_SIZE;
    XrSwapchainCreateInfo create = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      (geometry ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT : 0),
        .format = format, .sampleCount = 1, .width = width, .height = height,
        .faceCount = 1, .arraySize = TSFP_XR_EYES, .mipCount = 1 };
    if (XR_FAILED(xrCreateSwapchain(session, &create, &probe.swapchain)))
        return 0;

    probe.device = device;
    probe.size = (XrExtent2Di){ width, height };
    uint32_t count = 0;
    XrSwapchainImageVulkan2KHR *images = NULL;
    if (XR_FAILED(xrEnumerateSwapchainImages(probe.swapchain, 0, &count, NULL)) || !count)
        goto failed;
    images = calloc(count, sizeof *images);
    if (!geometry)
        probe.images = calloc(count, sizeof *probe.images);
    if (!images || (!geometry && !probe.images))
        goto failed;
    for (uint32_t i = 0; i < count; ++i)
        images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    if (XR_FAILED(xrEnumerateSwapchainImages(probe.swapchain, count, &probe.count,
                                           (XrSwapchainImageBaseHeader *)images))) {
        probe.count = 0;
        goto failed;
    }
    if (geometry) {
        probe.geometry_images = images;
        images = NULL;
    }
    for (uint32_t i = 0; !geometry && i < probe.count; ++i) {
        for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
            VkImageViewCreateInfo view = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = images[i].image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1 } };
            if (vkCreateImageView(device, &view, NULL, &probe.images[i][eye]) != VK_SUCCESS)
                goto failed;
        }
    }
    free(images);
    fprintf(stderr, "[M4] Projection swapchain %ux%u, %u layers, %u images\n",
            width, height, TSFP_XR_EYES, probe.count);
    return 1;

failed:
    free(images);
    tsfp_projection_destroy();
    return 0;
}

void tsfp_projection_begin(XrSpace local, const XrPosef *anchor,
                           const XrView eyes[TSFP_XR_EYES])
{
    probe.prepared = probe.drawn = 0;
    if (!probe.swapchain || !anchor || !eyes)
        return;
    if (probe.state == IMAGE_FREE) {
        XrSwapchainImageAcquireInfo acquire = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(xrAcquireSwapchainImage(probe.swapchain, &acquire, &probe.index)))
            return;
        probe.state = IMAGE_ACQUIRED;
    }
    if (probe.state == IMAGE_ACQUIRED) {
        XrSwapchainImageWaitInfo wait = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .timeout = 0 };
        const XrResult result = xrWaitSwapchainImage(probe.swapchain, &wait);
        if (XR_FAILED(result) || result == XR_TIMEOUT_EXPIRED)
            return;
        probe.state = IMAGE_WAITED;
    }
    probe.layer = (XrCompositionLayerProjection){ .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .space = local, .viewCount = TSFP_XR_EYES, .views = probe.views };
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        probe.views[eye] = (XrCompositionLayerProjectionView){
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = eyes[eye].pose, .fov = eyes[eye].fov,
            .subImage = { .swapchain = probe.swapchain, .imageRect.extent = probe.size,
                          .imageArrayIndex = eye }
        };
        if (!probe.geometry_images)
            probe.clip[eye] = tsfp_eye_clip(anchor, &eyes[eye], 0.05f, 100.0f);
    }
    probe.prepared = 1;
}

static void draw_marker(VkCommandBuffer commands, const XrMatrix4x4f *clip,
                        XrVector4f point, VkClearColorValue color)
{
    XrVector4f projected;
    XrMatrix4x4f_TransformVector4f(&projected, clip, &point);
    if (!(projected.w > 0.0f && projected.z >= 0.0f && projected.z <= projected.w))
        return;
    const float x = (projected.x / projected.w + 1.0f) * (EYE_SIZE * 0.5f);
    const float y = (projected.y / projected.w + 1.0f) * (EYE_SIZE * 0.5f);
    if (!(x >= MARKER_RADIUS && y >= MARKER_RADIUS &&
          x <= EYE_SIZE - MARKER_RADIUS && y <= EYE_SIZE - MARKER_RADIUS))
        return;
    const VkClearAttachment fill = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .colorAttachment = 0, .clearValue.color = color };
    const VkClearRect rect = { .rect.offset = { (int)x - MARKER_RADIUS, (int)y - MARKER_RADIUS },
        .rect.extent = { 2 * MARKER_RADIUS, 2 * MARKER_RADIUS }, .layerCount = 1 };
    vkCmdClearAttachments(commands, 1, &fill, 1, &rect);
}

void tsfp_projection_draw(VkCommandBuffer commands)
{
    if (!probe.prepared || !probe.images)
        return;
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        VkRenderingAttachmentInfo target = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = probe.images[probe.index][eye], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color.float32 = { 0.004f, 0.006f, 0.01f, 1.0f } };
        VkRenderingInfo render = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea.extent = { EYE_SIZE, EYE_SIZE }, .layerCount = 1,
            .colorAttachmentCount = 1, .pColorAttachments = &target };
        vkCmdBeginRendering(commands, &render);
        draw_marker(commands, &probe.clip[eye], (XrVector4f){ -1.05f, 0.55f, -1.6f, 1.0f },
                    (VkClearColorValue){ .float32 = { 0.2f, 0.6f, 0.4f, 1.0f } });
        draw_marker(commands, &probe.clip[eye], (XrVector4f){ 1.85f, 0.9f, -3.0f, 1.0f },
                    (VkClearColorValue){ .float32 = { 0.6f, 0.42f, 0.2f, 1.0f } });
        vkCmdEndRendering(commands);
    }
    probe.drawn = 1;
}

VkImage tsfp_projection_geometry_image(void)
{
    return probe.prepared && probe.geometry_images ? probe.geometry_images[probe.index].image : VK_NULL_HANDLE;
}

void tsfp_projection_geometry_submitted(void)
{
    probe.drawn = 1;
}

int tsfp_projection_has_geometry(void)
{
    return probe.released_geometry;
}

void tsfp_projection_forget_geometry(void)
{
    probe.released_geometry = 0;
}

const XrCompositionLayerProjection *tsfp_projection_end(int reuse_geometry)
{
    const int drawn = probe.drawn;
    probe.prepared = probe.drawn = 0;
    if (probe.state != IMAGE_WAITED)
        return reuse_geometry && probe.released_geometry ? &probe.layer : NULL;
    XrSwapchainImageReleaseInfo release = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_FAILED(xrReleaseSwapchainImage(probe.swapchain, &release))) {
        probe.released_geometry = 0;
        return NULL;
    }
    probe.state = IMAGE_FREE;
    probe.released_geometry = drawn && probe.geometry_images != NULL;
    return drawn ? &probe.layer : NULL;
}
