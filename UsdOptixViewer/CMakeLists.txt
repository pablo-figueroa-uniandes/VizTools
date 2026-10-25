cmake_minimum_required(VERSION 3.24)
project(UsdOptixViewer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Dependencies. Locations can be given through CMAKE_PREFIX_PATH or the
# variables / environment variables below (see /etc/profile.d/viztools.sh).
# Defaults to the all-users install in /opt/viztools, falling back to the
# per-user install in ~/.local/opt.
# ---------------------------------------------------------------------------
if(EXISTS "/opt/viztools")
  set(_vizDefault "/opt/viztools")
else()
  set(_vizDefault "$ENV{HOME}/.local/opt")
endif()
set(VIZ_PREFIX "${_vizDefault}" CACHE PATH "Root of the installed visualization tools")
if(NOT DEFINED CUDAToolkit_ROOT)
  if(DEFINED ENV{CUDA_HOME})
    set(CUDAToolkit_ROOT "$ENV{CUDA_HOME}")
  elseif(EXISTS "/usr/local/cuda-13.2")
    set(CUDAToolkit_ROOT "/usr/local/cuda-13.2")
  else()
    set(CUDAToolkit_ROOT "${VIZ_PREFIX}/cuda-13.2")
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

list(APPEND CMAKE_PREFIX_PATH "${VIZ_PREFIX}/sdl3" "${VIZ_PREFIX}/entt-4.0.0" "${VIZ_PREFIX}/usd-26.11")

find_package(CUDAToolkit 12 REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_package(EnTT REQUIRED CONFIG)
find_package(pxr REQUIRED CONFIG)

# ---------------------------------------------------------------------------
# OptiX device code: compiled to PTX with nvcc and embedded into the binary.
# ---------------------------------------------------------------------------
set(OPTIX_ARCH "compute_75" CACHE STRING "Virtual architecture used for the OptiX PTX")
set(PTX_FILE "${CMAKE_CURRENT_BINARY_DIR}/devicePrograms.ptx")
set(PTX_CPP  "${CMAKE_CURRENT_BINARY_DIR}/devicePrograms_ptx.cpp")
add_custom_command(
  OUTPUT "${PTX_FILE}"
  COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" -ptx -arch=${OPTIX_ARCH} -std=c++17 --use_fast_math -diag-suppress 20044
          -ccbin "${CMAKE_CXX_COMPILER}"
          -I "${OptiX_INCLUDE_DIR}" -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/devicePrograms.cu" -o "${PTX_FILE}"
  DEPENDS src/devicePrograms.cu src/LaunchParams.h
  COMMENT "Compiling OptiX programs to PTX")
add_custom_command(
  OUTPUT "${PTX_CPP}"
  COMMAND "${CMAKE_COMMAND}" -DINPUT=${PTX_FILE} -DOUTPUT=${PTX_CPP} -DSYMBOL=g_devicePrograms
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
  DEPENDS "${PTX_FILE}" cmake/EmbedFile.cmake
  COMMENT "Embedding PTX")

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
  src/main.cpp src/App.cpp src/Scene.cpp src/OptixRenderer.cpp src/StbImage.cpp "${PTX_CPP}")
target_include_directories(UsdOptixViewer PRIVATE src ${OptiX_INCLUDE_DIR})
target_link_libraries(UsdOptixViewer PRIVATE
  imgui SDL3::SDL3 EnTT::EnTT usd usdGeom usdShade ar sdf tf gf vt
  CUDA::cudart CUDA::cuda_driver ${CMAKE_DL_LIBS})
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
