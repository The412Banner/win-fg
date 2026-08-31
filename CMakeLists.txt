cmake_minimum_required(VERSION 3.18)
project(win_fg CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# ---- self-identifying build stamp -------------------------------------------
# CI passes -DWINFG_GIT_DESCRIBE=<git describe>; fall back to a local git describe,
# then to the VERSION file, so every build (CI or local) embeds an identifiable
# string. Recover it from a shipped .so with:
#     strings libwin_fg.so | grep win-fg-build
#     nm -D libwin_fg.so   | grep win_fg_build_version
if(NOT WINFG_GIT_DESCRIBE)
  execute_process(
    COMMAND git describe --tags --always --dirty --long
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE WINFG_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT WINFG_GIT_DESCRIBE)
  file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION _winfg_ver LIMIT_COUNT 1)
  set(WINFG_GIT_DESCRIBE "v${_winfg_ver}-nogit")
endif()
string(TIMESTAMP WINFG_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
message(STATUS "win-fg build stamp: ${WINFG_GIT_DESCRIBE} (${WINFG_BUILD_DATE})")
configure_file(${CMAKE_SOURCE_DIR}/src/version_stamp.cpp.in
               ${CMAKE_BINARY_DIR}/version_stamp.cpp @ONLY)

# VULKAN_INCLUDE points at a Vulkan-Headers include/ (vk_layer.h + vulkan.h).
add_library(win_fg SHARED src/layer.cpp src/framegen.cpp
            ${CMAKE_BINARY_DIR}/version_stamp.cpp)
target_include_directories(win_fg PRIVATE src ${VULKAN_INCLUDE})
target_compile_definitions(win_fg PRIVATE VK_NO_PROTOTYPES=1 VK_USE_PLATFORM_ANDROID_KHR)
set_target_properties(win_fg PROPERTIES OUTPUT_NAME win_fg)
if(ANDROID)
  find_library(log-lib log)
  target_link_libraries(win_fg ${log-lib})
endif()
