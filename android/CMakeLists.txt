cmake_minimum_required(VERSION 3.22)
project(tsfp_quest C CXX)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Configure with the Android NDK and ANDROID_ABI=arm64-v8a")
endif()

set(TSFP_PC "${CMAKE_CURRENT_SOURCE_DIR}/../external/tsfp-pc" CACHE PATH "Pinned PC port checkout")
set(TSFP_SHARED_SOURCE "${TSFP_PC}/src")
set(XBOX_RUNTIME "${TSFP_PC}/external/ts3FPrecomp")
set(TITLE "${XBOX_RUNTIME}/titles/futureperfect/src")
if(NOT EXISTS "${TITLE}/recomp/gen/recomp_funcs.h")
    message(FATAL_ERROR "Generate the game sources with the pinned PC port before configuring Android")
endif()

file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS "${TITLE}/recomp/gen/*.c")
add_library(tsfp_guest OBJECT ${GUEST_SOURCES})
set_target_properties(tsfp_guest PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
option(TSFP_OPTIMIZE_GUEST "Optimize the generated ARM64 guest code" OFF)
if(TSFP_OPTIMIZE_GUEST)
    # The ARM64 MMIO fault handler decodes scalar loads and stores.
    target_compile_options(tsfp_guest PRIVATE -O2 -fno-vectorize -fno-slp-vectorize)
else()
    target_compile_options(tsfp_guest PRIVATE -O0)
endif()
target_compile_definitions(tsfp_guest PRIVATE
    TSFP_RECORD_STATUS_FLAGS=1 RECOMP_NUMERIC_POLICY_HEADER="pc_native_numeric.h")
target_include_directories(tsfp_guest PRIVATE
    "${TITLE}" "${TITLE}/recomp" "${XBOX_RUNTIME}/include"
    "${XBOX_RUNTIME}/src" "${XBOX_RUNTIME}/src/kernel"
    "${TSFP_SHARED_SOURCE}/runtime" "${TSFP_SHARED_SOURCE}/gameplay"
    "${TSFP_SHARED_SOURCE}/graphics")

option(TSFP_BUILD_RUNTIME "Build the Android boot runtime" ON)
option(TSFP_RELEASE "Disable Quest diagnostic instrumentation" OFF)
if(TSFP_BUILD_RUNTIME)
    set_property(SOURCE "${TITLE}/recomp/gen/recomp_0002.c" "${TITLE}/recomp/gen/recomp_0003.c" "${TITLE}/recomp/gen/recomp_0004.c"
        "${TITLE}/recomp/gen/recomp_0005.c" "${TITLE}/recomp/gen/recomp_0009.c" APPEND PROPERTY COMPILE_OPTIONS
        -include "${CMAKE_CURRENT_SOURCE_DIR}/quest_guest_hooks.h")
    set(TSFP_QUEST_BOOT ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS TSFP_RECORD_STATUS_FLAGS=1)
    foreach(group diagnostics gameplay graphics input interface network runtime storage textures)
        set_property(DIRECTORY APPEND PROPERTY INCLUDE_DIRECTORIES "${TSFP_SHARED_SOURCE}/${group}")
    endforeach()
    add_subdirectory("${XBOX_RUNTIME}" xbox-runtime EXCLUDE_FROM_ALL)
    set(VK_DEPS "${XBOX_RUNTIME}/third_party")
    set(SHADERC_SOURCE "${ANDROID_NDK}/sources/third_party/shaderc")
    set(SHADERC_ARCHIVE "${CMAKE_BINARY_DIR}/../shaderc/libs/c++_static/arm64-v8a/libshaderc.a")
    set(XR_API "${CMAKE_BINARY_DIR}/../openxr-api" CACHE PATH "Built OpenXR loader and headers")
    add_library(OpenXR::loader SHARED IMPORTED)
    set_target_properties(OpenXR::loader PROPERTIES
        IMPORTED_LOCATION "${XR_API}/libopenxr_loader.so")
    if(NOT EXISTS "${SHADERC_ARCHIVE}")
        message(FATAL_ERROR "Build the NDK Shaderc archive with android/build.sh first")
    endif()
    target_sources(xbox_d3d8 PRIVATE
        "${XBOX_RUNTIME}/src/d3d/rhi_vulkan.c"
        "${XBOX_RUNTIME}/src/d3d/rhi_vulkan_vma.cpp"
        "${VK_DEPS}/volk/volk.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaderc_vulkan.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/projection_probe.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/stereo_probe.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/sniper_scope_render.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/uplink_render.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/xr.c")
    target_include_directories(xbox_d3d8 PRIVATE
        "${VK_DEPS}/Vulkan-Headers/include" "${VK_DEPS}/volk"
        "${VK_DEPS}/VulkanMemoryAllocator/include"
        "${XBOX_RUNTIME}/src/kernel"
        "${XR_API}/include" "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../external/openxr-sdk-source/src/common"
        "${SHADERC_SOURCE}/libshaderc/include"
        "${SHADERC_SOURCE}/third_party/spirv-tools/external/spirv-headers/include")
    target_compile_definitions(xbox_d3d8 PRIVATE
        RHI_HAVE_VULKAN RHI_VULKAN_SDL VK_NO_PROTOTYPES TSFP_OPENXR)
    target_include_directories(xbox_d3d8 PRIVATE "${XBOX_RUNTIME}/src/input")
    target_compile_definitions(xbox_input PRIVATE TSFP_OPENXR)
    set_target_properties(xbox_d3d8 PROPERTIES CXX_STANDARD 17)
    target_link_libraries(xbox_d3d8 PRIVATE "${SHADERC_ARCHIVE}" OpenXR::loader dl)
    target_sources(xbox_hle PRIVATE "${TSFP_SHARED_SOURCE}/native/audio_sdl.c")
    target_include_directories(xbox_hle PRIVATE "${TSFP_SHARED_SOURCE}")
    target_link_libraries(xbox_hle PRIVATE xbox_video)
    foreach(target xbox_d3d8 xbox_kernel xbox_hle)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    endforeach()
    if(TSFP_RELEASE)
        target_compile_definitions(xbox_d3d8 PRIVATE TSFP_RELEASE)
        target_compile_definitions(xbox_kernel PRIVATE TSFP_RELEASE)
    else()
        foreach(target xbox_d3d8 xbox_kernel xbox_hle)
            target_compile_definitions(${target} PRIVATE TSFP_QUEST_HITCH_TRACE)
        endforeach()
    endif()

    add_library(tsfp_boot SHARED
        boot.c mmio_android.c renderer_android.c quest_guest_hooks.c vr_options.c
        "${TITLE}/recomp_manual.c"
        "${TSFP_SHARED_SOURCE}/gameplay/pc_model_transform_guest.c")
    target_include_directories(tsfp_boot PRIVATE
        "${TITLE}" "${TITLE}/recomp" "${XBOX_RUNTIME}/include"
        "${TSFP_SHARED_SOURCE}/native" "${TSFP_SHARED_SOURCE}"
        "${VK_DEPS}/Vulkan-Headers/include" "${XR_API}/include")
    target_compile_definitions(tsfp_boot PRIVATE
        TSFP_RECORD_STATUS_FLAGS=1 RECOMP_NUMERIC_POLICY_HEADER="pc_native_numeric.h"
        TSFP_QUEST_STEREO_PROBE)
    if(TSFP_RELEASE)
        target_compile_definitions(tsfp_boot PRIVATE TSFP_RELEASE)
        target_link_options(tsfp_boot PRIVATE -Wl,--strip-debug)
    endif()
    target_link_libraries(tsfp_boot PRIVATE tsfp_guest xboxrecomp log m)

    add_executable(tsfp_mmio_test EXCLUDE_FROM_ALL
        "${TSFP_PC}/tests/gameplay/native_mmio.c" mmio_android.c)
    target_include_directories(tsfp_mmio_test PRIVATE "${TSFP_SHARED_SOURCE}/native")
    target_compile_definitions(tsfp_mmio_test PRIVATE tsfp_native_mmio=tsfp_android_mmio)
    target_link_libraries(tsfp_mmio_test PRIVATE platform m)
endif()
