cmake_minimum_required(VERSION 3.20)
project(Sonnote LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FindPkgConfig)

find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(JACK REQUIRED jack)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# ---------- ImGui sources ----------
#file(GLOB IMGUI_SRC
#  extern/imgui/imgui.cpp
#  extern/imgui/imgui_draw.cpp
#  extern/imgui/imgui_tables.cpp
#  extern/imgui/imgui_widgets.cpp
#  extern/imgui/imgui_demo.cpp
#  extern/imgui/backends/imgui_impl_sdl2.cpp
#  extern/imgui/backends/imgui_impl_opengl3.cpp
#)

#add_library(imgui STATIC ${IMGUI_SRC})
#target_include_directories(imgui PUBLIC
#  extern/imgui
#  extern/imgui/backends
#)

find_path(LIBPD_INCLUDE_DIR z_libpd.h PATH_SUFFIXES pd libpd)
find_library(LIBPD_LIBRARY NAMES pd libpd)
if (NOT LIBPD_INCLUDE_DIR OR NOT LIBPD_LIBRARY)
  message(FATAL_ERROR "libpd not found (on Ubuntu: sudo apt install libpd-dev)")
endif()

# ---------- SDL2 (config or pkg-config fallback) ----------
# Try the official CMake package first (provides SDL2::SDL2)
find_package(SDL2 QUIET CONFIG)

if (NOT TARGET SDL2::SDL2)
  # Fallback to CMake's FindSDL2 (if present)
  find_package(SDL2 QUIET)
endif()

if (NOT TARGET SDL2::SDL2)
  # Final fallback: pkg-config
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  add_library(SDL2::SDL2 INTERFACE IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${SDL2_LINK_LIBRARIES}"
    INTERFACE_COMPILE_OPTIONS "${SDL2_CFLAGS_OTHER}"
  )
endif()

# Make sure ImGui can see <SDL.h> by inheriting SDL2's include dirs
#imgui
target_link_libraries(SDL2::SDL2)

# ---------- OpenGL ----------
find_package(OpenGL REQUIRED)

# ---------- VST3 hosting (extern/vst3sdk, MIT) ----------
# Just the SDK's hosting sources, not its own CMake project. Needs the
# base, cmake, pluginterfaces and public.sdk submodules.
find_package(X11 REQUIRED)
set(VST3_SDK ${CMAKE_SOURCE_DIR}/extern/vst3sdk)
add_library(vst3_hosting STATIC
  ${VST3_SDK}/pluginterfaces/base/conststringtable.cpp
  ${VST3_SDK}/pluginterfaces/base/coreiids.cpp
  ${VST3_SDK}/pluginterfaces/base/funknown.cpp
  ${VST3_SDK}/pluginterfaces/base/ustring.cpp
  ${VST3_SDK}/public.sdk/source/common/commoniids.cpp
  ${VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp
  ${VST3_SDK}/public.sdk/source/common/memorystream.cpp
  ${VST3_SDK}/public.sdk/source/common/threadchecker_linux.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/eventlist.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/module.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/module_linux.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/parameterchanges.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${VST3_SDK}/public.sdk/source/vst/hosting/processdata.cpp
  ${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp
  ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
)
target_include_directories(vst3_hosting PUBLIC ${VST3_SDK})
target_compile_definitions(vst3_hosting PUBLIC RELEASE=1)
target_link_libraries(vst3_hosting PUBLIC dl)

# ---------- App ----------
add_executable(app
  src/main.cpp
  src/audio_engine.cpp
  src/vulkan_renderer.cpp
  src/text_renderer.cpp
  src/parser.cpp
  src/utils.cpp
  src/pd_host.cpp
  src/vst3_host.cpp
)

target_include_directories(app PRIVATE
  ${JACK_INCLUDE_DIRS}
  ${SNDFILE_INCLUDE_DIRS}
#  extern/msdfgen/core
#  extern/msdfgen/ext
#  extern/msdfgen
#  extern/imgui
#  extern/miniaudio
  ${SDL2_INCLUDE_DIRS}
  ${FREETYPE_INCLUDE_DIRS}
  ${LIBPD_INCLUDE_DIR}
)

target_link_libraries(app PRIVATE
#  imgui
  pthread
#  msdfgen-core
#  msdfgen-ext
  SDL2::SDL2
  OpenGL::GL
  Vulkan::Vulkan
  Freetype::Freetype
  ${JACK_LIBRARIES}
  ${SNDFILE_LIBRARIES}
  ${LIBPD_LIBRARY}
  vst3_hosting
  X11::X11
)

# Some Linux toolchains need these explicitly
if (UNIX AND NOT APPLE)
  target_link_libraries(app PRIVATE dl pthread fftw3f sndfile)
endif()

# Optional: stricter warnings for your code (not third-party)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(app PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Wno-stringop-overflow -g)
endif()

