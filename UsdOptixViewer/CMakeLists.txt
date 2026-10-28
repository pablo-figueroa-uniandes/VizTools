cmake_minimum_required(VERSION 3.24)
project(UsdOptixViewer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Dependencies, installed for all users in /opt/viztools (and CUDA in
# /usr/local/cuda-13.2). Locations can be changed through CMAKE_PREFIX_PATH or
# the variables / environment variables below (see /etc/profile.d/viztools.sh).
# ---------------------------------------------------------------------------
set(VIZ_PREFIX "/opt/viztools" CACHE PATH "Root of the installed visualization tools")
if(NOT EXISTS "${VIZ_PREFIX}")
  message(FATAL_ERROR "Visualization tools not found in VIZ_PREFIX=${VIZ_PREFIX}")
endif()
if(NOT DEFINED CUDAToolkit_ROOT)
  if(DEFINED ENV{CUDA_HOME})
    set(CUDAToolkit_ROOT "$ENV{CUDA_HOME}")
  else()
    set(CUDAToolkit_ROOT "/usr/local/cuda-13.2")
  endif()
endif()

set(OptiX_INSTALL_DIR "$ENV{OptiX_INSTALL_DIR}" CACHE PATH "OptiX SDK root (contains include/optix.h)")
if(NOT OptiX_INSTALL_DIR)
  set(OptiX_INSTALL_DIR "${VIZ_PREFIX}/optix-9.1.0" CACHE PATH "" FORCE)
endif()
find_path(OptiX_INCLUDE_DIR optix.h PATHS "${OptiX_INSTALL_DIR}/include" REQUIRED NO_DEFAULT_PATH)

set(IMGUI_DIR "$ENV{IMGUI_DIR}" CACHE PATH "Dear ImGui source directory")
if(NOT IMGUI_DIR)
  set(IMGUI_DIR "${VIZ_PREFIX}/imgui-1.93.0" CACHE PATH "" FORCE)
endif()
if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
  message(FATAL_ERROR "imgui.h not found in IMGUI_DIR=${IMGUI_DIR}")
endif()

# Default for --cloudxr-sdk when CLOUDXR_SDK_DIR is not set at run time
set(CLOUDXR_SDK_DIR "${VIZ_PREFIX}/cloudxr-6.2.1" CACHE PATH "CloudXR SDK (libcloudxr.so, openxr_cloudxr.json)")

list(APPEND CMAKE_PREFIX_PATH "${VIZ_PREFIX}/sdl3" "${VIZ_PREFIX}/usd-26.11" "${VIZ_PREFIX}/meshoptimizer-1.3"
     "${VIZ_PREFIX}/openxr-1.1.58")

find_package(CUDAToolkit 12 REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_package(pxr REQUIRED CONFIG)
find_package(meshoptimizer REQUIRED CONFIG)
# XR output: the OpenXR loader is linked statically. Only the Vulkan headers
# are used at build time; Vulkan itself is loaded at run time from the
# driver's libvulkan.so.1, and the CloudXR runtime from the CloudXR SDK.
find_package(OpenXR REQUIRED CONFIG)
find_package(Vulkan REQUIRED)

# ---------------------------------------------------------------------------
# OptiX device code: compiled to OptiX-IR with nvcc and embedded into the binary.
# ---------------------------------------------------------------------------
set(OPTIX_ARCH "compute_75" CACHE STRING "Virtual architecture used for the OptiX-IR")
set(OPTIXIR_FILE "${CMAKE_CURRENT_BINARY_DIR}/devicePrograms.optixir")
set(OPTIXIR_CPP  "${CMAKE_CURRENT_BINARY_DIR}/devicePrograms_optixir.cpp")
add_custom_command(
  OUTPUT "${OPTIXIR_FILE}"
  COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" -optix-ir -arch=${OPTIX_ARCH} -std=c++17 --use_fast_math -lineinfo
          -diag-suppress 20044 -ccbin "${CMAKE_CXX_COMPILER}"
          -I "${OptiX_INCLUDE_DIR}" -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/devicePrograms.cu" -o "${OPTIXIR_FILE}"
  DEPENDS src/devicePrograms.cu src/LaunchParams.h
  COMMENT "Compiling OptiX programs to OptiX-IR")
add_custom_command(
  OUTPUT "${OPTIXIR_CPP}"
  COMMAND "${CMAKE_COMMAND}" -DINPUT=${OPTIXIR_FILE} -DOUTPUT=${OPTIXIR_CPP} -DSYMBOL=g_devicePrograms
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
  DEPENDS "${OPTIXIR_FILE}" cmake/EmbedFile.cmake
  COMMENT "Embedding OptiX-IR")

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_executable(UsdOptixViewer
  src/main.cpp src/App.cpp src/Scene.cpp src/OptixRenderer.cpp src/StbImage.cpp src/XrOutput.cpp
  src/CloudXrService.cpp "${OPTIXIR_CPP}")
target_include_directories(UsdOptixViewer PRIVATE src ${OptiX_INCLUDE_DIR})
target_link_libraries(UsdOptixViewer PRIVATE
  imgui SDL3::SDL3 meshoptimizer::meshoptimizer usd usdGeom usdShade ar sdf tf gf vt
  CUDA::cudart CUDA::cuda_driver OpenXR::openxr_loader Vulkan::Headers ${CMAKE_DL_LIBS})
target_compile_definitions(UsdOptixViewer PRIVATE VK_NO_PROTOTYPES XR_USE_GRAPHICS_API_VULKAN
  CLOUDXR_SDK_DEFAULT="${CLOUDXR_SDK_DIR}")
# USD headers trigger a few deprecation warnings under C++20
target_compile_options(UsdOptixViewer PRIVATE -Wno-deprecated -Wno-deprecated-declarations)

# ---------------------------------------------------------------------------
# Installation: bin/UsdOptixViewer + sample scenes. RPATH points to the
# dependency libraries so the installed binary runs without LD_LIBRARY_PATH.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
set_target_properties(UsdOptixViewer PROPERTIES
  INSTALL_RPATH_USE_LINK_PATH ON
  INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS UsdOptixViewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/UsdOptixViewer/assets)
