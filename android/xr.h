#ifndef TSFP_XR_H
#define TSFP_XR_H

#include <vulkan/vulkan.h>
#include <stdint.h>

void tsfp_xr_enable(const char *capture_prefix, const char *probe, const char *cache,
                    const char *hand_calibration,
                    unsigned msaa, unsigned anisotropy);
int tsfp_xr_enabled(void);
struct RecompPadState;
int tsfp_xr_read_pad(int slot, struct RecompPadState *out);
int16_t tsfp_xr_cart_turn_axis(void);
int tsfp_xr_snap_turn_pending(void);
void tsfp_xr_set_snap_degrees(int degrees);
int tsfp_xr_geometry_enabled(void);
unsigned tsfp_xr_msaa(void);
unsigned tsfp_xr_anisotropy(void);
int tsfp_xr_touch_tracked(unsigned hand);
int tsfp_xr_uplink_trigger(unsigned hand);
int tsfp_xr_uplink_clip(float rows[32]);
struct TsfpScopeGeometry;
int tsfp_xr_scope_matrices(float rows[16]);
int tsfp_xr_scope_lens(struct TsfpScopeGeometry *geometry, float rows[32]);
VkExtent2D tsfp_xr_geometry_extent(void);
struct RhiImage;
void tsfp_xr_capture_frame(struct RhiImage *world);
int tsfp_xr_avatar_capture_begin(struct RhiImage *world);
void tsfp_xr_avatar_capture_end(struct RhiImage *world, int mode);
void tsfp_xr_shield_capture(struct RhiImage *world, unsigned stage);
int tsfp_xr_create_instance(PFN_vkGetInstanceProcAddr get_proc, const VkInstanceCreateInfo *info,
                            VkInstance *instance);
int tsfp_xr_physical_device(VkInstance instance, VkPhysicalDevice *physical);
int tsfp_xr_create_device(PFN_vkGetInstanceProcAddr get_proc, VkPhysicalDevice physical,
                          const VkDeviceCreateInfo *info, VkDevice *device);
int tsfp_xr_create_screen(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          uint32_t family, uint32_t width, uint32_t height, VkFormat *format);
void tsfp_xr_prepare_scene_frame(void);
VkImage tsfp_xr_begin_frame(int geometry_ready);
void tsfp_xr_draw_projection(VkCommandBuffer commands);
int tsfp_xr_world_matrices(float rows[32]);
void tsfp_xr_ghost_camera(const float game_view[16], float eyes[2][3][16]);
int tsfp_xr_visibility_matrix(const float game_view[16], const float game_lens[16], float result[16], int objects);
int tsfp_xr_object_outside(const float minimum[3], const float maximum[3]);
int tsfp_xr_world_active(void);
int tsfp_xr_weapon_matrices(const float projection[16], float rows[32]);
VkImage tsfp_xr_geometry_image(void);
void tsfp_xr_geometry_submitted(void);
int tsfp_xr_end_frame(void);
void tsfp_xr_add_gpu_wait(uint64_t elapsed);
void tsfp_xr_report_frame_timing(void);
void tsfp_xr_destroy(void);

#endif
